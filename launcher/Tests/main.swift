// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Model tests for the launcher (scripts/macos/build_launcher.sh --test). One PASS line or exit 1.
import Foundation

var failures = 0
func check(_ condition: @autoclosure () -> Bool, _ what: String, line: Int = #line) {
    if !condition() {
        failures += 1
        print("FAIL (launcher/Tests/main.swift:\(line)): \(what)")
    }
}

// IniFile: comments, order and unknown keys survive; set, add, remove.
do {
    var ini = IniFile(text: "# bbport settings\nupscaler=fsr3\nweird line\nfps_cap=40\r\n\nrenderer=metal\n")
    check(ini["upscaler"] == "fsr3", "read a key")
    check(ini["fps_cap"] == "40", "CRLF value")
    check(ini["missing"] == nil, "missing key")
    ini["upscaler"] = "metalfx"
    ini["fullscreen"] = "1"
    ini["renderer"] = nil
    check(ini.text == "# bbport settings\nupscaler=metalfx\nweird line\nfps_cap=40\n\nfullscreen=1\n",
          "write keeps the rest: \(ini.text.debugDescription)")
    check(IniFile(text: "").text == "\n" && IniFile(text: "").lines.isEmpty, "empty file")
    check(IniFile(text: "a=1\na=2\n")["a"] == "2", "last occurrence wins")
    check(IniFile(text: "key = spaced value \n")["key"] == "spaced value", "trimmed")
    check(IniFile(text: "#commented=1\n")["#commented"] == nil, "comment is not a key")
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("launcher-test-\(getpid())/x/bbport.ini")
    try? ini.write(to: url)
    check(IniFile(contentsOf: url) == ini, "write and read back")
    check(IniFile(contentsOf: URL(fileURLWithPath: "/nonexistent/bbport.ini")).lines.isEmpty, "missing file")
    try? FileManager.default.removeItem(at: url.deletingLastPathComponent().deletingLastPathComponent())
}

// Resolution: render sizes match scripts/patches.py (even sizes) for every output and preset.
do {
    let expected: [PixelSize: [PixelSize]] = [
        PixelSize(1280, 720): [PixelSize(1280, 720), PixelSize(854, 480), PixelSize(752, 424), PixelSize(640, 360), PixelSize(426, 240)],
        PixelSize(1920, 1080): [PixelSize(1920, 1080), PixelSize(1280, 720), PixelSize(1130, 636), PixelSize(960, 540), PixelSize(640, 360)],
        PixelSize(2560, 1440): [PixelSize(2560, 1440), PixelSize(1706, 960), PixelSize(1506, 848), PixelSize(1280, 720), PixelSize(854, 480)],
        PixelSize(3456, 1944): [PixelSize(3456, 1944), PixelSize(2304, 1296), PixelSize(2032, 1144), PixelSize(1728, 972), PixelSize(1152, 648)],
        PixelSize(3840, 2160): [PixelSize(3840, 2160), PixelSize(2560, 1440), PixelSize(2258, 1270), PixelSize(1920, 1080), PixelSize(1280, 720)],
    ]
    check(Resolution.outputs == [PixelSize(1280, 720), PixelSize(1920, 1080), PixelSize(2560, 1440), PixelSize(3456, 1944), PixelSize(3840, 2160)], "output list")
    for output in Resolution.outputs {
        for preset in 0..<Resolution.presetScales.count {
            let got = Resolution.renderSize(output: output, preset: preset)
            check(got == expected[output]![preset], "render size \(output) preset \(preset): \(got)")
        }
    }
    check(Resolution.renderSize(output: PixelSize(1920, 1080), preset: 99) == PixelSize(640, 360), "preset clamped")
    check(PixelSize(iniValue: "2560x1440") == PixelSize(2560, 1440), "parse WxH")
    check(PixelSize(iniValue: "3456X1944") == PixelSize(3456, 1944), "parse upper-case X")
    check(PixelSize(iniValue: "junk") == nil && PixelSize(iniValue: "0x0") == nil, "bad sizes")
    check(PixelSize(2560, 1440).iniValue == "2560x1440" && PixelSize(2560, 1440).description == "2560 × 1440", "format")
}

// Native wide patch: display size is independent of the neutral renderer size.
do {
    var settings = GameSettings()
    settings.output = PixelSize(2560, 1440)
    settings.ultrawide = true
    settings.wideOutput = PixelSize(3840, 1080)
    settings.upscaler = .metalfx
    settings.preset = 3
    settings.liveResolution = .on
    var ini = IniFile()
    settings.apply(to: &ini)
    check(ini["output_res"] == "1920x1080" && ini["ultrawide_res"] == "3840x1080", "wide patch and neutral renderer")
    check(ini["upscaler"] == "off" && ini["preset"] == "0" && ini["live_resolution"] == "0", "native wide avoids duplicate host scaling")
    var loaded = GameSettings(ini: ini)
    check(loaded.ultrawide && loaded.renderSize == PixelSize(3840, 1080), "wide selection survives restart")
    loaded.ultrawide = false
    loaded.apply(to: &ini)
    check(ini["ultrawide_res"] == nil && ini["output_res"] == "2560x1440", "disable removes patch and restores standard output")
}

// GameSettings: defaults, reading, values the launcher does not offer, writing back.
do {
    let defaults = GameSettings(ini: IniFile())
    check(defaults == GameSettings(), "empty ini = defaults")
    check(defaults.output == PixelSize(1920, 1080) && defaults.upscaler == .off && defaults.preset == 0, "default display")
    check(defaults.frameCap == .fps60 && !defaults.fullscreen, "default cap, window")
    check(defaults.gameTiming == .fixed && GameSettings(ini: IniFile(text: "game_timing=variable\n")).gameTiming == .variable, "game timing: default fixed, read")
    check(GameSettings(ini: IniFile(text: "game_timing=slow\n")).gameTiming == .fixed, "game timing: invalid -> fixed")
    var adaptive = GameSettings(ini: IniFile(text: "game_timing=adaptive\nfps_cap=40\n"))
    check(adaptive.gameTiming == .adaptive && adaptive.frameCap == .fps40, "adaptive timing at 40 FPS")
    var timingIni = IniFile(text: "unknown=preserved\n")
    adaptive.apply(to: &timingIni)
    check(timingIni["game_timing"] == "adaptive" && timingIni["unknown"] == "preserved", "adaptive timing round trip")
    adaptive.frameCap = .fps120
    check(adaptive.gameTiming.help(for: .fps120).contains("Experimental"), "adaptive limits are visible")
    check(GameTiming.adaptive.help(for: .fps30).contains("original timing"), "30 FPS stays original")
    check(FrameCap.fps60.hasFixedSteps && FrameCap.fps30.hasFixedSteps && FrameCap.fps90.hasFixedSteps && !FrameCap.fps40.hasFixedSteps && !FrameCap.fps120.hasFixedSteps, "caps with fixed-step patches")
    check(!defaults.jitter && defaults.modelDetail == .gameDefault && !defaults.reactive && defaults.reactiveScale == 1.0 && defaults.reactiveThreshold == 0.2 && defaults.reactiveMax == 0.9, "default advanced (bbport_settings.h)")
    check(!defaults.objectMotion && defaults.liveResolution == .off && defaults.effects["debug_camera"] == false && defaults.effects["debug_menu"] == false, "default advanced, debug")
    check(defaults.effects["effect_dof"] == true && defaults.effects["effect_ssr"] == false && defaults.effects["skip_intro"] == false, "default effects")
    check(defaults.sharpen && defaults.sharpness == 0.3 && !defaults.showFPS && !defaults.metalHUD && defaults.shaderCompile == .balanced, "default extras")
    check(defaults.textureCompression && !GameSettings(ini: IniFile(text: "texture_compression=0\n")).textureCompression, "texture compression: default on, read")

    let read = GameSettings(ini: IniFile(text: "upscaler=metalfx\npreset=2\noutput_res=2560x1440\nfps_cap=40\nfullscreen=1\neffect_dof=0\nshow_fps=1\nmetal_hud=1\nrenderer=metal\nshader_compile=skip\nsharpness=0.75\n"))
    check(read.upscaler == .metalfx && read.preset == 2 && read.output == PixelSize(2560, 1440), "read display")
    check(read.frameCap == .fps40 && read.fullscreen && read.shaderCompile == .skip, "read launcher keys")
    let advanced = GameSettings(ini: IniFile(text: "jitter=1\nmodel_lod=-2\nreactive=1\nreactive_scale=2.5\nreactive_threshold=0.4\nreactive_max=0.7\nobject_motion=1\nlive_resolution=auto\ndebug_camera=1\n"))
    check(advanced.jitter && advanced.modelDetail == .highest && advanced.reactive && advanced.reactiveScale == 2.5, "read advanced")
    check(advanced.reactiveThreshold == 0.4 && advanced.reactiveMax == 0.7 && advanced.objectMotion && advanced.liveResolution == .auto && advanced.effects["debug_camera"] == true, "read advanced, debug")
    let bad = GameSettings(ini: IniFile(text: "model_lod=7\nreactive_threshold=3\nlive_resolution=2\n"))
    check(bad.modelDetail == .gameDefault && bad.reactiveThreshold == 0.2 && bad.liveResolution == .off, "invalid advanced values -> defaults")
    check(read.effects["effect_dof"] == false && read.showFPS && read.metalHUD && read.sharpness == 0.75, "read effects")
    check(read.renderSize == PixelSize(1506, 848), "render size follows output and preset")
    check(GameSettings(ini: IniFile(text: "upscaler=taa\npreset=3\n")).renderSize == PixelSize(1920, 1080), "TAA renders native")

    // Review focus 2: values the launcher does not offer survive unless changed.
    var other = IniFile(text: "output_res=3008x1692\nupscaler=fsr4\nfps_cap=45\npreset=9\nsharpness=7\nfsr4_auto_exposure=1\n")
    let odd = GameSettings(ini: other)
    check(odd.output == PixelSize(3008, 1692) && odd.upscaler == .fsr4, "unlisted output and FSR 4 kept")
    check(odd.frameCap == .fps60 && odd.preset == 0 && odd.sharpness == 0.3, "invalid values -> defaults")
    odd.apply(to: &other)
    check(other["output_res"] == "3008x1692" && other["upscaler"] == "fsr4", "written back unchanged")
    check(other["fsr4_auto_exposure"] == "1" && other["renderer"] == nil, "other keys kept; no renderer key")
    var changed = read
    changed.output = PixelSize(3840, 2160)
    changed.effects["effect_ssr"] = true
    changed.modelDetail = .lowest
    changed.liveResolution = .auto
    changed.effects["debug_menu"] = true
    var file = IniFile(text: "# header\nupscaler=metalfx\nrenderer=vulkan\n")
    changed.apply(to: &file)
    check(file["output_res"] == "3840x2160" && file["effect_ssr"] == "1" && file["renderer"] == "vulkan", "apply writes and keeps")
    check(file["fps_cap"] == "40" && file["fullscreen"] == "1" && file["sharpness"] == "0.75", "apply launcher keys")
    check(file["model_lod"] == "2" && file["live_resolution"] == "auto" && file["debug_menu"] == "1" && file["jitter"] == "0", "apply advanced keys")
    check(file["reactive_scale"] == "1.00" && file["reactive_threshold"] == "0.20" && file["reactive_max"] == "0.90" && file["object_motion"] == "0", "apply reactive keys")
    check(file["metal_hud"] == "1", "apply writes the Metal HUD key")
    check(file["texture_compression"] == "1", "apply writes the texture compression key")
    check(file["game_timing"] == "fixed", "apply writes the game timing key")
    check(file.text.hasPrefix("# header\n"), "header kept")
    // Review focus 3: reloading after the game's menu changed the file picks up its values.
    file["preset"] = "3"
    check(GameSettings(ini: file).preset == 3, "reload sees the menu's change")
}

// Shell quoting and launch.conf (review focus 1: spaces, quotes; bash must source it).
do {
    let tricky = ["/Users/a/Library/Application Support/Bloodborne", "/Volumes/My Drive/Hunter's saves", "/tmp/$HOME `x` \\ \"q\""]
    for path in tricky {
        check(Shell.unquote(Shell.quote(path)) == path, "quote round trip \(path)")
    }
    check(Shell.unquote("/Users/a/My\\ Games/CUSA03173") == "/Users/a/My Games/CUSA03173", "printf %q style")
    check(Shell.unquote("\"/a b\"") == "/a b", "double quotes")

    var conf = IniFile(text: "BB_GAME_DIR=/Users/a/My\\ Games/CUSA03173\nBB_DATA_DIR=/tmp/data\nBB_FPS=uncap\n")
    var config = LaunchConfig(ini: conf)
    check(config.gameDir?.path == "/Users/a/My Games/CUSA03173" && config.saveDir == nil && config.showLauncher, "read launch.conf")
    check(config.dataDir?.path == "/tmp/data", "data folder from launch.conf")
    config.saveDir = URL(fileURLWithPath: tricky[1])
    config.gameDir = URL(fileURLWithPath: tricky[2])
    config.showLauncher = false
    config.apply(to: &conf)
    check(LaunchConfig(ini: conf) == config, "launch.conf round trip")
    check(conf["BB_FPS"] == "uncap" && conf["BB_DATA_DIR"] == "/tmp/data", "other assignments kept")

    let url = FileManager.default.temporaryDirectory.appendingPathComponent("launcher-conf-\(getpid()).conf")
    try? conf.write(to: url)
    let bash = Process()
    bash.executableURL = URL(fileURLWithPath: "/bin/bash")
    bash.arguments = ["-c", "source \"$1\" && printf '%s\\n%s\\n%s' \"$BB_GAME_DIR\" \"$BB_SAVE_DIR\" \"$BB_SHOW_LAUNCHER\"", "_", url.path]
    let pipe = Pipe()
    bash.standardOutput = pipe
    try? bash.run()
    bash.waitUntilExit()
    let sourced = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
    check(bash.terminationStatus == 0 && sourced == "\(tricky[2])\n\(tricky[1])\n0", "bash sources launch.conf: \(sourced.debugDescription)")
    try? FileManager.default.removeItem(at: url)
    config.saveDir = nil
    config.apply(to: &conf)
    check(conf["BB_SAVE_DIR"] == nil, "default save folder: no key")
}

// SaveFolder (review focus 4).
do {
    let fm = FileManager.default
    let root = fm.temporaryDirectory.appendingPathComponent("launcher-saves-\(getpid())")
    let old = root.appendingPathComponent("old"), new = root.appendingPathComponent("new folder")
    try? fm.createDirectory(at: old.appendingPathComponent("1/CUSA00207/SPRJ0005"), withIntermediateDirectories: true)
    try? Data("save".utf8).write(to: old.appendingPathComponent("1/CUSA00207/SPRJ0005/userdata0000"))
    check(SaveFolder.defaultURL(dataDir: URL(fileURLWithPath: "/d")).path == "/d/user/savedata", "default save folder")
    check(SaveFolder.hasSaves(old) && !SaveFolder.hasSaves(new), "has saves")
    check(SaveFolder.shouldOfferCopy(from: old, to: new), "offer copy to an empty folder")
    check(!SaveFolder.shouldOfferCopy(from: old, to: old) && !SaveFolder.shouldOfferCopy(from: old, to: URL(fileURLWithPath: old.path + "/")), "same folder: no offer")
    check(!SaveFolder.shouldOfferCopy(from: new, to: old), "nothing to copy: no offer")
    try? fm.createDirectory(at: new.appendingPathComponent("1"), withIntermediateDirectories: true)
    try? Data("mine".utf8).write(to: new.appendingPathComponent("1/keep"))
    check(!SaveFolder.shouldOfferCopy(from: old, to: new), "target has saves: no offer")
    do { try SaveFolder.copySaves(from: old, to: new) } catch { check(false, "copy: \(error)") }
    check(fm.fileExists(atPath: old.appendingPathComponent("1/CUSA00207/SPRJ0005/userdata0000").path), "original kept")
    check((try? String(contentsOf: new.appendingPathComponent("1/keep"), encoding: .utf8)) == "mine", "existing file not overwritten")
    check(SaveFolder.problem(root.appendingPathComponent("made"), create: true) == nil, "creates a writable folder")
    check(SaveFolder.problem(URL(fileURLWithPath: "/Volumes/no-such-drive-\(getpid())/saves"), create: false) != nil, "missing folder reported")
    try? fm.removeItem(at: root)
}

// Layout and launch plans.
do {
    let fm = FileManager.default
    let root = fm.temporaryDirectory.appendingPathComponent("launcher-layout-\(getpid())")
    let app = root.appendingPathComponent("Bloodborne.app")
    let contents = app.appendingPathComponent("Contents")
    let port = contents.appendingPathComponent("Resources/port")
    try? fm.createDirectory(at: port, withIntermediateDirectories: true)
    check(Layout.detect(bundleURL: app, info: [:]) == nil, "no run.sh, no repository: not a launcher bundle")
    let repo = root.appendingPathComponent("my repo")
    try? fm.createDirectory(at: repo, withIntermediateDirectories: true)
    let checkout = Layout.detect(bundleURL: app, info: ["BBRepository": repo.path])
    check(checkout == .checkout(repo: repo, app: app), "checkout app")
    try? Data().write(to: port.appendingPathComponent("run.sh"))
    let packaged = Layout.detect(bundleURL: app, info: ["BBRepository": repo.path])
    check(packaged == .packaged(contents: contents), "packaged app (run.sh inside wins)")


    let home = URL(fileURLWithPath: "/Users/h")
    let defaultData = home.appendingPathComponent("Library/Application Support/Bloodborne")
    let env = ["HOME": home.path, "PATH": "/usr/bin:/bin"]
    check(packaged!.launchConfURL(env: env) == defaultData.appendingPathComponent("launch.conf"), "packaged launch.conf in the data folder")
    check(checkout!.launchConfURL(env: env) == repo.appendingPathComponent("launch.conf"), "checkout launch.conf in the repository")
    check(checkout!.launchConfURL(env: env.merging(["BB_LAUNCH_CONF": "/tmp/t.conf"]) { $1 }).path == "/tmp/t.conf", "BB_LAUNCH_CONF")
    check(packaged!.dataDir(env: env, config: LaunchConfig()) == defaultData, "default data folder")
    check(packaged!.dataDir(env: env.merging(["BB_DATA_DIR": "/x"]) { $1 }, config: LaunchConfig()).path == "/x", "BB_DATA_DIR")
    check(checkout!.dataDir(env: env, config: LaunchConfig(ini: IniFile(text: "BB_DATA_DIR=/y\n"))).path == "/y", "launch.conf BB_DATA_DIR")
    check(Logs.folder(home: home).path == "/Users/h/Library/Logs/Bloodborne", "logs folder")

    let game = URL(fileURLWithPath: "/Games/My CUSA03173")
    let p = packaged!.launchPlan(gameDir: game, saveDir: nil, dataDir: defaultData, env: env)
    check(p.executable.path == "/bin/bash" && p.arguments == [port.appendingPathComponent("run.sh").path], "packaged runs its run.sh")
    check(p.environment["BB_GAME_DIR"] == game.path && p.environment["BB_DATA_DIR"] == defaultData.path, "game and data folder")
    check(p.environment["BB_LAUNCH_CONF"] == packaged!.launchConfURL(env: env).path, "launch.conf for the in-game menu")
    check(p.environment["BB_PREBUILT"] == "1" && p.environment["BB_PROBE"] == contents.appendingPathComponent("MacOS/bb-probe").path, "prebuilt probe")
    check(p.environment["PYTHON"] == contents.appendingPathComponent("Resources/python/bin/python3").path, "bundled Python")
    check(p.environment["VK_DRIVER_FILES"] == nil, "no Vulkan driver (Metal renderer)")
    check(p.environment["BB_SAVE_DIR"] == nil && p.directory.path == port.path, "default saves; runs in port")
    let saves = URL(fileURLWithPath: "/Volumes/Drive/Hunter's saves")
    let c = checkout!.launchPlan(gameDir: game, saveDir: saves, dataDir: defaultData, env: env.merging(["BB_SAVE_DIR": "/stale"]) { $1 })
    check(c.arguments == [repo.appendingPathComponent("run.sh").path] && c.directory.path == repo.path, "checkout runs the repository's run.sh")
    check(c.environment["BB_SAVE_DIR"] == saves.path && c.environment["BB_PREBUILT"] == nil, "chosen saves; builds")
    check(c.environment["BB_GPU_HOST"] == app.appendingPathComponent("Contents/MacOS/bbgpu-host").path, "game window from the app (Dock icon)")
    check(c.environment["BB_DATA_DIR"] == nil, "checkout: run.sh picks the data folder unless set")
    check(c.environment["PATH"]?.hasPrefix("/opt/homebrew/bin:") == true, "checkout: Homebrew tools for the rebuild")
    let cd = checkout!.launchPlan(gameDir: game, saveDir: nil, dataDir: defaultData, env: env.merging(["BB_DATA_DIR": "/z"]) { $1 })
    check(cd.environment["BB_DATA_DIR"] == "/z" && cd.environment["BB_SAVE_DIR"] == nil, "explicit data folder passes; stale save folder removed")
    check(packaged!.gameCheckCommand == [contents.appendingPathComponent("Resources/python/bin/python3").path, "-B", port.appendingPathComponent("scripts/game_check.py").path], "packaged game check")
    check(checkout!.gameCheckCommand == ["/usr/bin/env", "python3", "-B", repo.appendingPathComponent("scripts/game_check.py").path], "checkout game check")
    try? fm.createDirectory(at: root.appendingPathComponent("CUSA03173"), withIntermediateDirectories: true)
    try? Data().write(to: root.appendingPathComponent("CUSA03173/eboot.bin"))
    check(checkout!.defaultGameDir()?.path == root.appendingPathComponent("CUSA03173").path, "checkout default: ../CUSA03173")
    check(packaged!.defaultGameDir() == nil, "packaged: no default game folder")
    try? fm.removeItem(at: root)
}

// CrashReport: the cause line, summaries, excerpt, redaction, the issue link.
do {
    let mac = SystemInfo(appVersion: "0.5.3", model: "Mac16,5", chip: "Apple M4 Max", memoryGB: 48, macOS: "Version 26.6 (Build 25G62)")
    let home = "/Users/hunter"
    var log = (1...200).map { "line \($0)" }
    log[120] = "Crash: the GPU process crashed (signal 11 in Foo)"
    log[121] = "bbgpu-host: fatal fault (signal 11) at /Users/hunter/Applications/Bloodborne.app/x+0x10"
    log[150] = "Crash: the GPU process stopped unexpectedly (host process killed by signal 11)"
    let crash = CrashReport(status: 1, log: log.joined(separator: "\n"), logName: "game.log", system: mac,
                            settings: ["output 2560x1440"], home: home)
    check(crash.cause == "the GPU process crashed (signal 11 in Foo)", "first Crash: line is the cause")
    check(crash.summary == "The GPU process crashed (signal 11 in Foo).", "summary sentence")
    check(crash.report.contains("line 81\n") && crash.report.contains("line 160\n") && !crash.report.contains("line 80\n")
          && !crash.report.contains("line 161\n"), "excerpt: 40 lines either side of the cause")
    check(!crash.report.contains("hunter") && crash.report.contains("~/Applications/Bloodborne.app"), "home folder redacted")
    check(crash.report.contains("Mac16,5 (Apple M4 Max), 48 GB, macOS 26.6 (Build 25G62)") && crash.report.contains("output 2560x1440"), "system and settings")

    let stop = CrashReport(status: 21, log: "a\nSTOP: unsupported TLS module id\nb", logName: "game.log", system: mac, settings: [], home: home)
    check(stop.cause == "unsupported TLS module id" && stop.summary == "Unsupported TLS module id.", "STOP: line when there is no Crash: line")
    let signal = CrashReport(status: 139, log: "nothing\nuseful", logName: "game.log", system: mac, settings: [], home: home)
    check(signal.cause == nil && signal.summary == "The game stopped unexpectedly (signal 11)." && signal.report.contains("nothing\nuseful"), "signal status, end of log")
    check(CrashReport.redact("/Users/someone/x and /Users/hunter/y", home: home) == "~/x and ~/y", "other user folders redacted")

    let query = URLComponents(url: crash.issueURL, resolvingAgainstBaseURL: false)!.queryItems ?? []
    check(crash.issueURL.absoluteString.hasPrefix("https://github.com/Wooloomooloo2/bloodborne_mac_port/issues/new?"), "issue link")
    check(query.first { $0.name == "title" }?.value == "Crash: the GPU process crashed (signal 11 in Foo)", "issue title")
    check(query.first { $0.name == "body" }?.value?.contains("Apple M4 Max") == true, "issue body has the Mac")
}

// Versions and the GitHub releases check.
do {
    let v = { (s: String) in ReleaseVersion(s)! }
    check(v("mac-v0.5.2.2").numbers == [0, 5, 2, 2] && v("mac-v0.5.2.2").isPlain, "tag to version")
    check(v("0.5.2-test1").numbers == [0, 5, 2] && v("0.5.2-test1").suffix == "-test1" && !v("0.5.2-test1").isPlain, "test build")
    check(v("0.5.2-5-g0b50589").numbers == [0, 5, 2], "checkout build (git describe)")
    check(ReleaseVersion("dev-0b50589") == nil && ReleaseVersion("") == nil && ReleaseVersion("mac-v") == nil, "no number: no version")
    check(v("0.5.2.1").isNewer(than: v("0.5.2")) && v("0.5.10").isNewer(than: v("0.5.9")) && v("1.0").isNewer(than: v("0.9.9")), "newer")
    check(!v("0.5.2").isNewer(than: v("0.5.2.0")) && !v("0.5.2.0").isNewer(than: v("0.5.2")), "trailing zero is equal")
    check(v("0.5.2").isNewer(than: v("0.5.2-test1")) && !v("0.5.2-test1").isNewer(than: v("0.5.2")), "test build before its release")
    check(v("0.5.2-test2").isNewer(than: v("0.5.2-test1")) && v("0.5.2.1").isNewer(than: v("0.5.2-5-g0b50589")), "test builds in order; next release after a checkout")
    check(v("0.5.2-5-g0b50589").isNewer(than: v("0.5.2")) && !v("0.5.2").isNewer(than: v("0.5.2-5-g0b50589-dirty")), "checkout build after its tag")
    check(!v("0.5.2").isNewer(than: v("0.5.2-dirty")) && !v("0.5.2-test1").isNewer(than: v("0.5.2-test1")), "dirty tag build; equal")

    func release(_ tag: String, draft: Bool = false, prerelease: Bool = false, asset: String? = nil) -> String {
        let name = asset ?? "Bloodborne-macOS-\(tag.dropFirst(5)).zip"
        return """
        {"tag_name":"\(tag)","name":"\(tag): notes","body":"**Fixed** things","draft":\(draft),"prerelease":\(prerelease),
         "html_url":"https://github.com/o/r/releases/tag/\(tag)",
         "assets":[{"name":"pkg_extract-macOS.zip","size":1,"state":"uploaded","digest":null,"browser_download_url":"https://x/p.zip"},
                   {"name":"\(name)","size":51225450,"state":"uploaded","digest":"sha256:ab","browser_download_url":"https://x/\(name)"}]}
        """
    }
    // The API's order: newest published first; 0.5.2 was published after its fix releases.
    let json = Data(("[" + [release("mac-v0.5.2.2", prerelease: true), release("mac-v0.6.0-test1"),
                            release("mac-v0.7.0", draft: true), release("mac-v0.9.0", asset: "Other.zip"), release("v1.0"),
                            release("mac-v0.5.2"), release("mac-v0.5.2.1", prerelease: true), release("mac-v0.4.0")]
        .joined(separator: ",") + "]").utf8)
    // Pre-releases (GitHub's flag: 0.5.2.1, 0.5.2.2) and test tags (0.6.0-test1) are test builds.
    let all = (try? Updates.releases(json: json, includeTestBuilds: false)) ?? []
    check(all.map(\.tag) == ["mac-v0.5.2", "mac-v0.4.0"], "full releases only, newest version first: \(all.map(\.tag))")
    check(all.first?.zip.absoluteString == "https://x/Bloodborne-macOS-0.5.2.zip" && all.first?.size == 51225450, "the app's zip, not pkg_extract")
    check(all.first?.digest == "sha256:ab" && all.first?.title == "mac-v0.5.2: notes" && all.first?.notes == "**Fixed** things", "release details")
    check(all.allSatisfy { !$0.isTestBuild }, "full releases are not test builds")
    let tests = (try? Updates.releases(json: json, includeTestBuilds: true)) ?? []
    check(tests.map(\.tag) == ["mac-v0.6.0-test1", "mac-v0.5.2.2", "mac-v0.5.2.1", "mac-v0.5.2", "mac-v0.4.0"], "test builds when asked: \(tests.map(\.tag))")
    check(tests.prefix(3).allSatisfy(\.isTestBuild), "pre-releases and test tags marked")
    func newest(_ current: String?, tests: Bool = false, skipping: String? = nil) -> String? {
        (try? Updates.newest(json: json, current: current.flatMap(ReleaseVersion.init), includeTestBuilds: tests, skipping: skipping))??.tag
    }
    check(newest("0.4.0") == "mac-v0.5.2", "newer release offered")
    check(newest("0.5.2") == nil, "up to date: the 0.5.2.x pre-releases are not offered by default")
    check(newest("0.5.2", tests: true) == "mac-v0.6.0-test1", "pre-releases and test builds when asked")
    check(newest("0.5.2-test1") == "mac-v0.5.2", "a test build is offered its release")
    check(newest("0.5.2-5-g0b50589") == nil && newest("0.5.2-5-g0b50589", tests: true) == "mac-v0.6.0-test1", "checkout build: only later releases")
    check(newest("0.4.0", skipping: "0.5.2") == nil, "skipped version")
    check(newest(nil) == "mac-v0.5.2", "dev build: newest release")
    check((try? Updates.releases(json: Data("{\"message\":\"rate limited\"}".utf8), includeTestBuilds: false)) == nil, "API error is an error")

    let now = Date(timeIntervalSince1970: 2_000_000_000)
    check(Updates.isDue(last: nil, now: now) && Updates.isDue(last: now - Updates.interval, now: now), "due")
    check(!Updates.isDue(last: now - 3600, now: now) && Updates.isDue(last: now + 3600, now: now), "not due; clock set back")

    let info: [String: Any] = ["CFBundleIdentifier": Updates.bundleIdentifier, "CFBundleVersion": "0.5.2.2"]
    check(Updates.unpackedProblem(info: info, expected: v("0.5.2.2")) == nil, "unpacked app accepted")
    check(Updates.unpackedProblem(info: info, expected: v("0.5.3")) != nil, "wrong version refused")
    check(Updates.unpackedProblem(info: ["CFBundleIdentifier": "com.example", "CFBundleVersion": "0.5.2.2"], expected: v("0.5.2.2")) != nil, "other app refused")

    let fm = FileManager.default
    let root = fm.temporaryDirectory.appendingPathComponent("launcher-update-\(getpid())")
    try? fm.createDirectory(at: root, withIntermediateDirectories: true)
    let app = root.appendingPathComponent("Bloodborne.app")
    let packaged = Layout.packaged(contents: app.appendingPathComponent("Contents"))
    check(Updates.installProblem(layout: packaged, bundle: app) == nil, "packaged app in a writable folder installs")
    check(Updates.installProblem(layout: .checkout(repo: root, app: app), bundle: app) != nil, "checkout app: git, not the updater")
    check(Updates.installProblem(layout: packaged, bundle: URL(fileURLWithPath: "/private/var/folders/x/AppTranslocation/1/d/Bloodborne.app")) != nil, "translocated copy")
    check(Updates.installProblem(layout: packaged, bundle: URL(fileURLWithPath: "/System/Bloodborne.app")) != nil, "read-only folder")
    try? fm.removeItem(at: root)

    // launch.conf keeps the update preferences.
    var conf = IniFile(text: "BB_GAME_DIR=/g\n")
    var config = LaunchConfig(ini: conf)
    check(config.checkUpdates == nil && !config.testBuilds && config.skippedVersion == nil && config.lastUpdateCheck == nil, "update defaults")
    config.checkUpdates = false
    config.testBuilds = true
    config.skippedVersion = "0.5.2.2"
    config.lastUpdateCheck = now
    config.apply(to: &conf)
    check(LaunchConfig(ini: conf) == config && LaunchConfig(ini: conf).checkUpdates == false, "update preferences round trip")
    check(conf["BB_UPDATE_CHECKED"] == "2000000000" && conf["BB_CHECK_UPDATES"] == "0", "update keys written")
    config.testBuilds = false
    config.skippedVersion = nil
    config.checkUpdates = nil
    config.apply(to: &conf)
    check(conf["BB_UPDATE_TEST_BUILDS"] == nil && conf["BB_UPDATE_SKIP"] == nil && conf["BB_CHECK_UPDATES"] == nil, "defaults leave no key (not asked yet)")
    check(LaunchConfig(ini: IniFile(text: "BB_CHECK_UPDATES=1\n")).checkUpdates == true, "agreed")
}

// High-resolution ultrawide selection on memory-constrained Macs.
do {
    let gib: UInt64 = 1024 * 1024 * 1024
    for size in [PixelSize(3440, 1440), PixelSize(5120, 1440)] {
        check(Resolution.ultrawideMemoryWarning(size, memoryBytes: 16 * gib) != nil, "16 GB wide memory warning")
        check(Resolution.ultrawideMemoryWarning(size, memoryBytes: 24 * gib) == nil, "24 GB boundary")
        check(Resolution.ultrawideMemoryWarning(size, memoryBytes: 64 * gib) == nil, "64 GB wide selection")
    }
    check(Resolution.ultrawideMemoryWarning(PixelSize(3840, 1080), memoryBytes: 16 * gib) == nil, "1080p wide selection")
}

// SessionLog: severity from the game's own line shapes (taken from a real game.log).
do {
    func c(_ line: String, after: LogEntry? = nil) -> String {
        let r = SessionLog.classify(line, previous: after)
        return "\(r.severity.label) \(r.source)"
    }
    check(c("Crash: the GPU process crashed (signal 11 in Foo)") == "CRIT crash", "Crash: is critical")
    check(c("STOP: unsupported TLS module id") == "ERROR stop", "STOP: is an error")
    check(c("ERROR: no eboot") == "ERROR game" && c("Runtime ERROR: bad map") == "ERROR runtime" && c("AUDIT ERROR: x") == "ERROR audit", "ERROR prefixes")
    check(c("GPU [Render.Vulkan] <Warning> vk_pipeline_serialization.cpp:369 LoadPipelineStage: skipping") == "WARN gpu.render.vulkan", "GPU warning")
    check(c("GPU [Render] <Error> x.cpp:1 F: y") == "ERROR gpu.render" && c("GPU [Lib] <Critical> z") == "CRIT gpu.lib", "GPU error, critical")
    check(c("GPU [Render] <Debug> a") == "DEBUG gpu.render" && c("GPU [Render] <Info> a") == "INFO gpu.render", "GPU debug, info")
    check(c("== Stall during rendering at flush 3") == "WARN gpu", "stall")
    check(c("bbgpu-host: ready: serving the submit ring") == "INFO bbgpu-host", "component line")
    check(c("GPU process: host pid 40643 ready in 16 ms") == "INFO gpu-process", "component with a space")
    check(c("Runtime: open(/app0/dvdroot_ps4/map/m01) failed") == "WARN runtime", "failed is a warning")
    check(c("Context lock: could not take the lock") == "WARN context-lock", "could not is a warning")
    check(c("Bloodborne™ | entry=0xa0 | image=91,042,036 bytes") == "INFO game", "no component")
    check(c("[FAST PATH] Tracing disabled (BB_TRACE=0): direct import dispatch active.") == "INFO game", "bracket is not a component")
    let crash = LogEntry(time: Date(), severity: .critical, source: "crash", message: "Crash: x")
    check(c("  #3 0x1234 in Foo", after: crash) == "CRIT crash", "indented continuation keeps the line before")
    check(c("Runtime: 0 errors found") == "INFO runtime", "no warning words")

    // Text and JSON lines.
    let tz = TimeZone(secondsFromGMT: 3 * 3600)!
    let t = Date(timeIntervalSince1970: 1_792_668_185.123) // 2026-10-22 11:23:05.123 UTC
    let entry = LogEntry(time: t, severity: .warning, source: "gpu.render", message: "GPU [Render] <Warning> a \"quoted\" /path")
    let text = SessionLog.textLine(entry, timeZone: tz)
    check(text == "2026-10-22T14:23:05.123+03:00 WARN   GPU [Render] <Warning> a \"quoted\" /path", "text line: \(text)")
    let back = SessionLog.parse(text)
    check(back.severity == .warning && back.message == entry.message && abs((back.time ?? .distantPast).timeIntervalSince(t)) < 0.001, "text line parses back")
    let utcText = SessionLog.textLine(entry, timeZone: TimeZone(secondsFromGMT: 0)!)
    check(utcText.hasPrefix("2026-10-22T11:23:05.123+00:00 WARN "), "UTC+0 keeps the offset form: \(utcText)")
    check(SessionLog.parse("Runtime: plain old line") == (nil, nil, "Runtime: plain old line") as (Date?, LogSeverity?, String), "older log line is all message")
    let json = SessionLog.jsonLine(entry, session: .init(id: "game-2026-10-22_14-23-05", appVersion: "0.5.4"))
    let object = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
    check(object["@timestamp"] as? String == "2026-10-22T11:23:05.123Z" && object["log.level"] as? String == "warning", "JSON time (UTC) and level: \(json)")
    check(object["syslog.severity.code"] as? Int == 4 && object["log.logger"] as? String == "gpu.render", "JSON syslog code and source")
    check(object["message"] as? String == entry.message && object["session.id"] as? String == "game-2026-10-22_14-23-05"
          && object["service.version"] as? String == "0.5.4", "JSON message and session")
    check(!json.contains("\n") && json.hasPrefix("{\"@timestamp\""), "one line, stable key order")
    check(SessionLog.name(start: t, timeZone: tz) == "game-2026-10-22_14-23-05", "session name in local time")

    // Files: migration of the old game.log, links, two starts in one second, retention.
    let fm = FileManager.default
    let folder = fm.temporaryDirectory.appendingPathComponent("launcher-logs-\(getpid())")
    try? fm.removeItem(at: folder)
    try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
    try? Data("old latest\n".utf8).write(to: folder.appendingPathComponent("game.log"))
    try? Data("old previous\n".utf8).write(to: folder.appendingPathComponent("game.previous.log"))
    for (name, age) in [("game.log", 3600.0), ("game.previous.log", 7200.0)] { // written before the sessions below
        try? fm.setAttributes([.modificationDate: t.addingTimeInterval(-age)], ofItemAtPath: folder.appendingPathComponent(name).path)
    }
    let first = try? SessionLog.begin(in: folder, start: t)
    let migrated = SessionLog.sessions(in: folder).filter { $0 != first?.text }
    let migratedText = migrated.compactMap { try? String(contentsOf: $0, encoding: .utf8) }.sorted()
    check(migratedText == ["old latest\n", "old previous\n"], "old logs kept as dated sessions: \(migrated.map(\.lastPathComponent))")
    check(first.map { fm.fileExists(atPath: $0.text.path) && fm.fileExists(atPath: $0.json.path) } == true, "session files created")
    check((try? fm.destinationOfSymbolicLink(atPath: folder.appendingPathComponent("game.log").path)) == first?.text.lastPathComponent, "game.log links to the session")
    let second = try? SessionLog.begin(in: folder, start: t)
    check(second?.text.lastPathComponent == SessionLog.name(start: t) + "-2.log", "two starts in one second")
    check((try? fm.destinationOfSymbolicLink(atPath: folder.appendingPathComponent("game.previous.log").path)) == first?.text.lastPathComponent, "game.previous.log links to the one before")
    for i in 0..<30 { _ = try? SessionLog.begin(in: folder, start: t.addingTimeInterval(Double(i + 1) * 60)) }
    let kept = SessionLog.sessions(in: folder)
    check(kept.count == SessionLog.keep, "keeps \(SessionLog.keep) sessions: \(kept.count)")
    let jsonCount = ((try? fm.contentsOfDirectory(atPath: folder.path)) ?? []).filter { $0.hasSuffix(".jsonl") }.count
    check(jsonCount == SessionLog.keep, "their JSON logs too (\(jsonCount))")
    try? fm.removeItem(at: folder)

    // The size budget: the oldest sessions go first, whatever the count.
    try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
    var made: [URL] = []
    for i in 0..<5 {
        if let files = try? SessionLog.begin(in: folder, start: t.addingTimeInterval(Double(i) * 60), budget: 2_500) {
            try? Data(count: 600).write(to: files.text)
            try? Data(count: 400).write(to: files.json)
            made.append(files.text)
        }
    }
    let afterBudget = SessionLog.sessions(in: folder).map(\.lastPathComponent)
    check(afterBudget == made.suffix(3).map(\.lastPathComponent), "budget keeps the newest sessions that fit: \(afterBudget)")
    try? fm.removeItem(at: folder)

    // Crash report from a timestamped session log.
    let mac = SystemInfo(appVersion: "0.5.4", model: "Mac15,8", chip: "Apple M3 Max", memoryGB: 64, macOS: "Version 26.6 (Build 25G62)")
    let lines: [LogEntry] = [
        LogEntry(time: t, severity: .notice, source: "launcher", message: "launcher: Bloodborne for Mac 0.5.4"),
        LogEntry(time: t + 1, severity: .warning, source: "gpu.render", message: "GPU [Render] <Warning> slow pipeline"),
        LogEntry(time: t + 2, severity: .info, source: "runtime", message: "Runtime: loading /Users/hunter/save"),
        LogEntry(time: t + 3, severity: .warning, source: "gpu.render", message: "GPU [Render] <Warning> slow pipeline"),
        LogEntry(time: t + 4, severity: .error, source: "runtime", message: "Runtime ERROR: bad map"),
        LogEntry(time: t + 5, severity: .critical, source: "crash", message: "Crash: the GPU process crashed (signal 11)"),
        LogEntry(time: t + 6, severity: .error, source: "launcher", message: "launcher: the game stopped with status 1 after 0 min 6 s"),
    ]
    let logText = lines.map { SessionLog.textLine($0, timeZone: tz) }.joined(separator: "\n") + "\n"
    let report = CrashReport(status: 1, log: logText, logName: "game-2026-10-22_14-23-05.log", system: mac, settings: [],
                             home: "/Users/hunter", started: t, ended: t + 366, timeZone: tz)
    check(report.cause == "the GPU process crashed (signal 11)", "cause found behind the timestamp")
    check(report.report.contains("- Session: started 2026-10-22 14:23:05 +03:00, ran 6 min 6 s"), "session line")
    check(report.report.contains("(at 14:23:10.123)") && report.report.contains("7 lines: 1 critical, 2 errors, 2 warnings"), "cause time and counts")
    check(report.report.contains("14:23:08.123 WARN   GPU [Render] <Warning> slow pipeline  (×2)"), "repeated warning listed once with its count")
    check(report.report.contains("14:23:09.123 ERROR  Runtime ERROR: bad map") && report.report.contains("~/save") && !report.report.contains("hunter"), "excerpt with time and level, redacted")
}

// NativeCrash: macOS .ips reports of the game's processes, and the issue link.
do {
    let header = #"{"app_name":"bbgpu-host","timestamp":"2026-10-22 14:09:41.00 +0300","bug_type":"309","name":"bbgpu-host"}"#
    let body = #"""
    {"procName":"bbgpu-host","procPath":"\/Users\/hunter\/Applications\/Bloodborne.app\/Contents\/MacOS\/bbgpu-host",
     "crashReporterKey":"49B0A095-3C99-8D24-C7F9-C2BD20AB1301","sleepWakeUUID":"9035D3E7","bootSessionUUID":"B92C7B86",
     "exception":{"type":"EXC_BAD_ACCESS","signal":"SIGSEGV","subtype":"KERN_INVALID_ADDRESS at 0x0000000000000010"},
     "termination":{"namespace":"SIGNAL","indicator":"Segmentation fault: 11"},
     "threads":[{"frames":[{"imageOffset":4096,"imageIndex":0}]},
                {"triggered":true,"frames":[{"imageOffset":1234,"symbol":"Vulkan::Renderer::Draw()","symbolLocation":120,"imageIndex":1},
                                            {"imageOffset":4660,"imageIndex":0}]}],
     "usedImages":[{"name":"bbgpu-host"},{"name":"libbbgpu.dylib"}]}
    """#
    let crash = NativeCrash(ips: header + "\n" + body, file: "bbgpu-host-2026-10-22-140941.ips")
    check(crash?.process == "bbgpu-host" && crash?.exception == "EXC_BAD_ACCESS (SIGSEGV), KERN_INVALID_ADDRESS at 0x0000000000000010, Segmentation fault: 11", "exception: \(crash?.exception ?? "nil")")
    check(crash?.frames == ["0  libbbgpu.dylib           Vulkan::Renderer::Draw() + 120", "1  bbgpu-host               0x1234"], "crashed thread's frames: \(crash?.frames ?? [])")
    check(NativeCrash(ips: #"{"app_name":"Safari"}"# + "\n" + #"{"procName":"Safari"}"#, file: "x.ips") == nil, "other apps' reports ignored")
    check(NativeCrash(ips: "not json", file: "x.ips") == nil, "not an .ips")
    let scrubbed = NativeCrash.scrub(header + "\n" + body, home: "/Users/hunter")
    check(!scrubbed.contains("49B0A095") && !scrubbed.contains("9035D3E7") && scrubbed.contains("\"crashReporterKey\":\"removed\"")
          && !scrubbed.contains("hunter") && scrubbed.contains("~/Applications"), "identifiers and home removed from the zip's copy")

    let fm = FileManager.default
    let folder = fm.temporaryDirectory.appendingPathComponent("launcher-ips-\(getpid())")
    try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
    let now = Date()
    for (name, age) in [("bbgpu-host-2026-10-22-140941.ips", 10.0), ("bb-probe-2026-10-22-140942.ips", 5.0),
                        ("bbgpu-host-macos27-2026-10-22-140943.ips", 1.0), ("Safari-2026-10-22-140941.ips", 1.0),
                        ("bbgpu-host-2026-10-21-090000.ips", 86_400.0), ("bb-probe.diag", 1.0)] {
        let url = folder.appendingPathComponent(name)
        try? Data("x".utf8).write(to: url)
        try? fm.setAttributes([.modificationDate: now.addingTimeInterval(-age)], ofItemAtPath: url.path)
    }
    let found = NativeCrash.find(in: folder, since: now.addingTimeInterval(-60), until: now).map(\.lastPathComponent)
    check(found == ["bbgpu-host-2026-10-22-140941.ips", "bb-probe-2026-10-22-140942.ips", "bbgpu-host-macos27-2026-10-22-140943.ips"],
          "the session's reports of the game's processes, oldest first: \(found)")
    try? fm.removeItem(at: folder)

    let mac = SystemInfo(appVersion: "0.5.4", model: "Mac15,8", chip: "Apple M3 Max", memoryGB: 64, macOS: "Version 26.6 (Build 25G62)")
    let short = CrashReport(status: 139, log: "Runtime: hello\nCrash: the GPU process crashed (signal 11)\n", logName: "game.log",
                            system: mac, settings: [], home: "/Users/hunter", natives: [crash!])
    check(short.report.contains("- macOS crash report: bbgpu-host: EXC_BAD_ACCESS (SIGSEGV)") && short.report.contains("Vulkan::Renderer::Draw() + 120"), "native crash in the report")
    let link = short.issueLink(bundle: "crash-2026-10-22_14-03-05.zip")
    let linkBody = URLComponents(url: link.url, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == "body" }?.value ?? ""
    check(link.inline && linkBody.contains("Vulkan::Renderer::Draw()") && linkBody.contains("crash-2026-10-22_14-03-05.zip")
          && linkBody.contains("What were you doing"), "short report goes into the issue")
    let longLog = (1...400).map { "Runtime: line \($0) " + String(repeating: "x", count: 120) }.joined(separator: "\n") + "\nCrash: boom\n"
    let long = CrashReport(status: 1, log: longLog, logName: "game.log", system: mac, settings: [], home: "/h")
    let longLink = long.issueLink()
    check(longLink.url.absoluteString.count <= CrashReport.maxIssueURLLength || !longLink.inline, "never a link over the limit")
    check(long.compactReport.count < long.report.count, "compact report is shorter")
    let huge = CrashReport(status: 1, log: (1...400).map { "GPU [R] <Warning> w\($0) " + String(repeating: "é ", count: 145) }.joined(separator: "\n") + "\nCrash: boom\n",
                           logName: "game.log", system: mac, settings: [], home: "/h")
    let hugeLink = huge.issueLink()
    let hugeBody = URLComponents(url: hugeLink.url, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == "body" }?.value ?? ""
    check(!hugeLink.inline && hugeBody.contains("paste it below") && hugeBody.contains("Apple M3 Max"), "too long: paste from the clipboard")
}

if failures > 0 {
    print("FAIL: launcher model, \(failures) failed")
    exit(1)
}
print("PASS: launcher model (ini, resolutions, settings, launch.conf, saves, launch plan, crash report, updates, session logs, native crashes)")
