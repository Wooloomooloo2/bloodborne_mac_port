// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Syslog severities (RFC 5424 codes) that the game's lines map to.
enum LogSeverity: Int, CaseIterable, Comparable {
    case critical = 2, error = 3, warning = 4, notice = 5, info = 6, debug = 7

    /// In the text log, padded to one width.
    var label: String {
        switch self {
        case .critical: "CRIT"
        case .error: "ERROR"
        case .warning: "WARN"
        case .notice: "NOTICE"
        case .info: "INFO"
        case .debug: "DEBUG"
        }
    }

    /// In the JSON log: ECS `log.level` and Loki's `level` label.
    var keyword: String {
        switch self {
        case .critical: "critical"
        case .error: "error"
        case .warning: "warning"
        case .notice: "notice"
        case .info: "info"
        case .debug: "debug"
        }
    }

    init?(label: String) {
        guard let match = Self.allCases.first(where: { $0.label == label }) else { return nil }
        self = match
    }

    /// More severe first: critical < error < ... (the syslog order).
    static func < (a: LogSeverity, b: LogSeverity) -> Bool { a.rawValue < b.rawValue }
}

/// One line of a session log.
struct LogEntry: Equatable {
    var time: Date
    var severity: LogSeverity
    /// Who wrote it: "runtime", "bbgpu-host", "gpu.render.vulkan", "launcher", ...
    var source: String
    /// The line as the game wrote it.
    var message: String
}

/// The session logs in ~/Library/Logs/Bloodborne: one pair per game session,
/// game-<local start time>.log (text: time, severity, the game's line) and .jsonl (one JSON object
/// per line, for Loki, Elasticsearch, Vector, ...). game.log and game.previous.log link to the last
/// two sessions' text logs.
enum SessionLog {
    static let latestName = "game.log"
    static let previousName = "game.previous.log"
    /// Sessions kept (their .log and .jsonl); older ones are removed when a session starts.
    static let keep = 20
    /// One session's .log and .jsonl together; past it only the last `tailLines` lines are
    /// written, at the end (SessionLogWriter). A normal session is well under 1 MB.
    static let maxSessionBytes = 64 << 20
    static let tailLines = 1_000
    /// All kept sessions together: the oldest are removed past it when a session starts.
    static let maxFolderBytes = 256 << 20

    // MARK: Severity

    /// Severity and source of a line the game wrote. `previous`: the line before, whose severity
    /// and source an indented continuation line (a stack frame, a wrapped list) keeps.
    static func classify(_ line: String, previous: LogEntry? = nil) -> (severity: LogSeverity, source: String) {
        if let previous, line.first == " " || line.first == "\t" {
            return (previous.severity, previous.source)
        }
        if line.hasPrefix("Crash: ") { return (.critical, "crash") }
        if line.hasPrefix("STOP: ") { return (.error, "stop") }
        // The GPU library (shadPS4's logger): "GPU [Render.Vulkan] <Warning> file.cpp:12 Func: text".
        if line.hasPrefix("GPU ["), let close = line.firstIndex(of: "]") {
            let category = line[line.index(line.startIndex, offsetBy: 5)..<close]
            let rest = line[line.index(after: close)...].drop { $0 == " " }
            var severity = LogSeverity.info
            if rest.hasPrefix("<"), let end = rest.firstIndex(of: ">") {
                switch rest[rest.index(after: rest.startIndex)..<end] {
                case "Trace", "Debug": severity = .debug
                case "Warning": severity = .warning
                case "Error": severity = .error
                case "Critical": severity = .critical
                default: severity = .info
                }
            }
            return (severity, "gpu." + category.lowercased())
        }
        if line.hasPrefix("== Stall") { return (.warning, "gpu") }
        // "Component: text", "Runtime ERROR: text", "AUDIT ERROR: text".
        var source = "game"
        var text = Substring(line)
        if let colon = line.firstIndex(of: ":"), line.distance(from: line.startIndex, to: colon) <= 32 {
            let name = line[..<colon]
            if name.first?.isLetter == true,
               name.allSatisfy({ $0.isLetter || $0.isNumber || " ._+-".contains($0) }) {
                var words = name.split(separator: " ")
                if words.last == "ERROR" {
                    words.removeLast()
                    return (.error, words.isEmpty ? "game" : words.joined(separator: "-").lowercased())
                }
                source = words.joined(separator: "-").lowercased()
                text = line[line.index(after: colon)...]
            }
        }
        return (looksLikeWarning(text) ? .warning : .info, source)
    }

    /// "warning" first, or one of a few failure words anywhere (whole words, any case). Plain
    /// string checks, no regular expression: this runs for every line of a long trace.
    private static func looksLikeWarning(_ text: Substring) -> Bool {
        let lower = text.lowercased()
        if lower.drop(while: { $0 == " " }).hasPrefix("warning") { return true }
        for word in ["failed", "cannot", "could not", "unable to"] {
            var from = lower.startIndex
            while let range = lower.range(of: word, range: from..<lower.endIndex) {
                let before = range.lowerBound == lower.startIndex ? nil : lower[lower.index(before: range.lowerBound)]
                let after = range.upperBound == lower.endIndex ? nil : lower[range.upperBound]
                if before?.isLetter != true && after?.isLetter != true { return true }
                from = range.upperBound
            }
        }
        return false
    }

    // MARK: Formats

    /// Text log line: local time with offset (ISO 8601), severity, the game's line.
    /// 2026-10-22T14:03:05.123+03:00 WARN   GPU [Render] <Warning> ...
    static func textLine(_ entry: LogEntry, timeZone: TimeZone = .current) -> String {
        textLine(entry, time: timestamp(entry.time, timeZone: timeZone))
    }

    /// With the time already formatted (localTime(_:): lines that arrived together share it).
    static func textLine(_ entry: LogEntry, time: String) -> String {
        "\(time) \(entry.severity.label.padding(toLength: 6, withPad: " ", startingAt: 0)) \(entry.message)"
    }

    /// The text log's time format; the JSON log's is utcTime(_:).
    static func localTime(_ date: Date) -> String { timestamp(date, timeZone: local.timeZone) }
    static func utcTime(_ date: Date) -> String { timestamp(date, timeZone: utc.timeZone, zulu: true) }

    /// A text log line back to its parts; a line without the prefix (an older log) is all message.
    static func parse(_ line: String) -> (time: Date?, severity: LogSeverity?, message: String) {
        // The timestamp is 29 characters (yyyy-MM-ddTHH:mm:ss.SSS+hh:mm), then the padded label.
        guard line.count >= 37, line.dropFirst(10).first == "T",
              let time = parseTimestamp(String(line.prefix(29))) else { return (nil, nil, line) }
        let rest = line.dropFirst(30)
        guard let severity = LogSeverity(label: String(rest.prefix(6)).trimmingCharacters(in: .whitespaces)) else {
            return (nil, nil, line)
        }
        return (time, severity, String(rest.dropFirst(7)))
    }

    /// The session's fields on every JSON line (Loki labels, Elasticsearch fields).
    struct Session: Equatable {
        /// The log's name without extension, e.g. game-2026-10-22_14-03-05: the session id.
        var id: String
        var appVersion: String
    }

    /// One JSON object (ECS-style dotted names; @timestamp in UTC), keys in sorted order.
    static func jsonLine(_ entry: LogEntry, session: Session) -> String {
        jsonLine(entry, session: session, time: utcTime(entry.time))
    }

    /// Built as text, not with JSONSerialization: this runs for every line of a long trace.
    static func jsonLine(_ entry: LogEntry, session: Session, time: String) -> String {
        "{\"@timestamp\":\"\(time)\",\"log.level\":\"\(entry.severity.keyword)\",\"log.logger\":\(jsonString(entry.source)),"
            + "\"message\":\(jsonString(entry.message)),\"service.name\":\"bloodborne-mac-port\","
            + "\"service.version\":\(jsonString(session.appVersion)),\"session.id\":\(jsonString(session.id)),"
            + "\"syslog.severity.code\":\(entry.severity.rawValue)}"
    }

    /// A JSON string literal (quotes, backslashes and control characters escaped; UTF-8 as is).
    static func jsonString(_ s: String) -> String {
        var out = "\""
        out.reserveCapacity(s.utf8.count + 2)
        for scalar in s.unicodeScalars {
            switch scalar {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            case _ where scalar.value < 0x20: out += String(format: "\\u%04x", scalar.value)
            default: out.unicodeScalars.append(scalar)
            }
        }
        return out + "\""
    }

    // (ISO8601DateFormatter is thread-safe; one per time zone, not one per line.)
    private static let utc = isoFormatter(TimeZone(identifier: "UTC")!)
    private static let local = isoFormatter(.current)

    private static func isoFormatter(_ timeZone: TimeZone) -> ISO8601DateFormatter {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        formatter.timeZone = timeZone
        return formatter
    }

    /// UTC as ...Z (JSON); local time always with its offset, +00:00 included, so every text log
    /// line has the same width.
    private static func timestamp(_ date: Date, timeZone: TimeZone, zulu: Bool = false) -> String {
        let formatter = zulu ? utc : timeZone == local.timeZone ? local : isoFormatter(timeZone)
        let text = formatter.string(from: date)
        return !zulu && text.hasSuffix("Z") ? String(text.dropLast()) + "+00:00" : text
    }

    private static func parseTimestamp(_ text: String) -> Date? { local.date(from: text) }

    // MARK: Files

    /// game-2026-10-22_14-03-05 (local time: what the player sees in Finder).
    static func name(start: Date, timeZone: TimeZone = .current) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = timeZone
        formatter.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        return "game-" + formatter.string(from: start)
    }

    /// The session text logs in `folder`, oldest first.
    static func sessions(in folder: URL) -> [URL] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        return names.filter { $0.hasPrefix("game-") && $0.hasSuffix(".log") }.sorted()
            .map { folder.appendingPathComponent($0) }
    }

    /// Before the first session: logs of earlier versions (game.log and game.previous.log as
    /// files, not links) become dated session logs, named by when they were last written.
    static func migrateOldLogs(in folder: URL) {
        let fm = FileManager.default
        for old in [previousName, latestName] {
            let url = folder.appendingPathComponent(old)
            guard let attributes = try? fm.attributesOfItem(atPath: url.path),
                  attributes[.type] as? FileAttributeType == .typeRegular else { continue }
            let date = attributes[.modificationDate] as? Date ?? Date()
            var target = folder.appendingPathComponent(name(start: date) + ".log")
            if fm.fileExists(atPath: target.path) {
                target = folder.appendingPathComponent(name(start: date) + "-\(old.hasPrefix("game.previous") ? 1 : 2).log")
            }
            try? fm.moveItem(at: url, to: target)
        }
    }

    /// A new session: migrates old logs, removes sessions beyond `keep`, creates the session's
    /// two files and points game.log / game.previous.log at the new and the last text log.
    /// `keep` and `budget` (bytes for the earlier sessions): parameters for the tests.
    static func begin(in folder: URL, start: Date, keep: Int = SessionLog.keep,
                      budget: Int = SessionLog.maxFolderBytes - SessionLog.maxSessionBytes) throws -> (text: URL, json: URL) {
        let fm = FileManager.default
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        migrateOldLogs(in: folder)
        var stem = name(start: start)
        var n = 2
        while fm.fileExists(atPath: folder.appendingPathComponent(stem + ".log").path) {
            stem = name(start: start) + "-\(n)" // two starts in one second
            n += 1
        }
        let text = folder.appendingPathComponent(stem + ".log")
        let json = folder.appendingPathComponent(stem + ".jsonl")
        var earlier = sessions(in: folder)
        func remove(_ old: URL) {
            try? fm.removeItem(at: old)
            try? fm.removeItem(at: old.deletingPathExtension().appendingPathExtension("jsonl"))
        }
        for old in earlier.dropLast(max(0, keep - 1)) { remove(old) }
        earlier = Array(earlier.suffix(max(0, keep - 1)))
        // Then the size budget, oldest first; the new session gets maxSessionBytes of it.
        func size(_ url: URL) -> Int {
            [url, url.deletingPathExtension().appendingPathExtension("jsonl")].reduce(0) {
                $0 + ((try? fm.attributesOfItem(atPath: $1.path))?[.size] as? Int ?? 0)
            }
        }
        var total = earlier.reduce(0) { $0 + size($1) }
        while let oldest = earlier.first, total > budget {
            total -= size(oldest)
            remove(oldest)
            earlier.removeFirst()
        }
        fm.createFile(atPath: text.path, contents: nil)
        fm.createFile(atPath: json.path, contents: nil)
        link(folder.appendingPathComponent(latestName), to: text)
        if let last = earlier.last, fm.fileExists(atPath: last.path) {
            link(folder.appendingPathComponent(previousName), to: last)
        }
        return (text, json)
    }

    /// A relative symbolic link (the folder can move with the home folder).
    private static func link(_ url: URL, to target: URL) {
        try? FileManager.default.removeItem(at: url)
        try? FileManager.default.createSymbolicLink(atPath: url.path, withDestinationPath: target.lastPathComponent)
    }
}
