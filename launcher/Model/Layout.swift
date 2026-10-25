// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// How the launcher starts the game: the program, arguments, environment and folder.
struct LaunchPlan: Equatable {
    var executable: URL
    var arguments: [String]
    var environment: [String: String]
    var directory: URL
}

enum Logs {
    /// game.log (this session) and game.previous.log.
    static func folder(home: URL) -> URL { home.appendingPathComponent("Library/Logs/Bloodborne") }
}

/// Where the parts are: the release app (everything inside it, scripts/macos/package_app.sh) or
/// the checkout's local app (scripts/macos/make_app.sh; Info.plist BBRepository = the checkout).
enum Layout: Equatable {
    case packaged(contents: URL)
    case checkout(repo: URL, app: URL)

    /// By path: Foundation adds a trailing "/" to URLs of folders that exist, so URL == is not
    /// enough.
    static func == (a: Layout, b: Layout) -> Bool {
        switch (a, b) {
        case let (.packaged(x), .packaged(y)): x.standardizedFileURL.path == y.standardizedFileURL.path
        case let (.checkout(r1, a1), .checkout(r2, a2)):
            r1.standardizedFileURL.path == r2.standardizedFileURL.path
                && a1.standardizedFileURL.path == a2.standardizedFileURL.path
        default: false
        }
    }

    static func detect(bundleURL: URL, info: [String: Any]) -> Layout? {
        let contents = bundleURL.appendingPathComponent("Contents")
        if FileManager.default.fileExists(atPath: contents.appendingPathComponent("Resources/port/run.sh").path) {
            return .packaged(contents: contents)
        }
        if let repo = info["BBRepository"] as? String, !repo.isEmpty {
            return .checkout(repo: URL(fileURLWithPath: repo), app: bundleURL)
        }
        return nil
    }

    private var port: URL? {
        if case let .packaged(contents) = self { return contents.appendingPathComponent("Resources/port") }
        return nil
    }

    private static func home(_ env: [String: String]) -> URL {
        URL(fileURLWithPath: env["HOME"] ?? NSHomeDirectory())
    }

    private static func defaultData(_ env: [String: String]) -> URL {
        home(env).appendingPathComponent("Library/Application Support/Bloodborne")
    }

    /// launch.conf: in the data folder for the release app, next to Bloodborne.command in a
    /// checkout (shared with it). BB_LAUNCH_CONF overrides (tests).
    func launchConfURL(env: [String: String]) -> URL {
        if let path = env["BB_LAUNCH_CONF"], !path.isEmpty { return URL(fileURLWithPath: path) }
        switch self {
        case .packaged: return Self.defaultData(env).appendingPathComponent("launch.conf")
        case let .checkout(repo, _): return repo.appendingPathComponent("launch.conf")
        }
    }

    /// bbport.ini, the shader cache and the default save folder: BB_DATA_DIR, else launch.conf's
    /// BB_DATA_DIR, else ~/Library/Application Support/Bloodborne (run.sh's default).
    func dataDir(env: [String: String], config: LaunchConfig) -> URL {
        if let path = env["BB_DATA_DIR"], !path.isEmpty { return URL(fileURLWithPath: path) }
        return config.dataDir ?? Self.defaultData(env)
    }

    /// The game folder to suggest: ../CUSA03173 next to a checkout (as Bloodborne.command does).
    func defaultGameDir() -> URL? {
        guard case let .checkout(repo, _) = self else { return nil }
        let game = repo.deletingLastPathComponent().appendingPathComponent("CUSA03173")
        return FileManager.default.fileExists(atPath: game.appendingPathComponent("eboot.bin").path) ? game : nil
    }

    /// The benchmark's save and recorded route (docs/DESIGN_BENCHMARK.md; benchmark/ in git):
    /// BB_BENCH_ASSETS, else Contents/Resources/benchmark in the release app, benchmark/ in a
    /// checkout. Benchmark.assetsProblem says whether they are there.
    func benchmarkAssets(env: [String: String]) -> URL {
        if let path = env["BB_BENCH_ASSETS"], !path.isEmpty { return URL(fileURLWithPath: path) }
        switch self {
        case let .packaged(contents): return contents.appendingPathComponent("Resources/benchmark")
        case let .checkout(repo, _): return repo.appendingPathComponent("benchmark")
        }
    }

    /// A command running one of the port's Python scripts (`scripts/<script>`): the bundled
    /// Python in the release app, the system's in a checkout. -B: no bytecode inside the signed
    /// app bundle.
    func pythonCommand(script: String) -> [String] {
        switch self {
        case let .packaged(contents):
            [contents.appendingPathComponent("Resources/python/bin/python3").path, "-B",
             contents.appendingPathComponent("Resources/port/scripts/\(script)").path]
        case let .checkout(repo, _):
            ["/usr/bin/env", "python3", "-B", repo.appendingPathComponent("scripts/\(script)").path]
        }
    }

    var gameCheckCommand: [String] { pythonCommand(script: "game_check.py") }

    /// The environment the earlier bash launchers set (release app, checkout app), plus the save
    /// folder. Display settings reach the game through
    /// bbport.ini (run.sh: scripts/launch_settings.sh), not here.
    func launchPlan(gameDir: URL, saveDir: URL?, dataDir: URL, env: [String: String]) -> LaunchPlan {
        var environment = env
        environment["BB_GAME_DIR"] = gameDir.path
        // The in-game menu's "Show the launcher" toggle writes BB_SHOW_LAUNCHER there.
        environment["BB_LAUNCH_CONF"] = launchConfURL(env: env).path
        if let saveDir {
            environment["BB_SAVE_DIR"] = saveDir.path
        } else {
            environment.removeValue(forKey: "BB_SAVE_DIR")
        }
        let bash = URL(fileURLWithPath: "/bin/bash")
        switch self {
        case let .packaged(contents):
            let port = self.port!
            environment["BB_DATA_DIR"] = dataDir.path
            environment["BB_PREBUILT"] = "1"
            environment["BB_PROBE"] = contents.appendingPathComponent("MacOS/bb-probe").path
            environment["PYTHON"] = contents.appendingPathComponent("Resources/python/bin/python3").path
            environment["PYTHONDONTWRITEBYTECODE"] = "1"
            return LaunchPlan(executable: bash, arguments: [port.appendingPathComponent("run.sh").path],
                              environment: environment, directory: port)
        case let .checkout(repo, app):
            // run.sh rebuilds a checkout: Finder's PATH lacks Homebrew's cmake and ninja.
            environment["PATH"] = "/opt/homebrew/bin:" + (env["PATH"] ?? "/usr/bin:/bin:/usr/sbin:/sbin")
            environment["BB_GPU_HOST"] = app.appendingPathComponent("Contents/MacOS/bbgpu-host").path
            return LaunchPlan(executable: bash, arguments: [repo.appendingPathComponent("run.sh").path],
                              environment: environment, directory: repo)
        }
    }
}
