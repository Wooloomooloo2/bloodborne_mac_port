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

do {
    check(AppUpdate.isNewer(remote: "mac-v0.5.4.1", local: "0.5.3"), "newer minor/patch version")
    check(AppUpdate.isNewer(remote: "0.5.4.1", local: "0.5.4"), "newer point version")
    check(!AppUpdate.isNewer(remote: "0.5.4.1", local: "0.5.4.1"), "same version is not newer")
    check(!AppUpdate.isNewer(remote: "0.5.3", local: "0.5.4.1"), "older version is not newer")
    check(!AppUpdate.isNewer(remote: "mac-v0.5.4.1", local: "development build"), "dev build ignored")
    check(AppUpdate.cleanVersion("mac-v0.5.4.1") == "0.5.4.1", "clean mac-v prefix")
    check(AppUpdate.cleanVersion("v1.0.0") == "1.0.0", "clean v prefix")
}

if failures > 0 {
    print("FAIL: launcher model, \(failures) failed")
    exit(1)
}
print("PASS: launcher model (ini, resolutions, settings, launch.conf, saves, launch plan, crash report, updates)")
