// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// After the game stopped with an error: macOS's crash reports of its processes, and a zip with
/// everything for an issue, in ~/Library/Logs/Bloodborne/Reports (built without asking: nothing
/// leaves the Mac until the player attaches it).
enum CrashBundle {
    static let keep = 10
    static var diagnosticReports: URL {
        URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Logs/DiagnosticReports")
    }

    /// The .ips reports of the session. macOS writes them a few seconds after the crash: waits up
    /// to `wait` for one when the exit looks like a crash (a signal, or a "Crash:" line).
    static func nativeCrashes(since start: Date, expectOne: Bool, wait: TimeInterval = 8) -> (files: [URL], crashes: [NativeCrash]) {
        let deadline = Date().addingTimeInterval(expectOne ? wait : 0)
        var files: [URL] = []
        repeat {
            files = NativeCrash.find(in: diagnosticReports, since: start.addingTimeInterval(-5), until: Date().addingTimeInterval(60))
            if !files.isEmpty || Date() >= deadline { break }
            Thread.sleep(forTimeInterval: 0.5)
        } while true
        let crashes = files.compactMap { url in
            (try? String(contentsOf: url, encoding: .utf8)).flatMap { NativeCrash(ips: $0, file: url.lastPathComponent) }
        }
        return (files, crashes)
    }

    /// Writes Reports/crash-<session>.zip: the report, the session's .log and .jsonl, the .ips
    /// reports, bbport.ini and launch.conf, with the home folder as ~ throughout.
    static func write(report: CrashReport, name: String, logs: URL, sessionLog: URL?, ipsFiles: [URL],
                      settings: URL, launchConf: URL) throws -> URL {
        let fm = FileManager.default
        let home = NSHomeDirectory()
        let folder = logs.appendingPathComponent("Reports")
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        let staging = fm.temporaryDirectory.appendingPathComponent("bloodborne-report-\(UUID().uuidString)/\(name)")
        try fm.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: staging.deletingLastPathComponent()) }

        func copy(_ url: URL, as fileName: String? = nil, scrub: (String) -> String) {
            guard let text = try? String(contentsOf: url, encoding: .utf8) else { return }
            try? scrub(text).write(to: staging.appendingPathComponent(fileName ?? url.lastPathComponent), atomically: true, encoding: .utf8)
        }
        let redact = { (text: String) in CrashReport.redact(text, home: home) }
        try report.report.write(to: staging.appendingPathComponent("crash-report.md"), atomically: true, encoding: .utf8)
        if let sessionLog {
            copy(sessionLog, scrub: redact)
            copy(sessionLog.deletingPathExtension().appendingPathExtension("jsonl"), scrub: redact)
        }
        for ips in ipsFiles { copy(ips, scrub: { NativeCrash.scrub($0, home: home) }) }
        copy(settings, as: "bbport.ini", scrub: redact)
        copy(launchConf, as: "launch.conf", scrub: redact)

        let zip = folder.appendingPathComponent(name + ".zip")
        try? fm.removeItem(at: zip)
        // (No resource forks or extended attributes: no "._" files in the zip.)
        let result = GameRunner.run(["/usr/bin/ditto", "-c", "-k", "--norsrc", "--noextattr", "--noqtn", "--keepParent",
                                     staging.path, zip.path])
        guard result.status == 0 else {
            throw NSError(domain: "CrashBundle", code: Int(result.status),
                          userInfo: [NSLocalizedDescriptionKey: "Could not write \(zip.lastPathComponent): \(result.output)"])
        }
        // Only the newest reports stay.
        let zips = ((try? fm.contentsOfDirectory(atPath: folder.path)) ?? []).filter { $0.hasSuffix(".zip") }.sorted()
        for old in zips.dropLast(keep) { try? fm.removeItem(at: folder.appendingPathComponent(old)) }
        return zip
    }
}
