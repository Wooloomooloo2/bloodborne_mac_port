// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum Shell {
    /// Single-quoted for bash: it's -> 'it'\''s'.
    static func quote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    /// The value of a shell word: single quotes, double quotes and backslash escapes (what
    /// quote() and the earlier launchers' printf %q wrote).
    static func unquote(_ s: String) -> String {
        var out = ""
        var single = false, double = false, escaped = false
        for c in s {
            if escaped {
                out.append(c)
                escaped = false
            } else if single {
                if c == "'" { single = false } else { out.append(c) }
            } else if c == "\\" {
                escaped = true
            } else if c == "'" && !double {
                single = true
            } else if c == "\"" {
                double.toggle()
            } else {
                out.append(c)
            }
        }
        return out
    }
}

/// launch.conf: the launcher's own settings as shell assignments (bash sources the file in
/// Bloodborne.command): BB_GAME_DIR, BB_SAVE_DIR (absent: the default), BB_SHOW_LAUNCHER, and the
/// update check's BB_CHECK_UPDATES (absent: not asked yet), BB_UPDATE_TEST_BUILDS, BB_UPDATE_SKIP (a version the player
/// skipped) and BB_UPDATE_CHECKED (the last automatic check, Unix seconds).
/// Other assignments in it (BB_DATA_DIR, BB_FPS, ...) stay in the IniFile.
struct LaunchConfig: Equatable {
    var gameDir: URL?
    var saveDir: URL?
    var showLauncher = true
    /// Automatic update checks; nil until the player answers the first-run question.
    var checkUpdates: Bool?
    var testBuilds = false
    var skippedVersion: String?
    var lastUpdateCheck: Date?
    private(set) var dataDir: URL?
    /// Other BB_* / MTL_* / MVK_* assignments (diagnostics such as BB_FRAME_STATS=1): passed on to
    /// the game; a variable already set in the environment wins (#69).
    private(set) var extraEnvironment: [String: String] = [:]
    static let ownKeys: Set<String> = ["BB_GAME_DIR", "BB_SAVE_DIR", "BB_SHOW_LAUNCHER", "BB_CHECK_UPDATES",
                                       "BB_UPDATE_TEST_BUILDS", "BB_UPDATE_SKIP", "BB_UPDATE_CHECKED",
                                       "BB_LAUNCH_CONF"]

    init() {}

    init(ini: IniFile) {
        func path(_ key: String) -> URL? {
            guard let v = ini[key].map(Shell.unquote), !v.isEmpty else { return nil }
            return URL(fileURLWithPath: v)
        }
        gameDir = path("BB_GAME_DIR")
        saveDir = path("BB_SAVE_DIR")
        dataDir = path("BB_DATA_DIR")
        if let v = ini["BB_SHOW_LAUNCHER"] { showLauncher = Shell.unquote(v) != "0" }
        if let v = ini["BB_CHECK_UPDATES"] { checkUpdates = Shell.unquote(v) != "0" }
        if let v = ini["BB_UPDATE_TEST_BUILDS"] { testBuilds = Shell.unquote(v) == "1" }
        skippedVersion = ini["BB_UPDATE_SKIP"].map(Shell.unquote).flatMap { $0.isEmpty ? nil : $0 }
        lastUpdateCheck = ini["BB_UPDATE_CHECKED"].flatMap { TimeInterval(Shell.unquote($0)) }
            .map(Date.init(timeIntervalSince1970:))
        for case let .entry(key, value) in ini.lines
        where ["BB_", "MTL_", "MVK_"].contains(where: key.hasPrefix) && !Self.ownKeys.contains(key)
            && key.allSatisfy({ $0.isUppercase || $0.isNumber || $0 == "_" }) {
            extraEnvironment[key] = Shell.unquote(value)
        }
    }

    /// `env` plus the extra assignments it does not already set.
    func environment(adding env: [String: String]) -> [String: String] {
        env.merging(extraEnvironment) { current, _ in current }
    }

    func apply(to ini: inout IniFile) {
        ini["BB_GAME_DIR"] = gameDir.map { Shell.quote($0.path) }
        ini["BB_SAVE_DIR"] = saveDir.map { Shell.quote($0.path) }
        ini["BB_SHOW_LAUNCHER"] = showLauncher ? "1" : "0"
        ini["BB_CHECK_UPDATES"] = checkUpdates.map { $0 ? "1" : "0" }
        ini["BB_UPDATE_TEST_BUILDS"] = testBuilds ? "1" : nil
        ini["BB_UPDATE_SKIP"] = skippedVersion.map(Shell.quote)
        ini["BB_UPDATE_CHECKED"] = lastUpdateCheck.map { String(Int($0.timeIntervalSince1970)) }
    }
}
