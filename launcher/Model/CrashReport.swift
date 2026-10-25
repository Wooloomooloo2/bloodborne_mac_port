// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The Mac the report comes from.
struct SystemInfo: Equatable {
    var appVersion: String
    var model: String   // hw.model, e.g. Mac16,5
    var chip: String    // machdep.cpu.brand_string, e.g. Apple M4 Max
    var memoryGB: Int
    var macOS: String   // e.g. Version 26.6 (Build 25G62)

    static func current(appVersion: String) -> SystemInfo {
        func string(_ name: String) -> String {
            var size = 0
            guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return "unknown" }
            var bytes = [CChar](repeating: 0, count: size)
            guard sysctlbyname(name, &bytes, &size, nil, 0) == 0 else { return "unknown" }
            return String(decoding: bytes.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
        }
        var memory: UInt64 = 0
        var size = MemoryLayout<UInt64>.size
        sysctlbyname("hw.memsize", &memory, &size, nil, 0)
        return SystemInfo(appVersion: appVersion, model: string("hw.model"), chip: string("machdep.cpu.brand_string"),
                          memoryGB: Int(memory >> 30), macOS: ProcessInfo.processInfo.operatingSystemVersionString)
    }

    var line: String { "\(model) (\(chip)), \(memoryGB) GB, macOS \(macOS.replacingOccurrences(of: "Version ", with: ""))" }
}

/// What the launcher shows when the game stops with an error: a plain summary, and a report to
/// paste into a GitHub issue. The game writes one "Crash:" line on every fatal path (bb-probe,
/// bbgpu-host, the GPU watchdogs); older paths end with "STOP:" or "ERROR:".
struct CrashReport: Equatable {
    static let newIssue = URL(string: "https://github.com/Wooloomooloo2/bloodborne_mac_port/issues/new")!
    static let contextLines = 40
    static let maxLineLength = 300

    let status: Int32
    /// The game's own explanation (without its "Crash:" prefix), if the log has one.
    let cause: String?
    let summary: String
    let report: String
    /// The report with less context, to fit a new-issue link.
    let compactReport: String
    let system: SystemInfo

    /// Warnings and errors listed before the cause (the most recent, each text once).
    static let recentProblems = 12

    /// `log`: the session's text log (SessionLog: time and severity on each line; an older plain
    /// log works too); `settings`: one-line summaries; `home`: shortened to ~; `started`/`ended`:
    /// the session's start and end, when known; `natives`: macOS's crash reports of the session.
    init(status: Int32, log: String, logName: String, system: SystemInfo, settings: [String], home: String,
         started: Date? = nil, ended: Date? = nil, natives: [NativeCrash] = [], timeZone: TimeZone = .current) {
        self.status = status
        self.system = system
        var parsed = log.split(separator: "\n", omittingEmptySubsequences: false).map { SessionLog.parse(String($0)) }
        if parsed.last?.message == "" { parsed.removeLast() }
        let lines = parsed.map(\.message)
        // Severity as written, else as the launcher would classify the line (older logs).
        var severities: [LogSeverity] = []
        var previous: LogEntry?
        for line in parsed {
            let classified = SessionLog.classify(line.message, previous: previous)
            let severity = line.severity ?? classified.severity
            severities.append(severity)
            previous = LogEntry(time: line.time ?? Date(), severity: severity, source: classified.source, message: line.message)
        }
        // The first Crash: line is the cause (later ones follow from it, e.g. the GPU process
        // crashing, then bb-probe losing it); else the last STOP:/ERROR: line.
        var causeIndex = lines.firstIndex { $0.hasPrefix("Crash: ") }
        if causeIndex == nil {
            causeIndex = lines.lastIndex { $0.hasPrefix("STOP: ") || $0.hasPrefix("ERROR: ") }
        }
        let rawCause = causeIndex.map { lines[$0] }
        cause = rawCause.map { line in
            for prefix in ["Crash: ", "STOP: ", "ERROR: "] where line.hasPrefix(prefix) {
                return String(line.dropFirst(prefix.count))
            }
            return line
        }.map { Self.redact($0, home: home) }

        if let cause {
            let sentence = cause.prefix(1).uppercased() + cause.dropFirst()
            summary = sentence.hasSuffix(".") ? sentence : sentence + "."
        } else if status > 128 && status < 160 {
            summary = "The game stopped unexpectedly (signal \(status - 128))."
        } else {
            summary = "The game stopped with status \(status)."
        }

        let time = DateFormatter()
        time.locale = Locale(identifier: "en_US_POSIX")
        time.timeZone = timeZone
        time.dateFormat = "HH:mm:ss.SSS"
        func show(_ i: Int) -> String {
            let line = lines[i]
            let short = line.count > Self.maxLineLength ? String(line.prefix(Self.maxLineLength)) + " …" : line
            guard let at = parsed[i].time else { return Self.redact(short, home: home) } // older log
            let label = severities[i].label.padding(toLength: 6, withPad: " ", startingAt: 0)
            return Self.redact("\(time.string(from: at)) \(label) \(short)", home: home)
        }

        // The most recent warnings and errors before the cause (or the end), each text once.
        var problems: [(index: Int, count: Int)] = []
        var seen: [String: Int] = [:] // text -> position in `problems`
        for i in (0..<(causeIndex ?? lines.count)).reversed() where severities[i] <= .warning {
            let key = lines[i].trimmingCharacters(in: .whitespaces)
            if let at = seen[key] {
                problems[at].count += 1
            } else if problems.count < Self.recentProblems {
                seen[key] = problems.count
                problems.append((i, 1))
            }
        }
        problems.reverse()

        // The full report (the launcher's window, the clipboard, the zip), and a compact one that
        // fits a new-issue link: fewer lines around the cause, fewer warnings and frames.
        let cause = self.cause // (render must not capture self before init ends)
        func render(context: Int, problemCount: Int, frameCount: Int) -> String {
            // Log excerpt: around the cause, else the end of the log.
            let window: Range<Int>
            if let index = causeIndex {
                window = max(0, index - context)..<min(lines.count, index + context)
            } else {
                window = max(0, lines.count - context * 2)..<lines.count
            }
            let excerpt = window.map(show).joined(separator: "\n")
            let problems = problems.suffix(problemCount)

            var text = "**Bloodborne for Mac \(system.appVersion): crash report**\n\n"
            text += "- Mac: \(system.line)\n"
            if !settings.isEmpty { text += "- Settings: \(settings.joined(separator: ", "))\n" }
            if let started {
                let day = DateFormatter()
                day.locale = Locale(identifier: "en_US_POSIX")
                day.timeZone = timeZone
                day.dateFormat = "yyyy-MM-dd HH:mm:ss ZZZZZ"
                text += "- Session: started \(day.string(from: started))"
                if let ended {
                    let seconds = max(0, Int(ended.timeIntervalSince(started)))
                    text += ", ran \(seconds / 60) min \(seconds % 60) s"
                }
                text += "\n"
            }
            text += "- Exit status: \(status)\n"
            text += "- Cause: \(cause ?? "(no cause line in the log)")"
            if let index = causeIndex, let at = parsed[index].time { text += " (at \(time.string(from: at)))" }
            text += "\n"
            let counts = [LogSeverity.critical, .error, .warning].compactMap { severity -> String? in
                let n = severities.filter { $0 == severity }.count
                return n == 0 ? nil : "\(n) \(severity.keyword)\(n == 1 ? "" : severity == .critical ? "" : "s")"
            }
            text += "- Log: \(logName), \(lines.count) lines" + (counts.isEmpty ? "" : ": " + counts.joined(separator: ", ")) + "\n"
            for native in natives { text += "- macOS crash report: \(Self.redact(native.summary, home: home))\n" }
            text += "\n"
            for native in natives where !native.frames.isEmpty {
                text += "<details><summary>\(native.process): crashed thread</summary>\n\n```\n"
                text += native.frames.prefix(frameCount).map { Self.redact($0, home: home) }.joined(separator: "\n")
                text += "\n```\n</details>\n\n"
            }
            if !problems.isEmpty {
                text += "<details><summary>Warnings and errors before \(causeIndex != nil ? "the cause" : "the end") (\(problems.count) most recent)</summary>\n\n```\n"
                text += problems.map { show($0.index) + ($0.count > 1 ? "  (×\($0.count))" : "") }.joined(separator: "\n")
                text += "\n```\n</details>\n\n"
            }
            text += "<details><summary>\(logName) (\(window.count) lines\(causeIndex != nil ? " around the cause" : ", the end of the log"))</summary>\n\n"
            text += "```\n\(excerpt)\n```\n</details>\n"
            return text
        }
        report = render(context: Self.contextLines, problemCount: Self.recentProblems, frameCount: NativeCrash.maxFrames)
        compactReport = render(context: 8, problemCount: 5, frameCount: 8)
    }

    /// GitHub's new-issue links stop working somewhere above 8,000 characters.
    static let maxIssueURLLength = 7_500

    /// A new issue with the title and the report filled in: the full report if the link can hold
    /// it, else the compact one; `inline` false when even that is too long (the report is then
    /// pasted from the clipboard). `bundle`: the diagnostics zip's name, to drag onto the issue.
    func issueLink(bundle: String? = nil) -> (url: URL, inline: Bool) {
        let title = "Crash: " + String((cause ?? summary).prefix(90))
        let attach = bundle.map { "**Diagnostics:** please drag `\($0)` (the launcher showed it in Finder) onto this issue: it has the full session log and macOS's crash reports.\n\n" } ?? ""
        let ask = "**What were you doing when it happened?**\n\n\n\n"
        func url(_ body: String) -> URL {
            var components = URLComponents(url: Self.newIssue, resolvingAgainstBaseURL: false)!
            components.queryItems = [URLQueryItem(name: "title", value: title), URLQueryItem(name: "body", value: body)]
            return components.url!
        }
        for text in [report, compactReport] {
            let link = url(ask + attach + text)
            if link.absoluteString.count <= Self.maxIssueURLLength { return (link, true) }
        }
        let paste = """
            **Mac:** \(system.line)
            **Version:** \(system.appVersion)

            \(ask)\(attach)**Crash report** (Report on GitHub copied it to the clipboard: paste it below)

            """
        return (url(paste), false)
    }

    /// The new-issue link without a diagnostics zip.
    var issueURL: URL { issueLink().url }

    /// The user's home folder and any other /Users/<name> become ~, so reports don't carry names.
    static func redact(_ text: String, home: String) -> String {
        guard text.contains("/Users/") else { return text } // most lines (the session log redacts every line)
        var result = home.isEmpty ? text : text.replacingOccurrences(of: home, with: "~")
        result = result.replacingOccurrences(of: #"/Users/[^/\s:]+"#, with: "~", options: .regularExpression)
        return result
    }
}
