// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Foundation

/// Process work for the launcher: the game, the game check, Rosetta.
enum GameRunner {
    /// Runs a short command; its status and output (stdout and stderr together).
    @discardableResult
    static func run(_ command: [String]) -> (status: Int32, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: command[0])
        process.arguments = Array(command.dropFirst())
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do { try process.run() } catch { return (-1, error.localizedDescription) }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return (process.terminationStatus, String(decoding: data, as: UTF8.self))
    }

    /// The game's processes (one copy at a time).
    static var isGameRunning: Bool {
        run(["/usr/bin/pgrep", "-x", "bbgpu-host"]).status == 0 ||
            run(["/usr/bin/pgrep", "-x", "bbgpu-host-macos27"]).status == 0 ||
            run(["/usr/bin/pgrep", "-x", "bb-probe"]).status == 0
    }

    static var hasRosetta: Bool { run(["/usr/bin/arch", "-x86_64", "/usr/bin/true"]).status == 0 }

    /// Asks for an administrator password (macOS's dialog) and installs Rosetta 2.
    static func installRosetta() -> Bool {
        run(["/usr/bin/osascript", "-e",
             "do shell script \"/usr/sbin/softwareupdate --install-rosetta --agree-to-license\" with administrator privileges"])
        return hasRosetta
    }

    /// nil when the folder holds a usable game, else game_check.py's reason.
    static func checkGame(_ folder: URL, layout: Layout) -> String? {
        guard FileManager.default.fileExists(atPath: folder.appendingPathComponent("eboot.bin").path) else {
            return "There is no eboot.bin in this folder. Choose the folder that contains eboot.bin, sce_sys and dvdroot_ps4."
        }
        let result = run(layout.gameCheckCommand + [folder.path])
        if result.status == 0 { return nil }
        let reason = result.output.trimmingCharacters(in: .whitespacesAndNewlines)
        return reason.isEmpty ? "The game check failed (status \(result.status))." : reason
    }

    /// Starts the game; `done` gets its exit status on the main queue. The output goes to
    /// `log` (the previous session's log is kept as game.previous.log).
    static func start(_ plan: LaunchPlan, log: URL, done: @escaping (Int32) -> Void) throws -> Process {
        let fm = FileManager.default
        try fm.createDirectory(at: log.deletingLastPathComponent(), withIntermediateDirectories: true)
        let previous = log.deletingLastPathComponent().appendingPathComponent("game.previous.log")
        if fm.fileExists(atPath: log.path) {
            try? fm.removeItem(at: previous)
            try? fm.moveItem(at: log, to: previous)
        }
        fm.createFile(atPath: log.path, contents: nil)
        let output = try FileHandle(forWritingTo: log)
        let process = Process()
        process.executableURL = plan.executable
        process.arguments = plan.arguments
        process.environment = plan.environment
        process.currentDirectoryURL = plan.directory
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = output
        process.standardError = output
        process.terminationHandler = { finished in
            try? output.close()
            DispatchQueue.main.async { done(finished.terminationStatus) }
        }
        try process.run()
        return process
    }
}
