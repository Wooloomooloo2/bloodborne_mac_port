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
    let system: SystemInfo

    /// `log`: the session's game.log; `settings`: one-line summaries; `home`: shortened to ~.
    init(status: Int32, log: String, logName: String, system: SystemInfo, settings: [String], home: String) {
        self.status = status
        self.system = system
        let lines = log.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
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

        // Log excerpt: around the cause, else the end of the log.
        let window: ArraySlice<String>
        if let index = causeIndex {
            window = lines[max(0, index - Self.contextLines)..<min(lines.count, index + Self.contextLines)]
        } else {
            window = lines.suffix(Self.contextLines * 2)
        }
        let excerpt = window.map { line -> String in
            let short = line.count > Self.maxLineLength ? String(line.prefix(Self.maxLineLength)) + " …" : line
            return Self.redact(short, home: home)
        }.joined(separator: "\n")

        var text = "**Bloodborne for Mac \(system.appVersion): crash report**\n\n"
        text += "- Mac: \(system.line)\n"
        if !settings.isEmpty { text += "- Settings: \(settings.joined(separator: ", "))\n" }
        text += "- Exit status: \(status)\n"
        text += "- Cause: \(cause ?? "(no cause line in the log)")\n\n"
        text += "<details><summary>\(logName) (\(window.count) lines\(causeIndex != nil ? " around the cause" : ", the end of the log"))</summary>\n\n"
        text += "```\n\(excerpt)\n```\n</details>\n"
        report = text
    }

    /// A new issue with the title and the Mac filled in; the report itself is pasted (it is too
    /// long for a URL).
    var issueURL: URL {
        let title = "Crash: " + String((cause ?? summary).prefix(90))
        let body = """
            **Mac:** \(system.line)
            **Version:** \(system.appVersion)

            **What were you doing when it happened?**


            **Crash report** (Report on GitHub copied it to the clipboard: paste it below)

            """
        var components = URLComponents(url: Self.newIssue, resolvingAgainstBaseURL: false)!
        components.queryItems = [URLQueryItem(name: "title", value: title), URLQueryItem(name: "body", value: body)]
        return components.url!
    }

    /// The user's home folder and any other /Users/<name> become ~, so reports don't carry names.
    static func redact(_ text: String, home: String) -> String {
        var result = home.isEmpty ? text : text.replacingOccurrences(of: home, with: "~")
        result = result.replacingOccurrences(of: #"/Users/[^/\s:]+"#, with: "~", options: .regularExpression)
        return result
    }
}
