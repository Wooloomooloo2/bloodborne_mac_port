// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// A macOS crash report (.ips in ~/Library/Logs/DiagnosticReports) of one of the game's processes:
/// the exception and the crashed thread's frames, for the crash report and the diagnostics zip.
/// An .ips file is one JSON line (the header) followed by one JSON object (the report).
struct NativeCrash: Equatable {
    /// The game's processes (bbgpu-host-macos27 included by prefix).
    static let processes = ["bb-probe", "bbgpu-host"]
    static let maxFrames = 16

    var file: String
    var process: String
    /// "EXC_BAD_ACCESS (SIGSEGV), KERN_INVALID_ADDRESS at 0x0000000000000010", or the termination.
    var exception: String
    /// "0  libbbgpu.dylib  Vulkan::Foo::Bar() + 120" for the crashed thread.
    var frames: [String]

    /// nil when the text is not an .ips report of one of the game's processes.
    init?(ips text: String, file: String) {
        guard let newline = text.firstIndex(of: "\n"),
              let header = Self.object(text[..<newline]),
              let body = Self.object(text[text.index(after: newline)...]) else { return nil }
        let name = body["procName"] as? String ?? header["app_name"] as? String ?? header["name"] as? String ?? ""
        guard Self.processes.contains(where: { name.hasPrefix($0) }) else { return nil }
        self.file = file
        process = name

        var parts: [String] = []
        if let exception = body["exception"] as? [String: Any] {
            var head = exception["type"] as? String ?? "exception"
            if let signal = exception["signal"] as? String { head += " (\(signal))" }
            parts.append(head)
            if let subtype = exception["subtype"] as? String { parts.append(subtype) }
        }
        if let termination = body["termination"] as? [String: Any], let indicator = termination["indicator"] as? String {
            parts.append(indicator)
        }
        exception = parts.isEmpty ? "unknown" : parts.joined(separator: ", ")

        let images = (body["usedImages"] as? [[String: Any]]) ?? []
        let threads = (body["threads"] as? [[String: Any]]) ?? []
        let crashed = threads.first { $0["triggered"] as? Bool == true } ?? threads.first
        let rawFrames = (crashed?["frames"] as? [[String: Any]]) ?? []
        frames = rawFrames.prefix(Self.maxFrames).enumerated().map { i, frame in
            let index = frame["imageIndex"] as? Int ?? -1
            let image = images.indices.contains(index) ? images[index]["name"] as? String ?? "???" : "???"
            let place: String
            if let symbol = frame["symbol"] as? String {
                place = symbol + (frame["symbolLocation"] as? Int).map { " + \($0)" }.orEmpty
            } else {
                place = String(format: "0x%llx", (frame["imageOffset"] as? Int).map(Int64.init) ?? 0)
            }
            return "\(i)".padding(toLength: 3, withPad: " ", startingAt: 0) + image.padding(toLength: 24, withPad: " ", startingAt: 0) + " " + place
        }
    }

    private static func object<S: StringProtocol>(_ text: S) -> [String: Any]? {
        (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any]
    }

    /// The game's .ips reports written between `since` and `until` (newest last).
    static func find(in folder: URL, since: Date, until: Date) -> [URL] {
        let fm = FileManager.default
        let names = (try? fm.contentsOfDirectory(atPath: folder.path)) ?? []
        return names.filter { name in
            name.hasSuffix(".ips") && processes.contains { name.hasPrefix($0) || name.hasPrefix("ExcUserFault_" + $0) }
        }.compactMap { name -> (URL, Date)? in
            let url = folder.appendingPathComponent(name)
            guard let date = (try? fm.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date,
                  date >= since, date <= until else { return nil }
            return (url, date)
        }.sorted { $0.1 < $1.1 }.map(\.0)
    }

    /// An .ips report for the diagnostics zip: the home folder as ~, and the machine's identifiers
    /// (crash reporter key, sleep/wake and boot session ids) removed.
    static func scrub(_ text: String, home: String) -> String {
        // .ips files escape slashes ("\/Users\/name\/..."): the same JSON without, so paths match.
        var result = CrashReport.redact(text.replacingOccurrences(of: "\\/", with: "/"), home: home)
        for key in ["crashReporterKey", "sleepWakeUUID", "bootSessionUUID", "deviceIdentifierForVendor"] {
            result = result.replacingOccurrences(of: "\"\(key)\"\\s*:\\s*\"[^\"]*\"", with: "\"\(key)\":\"removed\"",
                                                 options: .regularExpression)
        }
        return result
    }

    /// For the crash report: one line, then the frames.
    var summary: String { "\(process): \(exception) (\(file))" }
}

private extension Optional where Wrapped == String {
    var orEmpty: String { self ?? "" }
}
