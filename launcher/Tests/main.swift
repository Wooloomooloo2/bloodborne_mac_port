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
    check(Resolution.ultrawideMemoryWarning(PixelSize(2560, 1600), memoryBytes: 16 * gib) != nil
          && Resolution.ultrawideMemoryWarning(PixelSize(1920, 1200), memoryBytes: 16 * gib) == nil, "16:10 memory warning")
    check(Resolution.wideLabel(PixelSize(1920, 1200)) == "1920 × 1200 · 16:10" && Resolution.wideLabel(PixelSize(3440, 1440)).hasSuffix("21:9")
          && Resolution.wideLabel(PixelSize(2560, 1080)).hasSuffix("21:9") && Resolution.wideLabel(PixelSize(5120, 1440)).hasSuffix("32:9"), "aspect labels")
    let sixteen = GameSettings(ini: IniFile(text: "ultrawide_res=2560x1600\n"))
    check(sixteen.ultrawide && sixteen.wideOutput == PixelSize(2560, 1600), "16:10 from bbport.ini")
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

// Benchmark: assets, per-pass launch plans and settings.
do {
    let fm = FileManager.default
    let root = fm.temporaryDirectory.appendingPathComponent("launcher-bench-\(getpid())")
    let app = root.appendingPathComponent("Bloodborne.app")
    let contents = app.appendingPathComponent("Contents")
    let repo = root.appendingPathComponent("repo")
    let packaged = Layout.packaged(contents: contents)
    let checkout = Layout.checkout(repo: repo, app: app)
    let env = ["HOME": "/Users/h", "PATH": "/usr/bin:/bin", "BB_FPS_LIMIT": "40", "BB_PRESENT_MODE": "Fifo"]
    check(packaged.benchmarkAssets(env: env).path == contents.appendingPathComponent("Resources/benchmark").path, "packaged benchmark assets")
    check(checkout.benchmarkAssets(env: env).path == repo.appendingPathComponent("benchmark").path, "checkout benchmark assets")
    check(checkout.benchmarkAssets(env: env.merging(["BB_BENCH_ASSETS": "/b"]) { $1 }).path == "/b", "BB_BENCH_ASSETS")

    let assets = root.appendingPathComponent("assets")
    check(Benchmark.assetsProblem(assets) != nil, "no assets: unavailable")
    try? fm.createDirectory(at: assets.appendingPathComponent("savedata/1/CUSA00207/SPRJ0005"), withIntermediateDirectories: true)
    check(Benchmark.assetsProblem(assets) != nil, "save without the route: unavailable")
    try? Data([1, 2]).write(to: assets.appendingPathComponent("route.pad"))
    try? Data([7]).write(to: assets.appendingPathComponent("savedata/1/CUSA00207/SPRJ0005/userdata0000"))
    check(Benchmark.assetsProblem(assets) == nil, "route and save: available")

    let work = root.appendingPathComponent("work"), out = root.appendingPathComponent("Benchmarks/run")
    let save = work.appendingPathComponent("savedata/1/CUSA00207/SPRJ0005/userdata0000")
    try? Benchmark.prepare(.traversal, assets: assets, work: work)
    check(fm.contents(atPath: save.path) == Data([7]), "fresh save copy")
    try? Data([9]).write(to: save)
    try? Benchmark.prepare(.throughput, assets: assets, work: work)
    check(fm.contents(atPath: save.path) == Data([7]), "the next pass starts from the original save again")
    let ini = IniFile(contentsOf: work.appendingPathComponent("benchmark.ini"))
    check(ini["fps_cap"] == "0" && ini["output_res"] == "1920x1080" && ini["upscaler"] == "off" && ini["preset"] == "0", "throughput ini")
    check(ini["show_fps"] == "1" && ini["live_resolution"] == "0" && ini["fullscreen"] == "0" && ini["metal_hud"] == "0", "throughput ini display")
    check(ini["skip_intro"] == "1" && ini["texture_compression"] == "1" && ini["effect_dof"] == "1" && ini["effect_ssr"] == "0", "intro skipped, effects at defaults")
    check(Benchmark.settings(for: .traversal)["fps_cap"] == "60", "traversal capped at 60")

    let game = URL(fileURLWithPath: "/Games/CUSA03173")
    let data = URL(fileURLWithPath: "/Users/h/Library/Application Support/Bloodborne")
    let t = Benchmark.launchPlan(.traversal, layout: packaged, gameDir: game, dataDir: data, env: env, assets: assets, work: work, out: out).environment
    check(t["BB_BENCHMARK"] == "traversal" && t["BB_BENCH_OUT"] == out.appendingPathComponent("traversal.csv").path, "traversal mode and CSV")
    check(t["BB_PAD_REPLAY"] == assets.appendingPathComponent("route.pad").path && t["BB_FPS"] == "60" && t["BB_FRAME_STATS"] == "1", "traversal replay")
    check(t["BB_MODS_ENABLED"] == "0" && t["BB_PATCHES_DIR"] == work.appendingPathComponent("no-patches").path, "no mods or patch files")
    check(t["BB_SAVE_DIR"] == work.appendingPathComponent("savedata").path && t["BB_CONFIG"] == work.appendingPathComponent("benchmark.ini").path, "benchmark save and settings, not the player's")
    check(t["BB_FPS_LIMIT"] == nil && t["BB_PRESENT_MODE"] == nil && t["BB_VBLANK_HZ"] == nil && t["BB_BENCH_SECONDS"] == nil, "inherited pacing variables removed")
    check(t["BB_GAME_DIR"] == game.path && t["BB_DATA_DIR"] == data.path && t["BB_PREBUILT"] == "1", "the normal launch underneath (warm shader cache)")
    let p = Benchmark.launchPlan(.throughput, layout: checkout, gameDir: game, dataDir: data, env: env, assets: assets, work: work, out: out)
    let e = p.environment
    check(e["BB_BENCHMARK"] == "throughput" && e["BB_BENCH_OUT"] == out.appendingPathComponent("throughput.csv").path && e["BB_BENCH_SECONDS"] == "30", "throughput mode")
    check(e["BB_FPS"] == "uncap" && e["BB_FPS_LIMIT"] == "0" && e["BB_VBLANK_HZ"] == "480" && e["BB_PRESENT_MODE"] == "Immediate", "throughput uncapped")
    check(e["BB_PAD_REPLAY"] == nil && e["BB_SAVE_DIR"] == work.appendingPathComponent("savedata").path, "throughput: no replay, benchmark save")
    check(p.arguments == [repo.appendingPathComponent("run.sh").path], "checkout run.sh")
    check(Benchmark.runFolderName(Date(timeIntervalSince1970: 0)).count == "yyyy-MM-dd HH.mm.ss".count, "run folder name")
    try? fm.removeItem(at: root)
}

// Benchmark: the game's CSV.
do {
    let header = "t_ms,frame_ms,game_ms,gpu_ms,draws,compiles,compile_wait_ms,upload_mb,game_cpu_ms,gpu_thread_ms,ahead_ms,game_faults\n"
    let ok = BenchmarkCSV(text: "# bloodborne-benchmark 1\n# pass=traversal\n# started=1729800000.123\n# output=1920x1080\n" + header
        + "16.7,16.7,16.6,9.1,4210,0,0.0,0.4,21.5,5.9,0.0,0\n33.4,16.7,16.6,9.0,4200,2,3.5,0.0,21.0,6.0,0.0,1\n# ended=1729800130.456\n# result=ok\n")
    check(ok.outcome == .ok && ok.frames.count == 2 && ok.meta["pass"] == "traversal" && ok.meta["format"] == "1", "ok pass")
    check(ok.started == 1729800000.123 && ok.ended == 1729800130.456, "measuring window")
    check(ok.frames[1] == BenchmarkFrame(t: 33.4, frame: 16.7, game: 16.6, gpu: 9.0, draws: 4200, compiles: 2, compileWait: 3.5,
                                         upload: 0, gameCPU: 21.0, gpuThread: 6.0, ahead: 0, faults: 1), "row fields")
    let error = BenchmarkCSV(text: "# bloodborne-benchmark 1\n# pass=throughput\n" + header + "# result=error: no gameplay after 150 s\n")
    check(error.outcome == .error("no gameplay after 150 s") && error.frames.isEmpty, "error pass")
    let cut = BenchmarkCSV(text: "# pass=traversal\n" + header + "16.7,16.7,16.6,9.1,4210,0,0.0,0.4,21.5,5.9,0.0,0\n33.4,16.7,16.6,9.")
    check(cut.outcome == .interrupted && cut.frames.count == 1, "truncated: interrupted, the cut row skipped")
    check(BenchmarkCSV(text: "").outcome == .interrupted, "no file: interrupted")
    check(BenchmarkCSV(text: "frame_ms,t_ms\n20,20\n# result=ok\n").frames == [BenchmarkFrame(t: 20, frame: 20)], "columns by name")
}

// Benchmark: metrics on a known frame list, system samples, the report.
do {
    // 200 frames of 10 ms (GPU 5 ms) with three hitches: a 100 ms compile, a 40 ms stream, a 30 ms GPU frame.
    var frames: [BenchmarkFrame] = []
    var t = 0.0
    for i in 0..<200 {
        var f = BenchmarkFrame(frame: 10, game: 9, gpu: 5)
        if i == 50 { f.frame = 100; f.compileWait = 50; f.compiles = 3 }
        if i == 100 { f.frame = 40; f.upload = 40 }
        if i == 150 { f.frame = 30; f.gpu = 28 }
        t += f.frame
        f.t = t
        frames.append(f)
    }
    let m = FrameMetrics(frames)!
    func near(_ a: Double, _ b: Double) -> Bool { abs(a - b) < 0.01 }
    check(m.frames == 200 && near(m.seconds, 2.14) && near(m.averageFPS, 200 / 2.14), "average FPS: \(m.averageFPS)")
    check(m.maxFPS == 98, "best 1 s window: \(String(describing: m.maxFPS))")
    check(near(m.low1FPS, 1000 / 70) && near(m.low01FPS, 10), "1% low = slowest 2 frames, 0.1% = the slowest one")
    check(m.over33 == 2 && m.over50 == 1 && m.over100 == 0 && m.longestMs == 100 && m.medianMs == 10, "stutter counts")
    check(m.hitchThresholdMs == 25 && m.hitches == 3 && near(m.hitchMsPerMinute, 140 / 2.14 * 60), "hitches: \(m.hitchMsPerMinute)")
    check(m.worstHitches.map(\.cause) == [.compile, .streaming, .gpu] && m.worstHitches.map(\.frameMs) == [100, 40, 30], "worst hitches, causes")
    check(near(m.worstHitches[0].timeS, 0.6), "hitch time into the route")
    check(near(m.gpuBusyPercent, 100 * 1023 / 2140) && near(m.gpuMsPerFrame, 1023 / 200.0) && m.compiles == 3 && m.compileWaitMs == 50, "GPU busy")
    check(FrameMetrics.hitch(BenchmarkFrame(frame: 60, game: 55, gpu: 10)).cause == .gameCPU, "game CPU hitch")
    check(FrameMetrics.hitch(BenchmarkFrame(frame: 60, game: 20, gpu: 10, ahead: 40)).cause == .gpu, "frames-ahead wait: GPU")
    check(FrameMetrics.hitch(BenchmarkFrame(frame: 60, game: 20, gpu: 10, faults: 25)).cause == .streaming, "faults: streaming")
    var typical = FrameMetrics.Typical()
    typical.upload = 14; typical.faults = 46
    check(FrameMetrics.hitch(BenchmarkFrame(frame: 60, game: 20, gpu: 10, upload: 15, faults: 50), typical).cause == .other,
          "typical uploads and faults are not streaming")
    check(FrameMetrics.hitch(BenchmarkFrame(frame: 60, game: 20, gpu: 10, upload: 120, faults: 50), typical).cause == .streaming,
          "a 120 MB upload is streaming")
    check(FrameMetrics.hitch(BenchmarkFrame(frame: 60, game: 20, gpu: 10)).cause == .other, "other")
    check(FrameMetrics([]) == nil, "no frames, no metrics")
    let short = FrameMetrics([BenchmarkFrame(t: 16, frame: 16)])!
    check(short.maxFPS == nil && near(short.low1FPS, 62.5) && short.hitches == 0, "a pass under 1 s")

    func sample(_ time: Double, cpu: Double?, mb: Double, swap: Double, ins: UInt64, pressure: Int = 1, pass: BenchmarkPass = .traversal) -> SystemSample {
        SystemSample(pass: pass, time: time, probe: ProcessSample(cpu: cpu, footprintMB: mb), host: nil, swapUsedMB: swap,
                     swapIns: ins, pressure: pressure, thermal: 0)
    }
    let samples = [sample(99, cpu: 500, mb: 9000, swap: 0, ins: 0, pressure: 4), sample(100, cpu: nil, mb: 2000, swap: 10, ins: 5),
                   sample(101, cpu: 150, mb: 3000, swap: 30, ins: 9, pressure: 2), sample(102, cpu: 250, mb: 2500, swap: 20, ins: 12),
                   sample(101, cpu: 900, mb: 1, swap: 0, ins: 0, pass: .throughput)]
    let s = SystemMetrics(samples.filter { $0.pass == .traversal }, from: 100, to: 102)!
    check(s.processes == [ProcessStats(name: "bb-probe", meanCPU: 200, peakCPU: 250, peakFootprintMB: 3000)], "process CPU and memory in the window")
    check(s.swapStartMB == 10 && s.swapEndMB == 20 && s.swapPeakMB == 30 && s.swapIns == 7, "swap")
    check(s.worstPressure == "warning" && s.worstThermal == "nominal", "pressure (loading outside the window ignored)")
    check(samples[1].csvRow == "traversal,100.000,,2000.0,,,10.0,5,1,0" && SystemSample.csvHeader.split(separator: ",").count == 10, "system.csv row")

    var csv = BenchmarkCSV(text: "# started=100\n# ended=102\n# result=ok\n")
    csv.frames = frames
    let info = BenchmarkSystemInfo(model: "Mac16,5", chip: "Apple M4 Max", performanceCores: 12, efficiencyCores: 4, gpuCores: 40,
                                   memoryGB: 64, macOS: "27.0.1", macOSBuild: "27A1", appVersion: "0.5.5",
                                   appBuild: "0.5.5-3-gabc", commit: "abc", display: "3456x2234 @ 120 Hz", power: "AC power", lowPowerMode: false)
    let passes = [PassSummary(pass: .traversal, csv: csv, samples: samples, exitStatus: 0),
                  PassSummary(pass: .throughput, csv: BenchmarkCSV(text: "t_ms,frame_ms\n5,5\n"), samples: samples, exitStatus: 1,
                              problem: "GPU host lost")]
    let summary = BenchmarkSummary(date: "2026-10-24 21.30.05", system: info, passes: passes)
    let report = summary.markdown
    check(!summary.complete && passes[0].ok && !passes[1].ok, "an interrupted pass makes the run incomplete")
    for line in ["# Bloodborne benchmark, 2026-10-24 21.30.05", "Mac16,5, Apple M4 Max, 12P + 4E CPU, 40-core GPU, 64 GB",
                 "**macOS:** 27.0.1 (27A1); app 0.5.5 (0.5.5-3-gabc), commit abc", "## Traversal:", "| Average FPS | 93.5 |",
                 "| 1% / 0.1% low | 14.3 / 10.0 FPS |", "| Hitches (over 25.0 ms) | 3,", "| 0.6 s | 100.0 ms | compile: compile wait 50 ms",
                 "| bb-probe | CPU 200% mean, 250% peak; 2.9 GB peak memory |", "**Result: interrupted", "(exit status 1)", "> GPU host lost"] {
        check(report.contains(line), "report has \(line.debugDescription)")
    }
    check(!report.components(separatedBy: "## Throughput")[0].contains("Max FPS"), "no max FPS for the capped traversal")
    check(report.contains("| Max FPS (best 1 s) |") == false, "throughput under 1 s: no max FPS")
    check((try? JSONDecoder().decode(BenchmarkSummary.self, from: summary.json)) == summary, "summary.json round trip")
}

// Mods & Cheats: the scripts' reports, mods.json, cheats=, patches.json, imports, save backups.
do {
    let report = try? ModReport.decode("""
    {"mods": [{"name": "A", "enabled": true, "files": 3, "error": null},
              {"name": "Broken", "enabled": true, "files": 0, "error": "Broken: no game folders"},
              {"name": "B", "enabled": false, "files": 1, "error": null}],
     "conflicts": [{"path": "dvdroot_ps4/chr/c1000.chrbnd.dcx", "mods": ["A", "C"], "winner": "C"}]}
    """)
    check(report?.mods.map(\.name) == ["A", "Broken", "B"] && report?.mods[0].error == nil
          && report?.mods[1].error == "Broken: no game folders" && report?.mods[2].enabled == false, "mods report")
    check(report?.conflicts.first?.line == "dvdroot_ps4/chr/c1000.chrbnd.dcx — A, C: C wins", "conflict line")
    check((try? ModReport.decode("{\"mods\": [], \"conflicts\": []}")) == ModReport(mods: [], conflicts: []), "empty report")
    check((try? ModReport.decode("Mods: oops")) == nil, "not JSON")

    let list = try? PatchList.decode("""
    {"cheats": [{"name": "No Rally Decay", "group": "Gameplay", "label": "Rally doesn't decay", "exclusive": "rally", "note": "n"},
                {"name": "Disable Rally (HP Regain)", "group": "Gameplay", "label": "No rally", "exclusive": "rally", "note": ""},
                {"name": "Player No Dead (Read note)", "group": "Cheats", "label": "Player can't die", "exclusive": null, "note": "a\\nb"},
                {"name": "Increased camera distance", "group": "Camera", "label": "Camera further away", "exclusive": null, "note": ""}],
     "external": [{"key": "x.xml/Fast", "file": "x.xml", "name": "Fast", "author": "me", "note": "", "enabled": true},
                  {"key": "x.xml/None", "file": "x.xml", "name": null, "author": "", "note": "", "enabled": false}]}
    """)
    let cheats = list?.cheats ?? []
    check(cheats.count == 4 && cheats[0].exclusive == "rally" && cheats[2].exclusive == nil && cheats[2].note == "a\nb", "cheats list")
    check(list?.external.count == 2 && list?.external[1].name == nil && list?.external[0].enabled == true, "external patches")
    check(Cheats.grouped(cheats).map(\.group) == ["Cheats", "Gameplay", "Camera"], "group order")

    // Exclusive groups: one of rally at a time; others untouched.
    var on = Cheats.toggle("No Rally Decay", on: true, in: ["Increased camera distance"], cheats: cheats)
    check(on == ["Increased camera distance", "No Rally Decay"], "turn on: \(on)")
    on = Cheats.toggle("Disable Rally (HP Regain)", on: true, in: on, cheats: cheats)
    check(on == ["Increased camera distance", "Disable Rally (HP Regain)"], "exclusive turns the other off: \(on)")
    on = Cheats.toggle("Increased camera distance", on: false, in: on, cheats: cheats)
    check(on == ["Disable Rally (HP Regain)"], "turn off")
    check(Cheats.toggle("Player No Dead (Read note)", on: true, in: ["Player No Dead (Read note)"], cheats: cheats) == ["Player No Dead (Read note)"], "no duplicate")

    // cheats= through GameSettings: other keys stay, no duplicate key, none on = no key.
    var ini = IniFile(text: "# header\ncheats=No Rally Decay; Unknown Patch ;No Rally Decay\nmy_key=7\n")
    var settings = GameSettings(ini: ini)
    check(settings.cheats == ["No Rally Decay", "Unknown Patch"], "parse cheats: \(settings.cheats)")
    settings.cheats.append("Increased camera distance")
    settings.apply(to: &ini)
    check(ini["cheats"] == "No Rally Decay;Unknown Patch;Increased camera distance" && ini["my_key"] == "7"
          && ini.text.hasPrefix("# header\ncheats=") && ini.text.components(separatedBy: "cheats=").count == 2, "cheats round trip: \(ini.text)")
    check(GameSettings(ini: ini) == settings, "read back")
    settings.cheats = []
    settings.apply(to: &ini)
    check(ini["cheats"] == nil && ini["my_key"] == "7", "no cheats: key removed")
    check(Cheats.parse(nil).isEmpty && Cheats.parse(" ; ").isEmpty && Cheats.format([]) == nil, "empty cheats")
    check(Cheats.summary(mods: 0, cheats: 0) == "No mods or cheats" && Cheats.summary(mods: 2, cheats: 1) == "2 mods · 1 cheat on"
          && Cheats.summary(mods: 1, cheats: 0) == "1 mod on" && Cheats.summary(mods: 0, cheats: 3) == "3 cheats on", "summary")

    // mods.json: the list's order, the unticked ones disabled; moves.
    let mods = report?.mods ?? []
    let config = ModsConfig(mods: mods.moving("B", by: -1))
    check(config.order == ["A", "B", "Broken"] && config.disabled == ["B"], "mods.json from the list")
    check(mods.moving("A", by: -1) == mods && mods.moving("B", by: 1) == mods, "moves stop at the ends")
    let json = (try? JSONSerialization.jsonObject(with: config.json)) as? [String: [String]]
    check(json == ["order": ["A", "B", "Broken"], "disabled": ["B"]], "mods.json format")
    check((try? JSONDecoder().decode(ModsConfig.self, from: Data("{\"order\": [\"x\"]}".utf8))) == ModsConfig(order: ["x"]), "partial mods.json")

    // patches.json
    var selection = PatchSelection(enabled: ["b.xml/On"], disabled: ["a.xml/Off"])
    selection.set("a.xml/Off", on: true)
    selection.set("c.xml/New", on: false)
    let patchesJSON = (try? JSONSerialization.jsonObject(with: selection.json)) as? [String: [String]]
    check(patchesJSON == ["enabled": ["a.xml/Off", "b.xml/On"], "disabled": ["c.xml/New"]], "patches.json: \(String(describing: patchesJSON))")

    // Files on disk: mods.json, patches.json and the enabled folders.
    let fm = FileManager.default
    let data = fm.temporaryDirectory.appendingPathComponent("launcher-mods-\(getpid())")
    let files = ModFiles(dataDir: data)
    check(files.mods.lastPathComponent == "mods" && files.patchesConfig.lastPathComponent == "patches.json"
          && ModFiles(dataDir: data, env: ["BB_MODS_DIR": "/x/m"]).mods.path == "/x/m", "mod files")
    for name in ["A", "B", ".hidden"] { try? fm.createDirectory(at: files.mods.appendingPathComponent(name), withIntermediateDirectories: true) }
    try? Data().write(to: files.mods.appendingPathComponent("readme.txt"))
    check(ModsConfig.enabledMods(modsDir: files.mods, config: files.modsConfig) == ["A", "B"], "all folders on without mods.json")
    try? ModsConfig(order: ["B", "A"], disabled: ["B"]).write(to: files.modsConfig)
    check(ModsConfig(contentsOf: files.modsConfig) == ModsConfig(order: ["B", "A"], disabled: ["B"]), "mods.json read back")
    check(ModsConfig.enabledMods(modsDir: files.mods, config: files.modsConfig) == ["A"], "disabled folder")
    try? selection.write(to: files.patchesConfig)
    check(PatchSelection(contentsOf: files.patchesConfig) == selection, "patches.json read back")

    // Import names.
    check(ModImport.baseName(of: URL(fileURLWithPath: "/d/Better Hunter.zip")) == "Better Hunter"
          && ModImport.baseName(of: URL(fileURLWithPath: "/d/Folder.v2")) == "Folder.v2"
          && ModImport.baseName(of: URL(fileURLWithPath: "/d/.zip")) == "Mod", "import base name")
    check(ModImport.uniqueName("X", existing: ["Y"]) == "X" && ModImport.uniqueName("X", existing: ["x", "X 2"]) == "X 3", "import collisions")

    // Backups: rotation keeps the newest five dated folders, the signature ignores order.
    let names = ["2026-10-20 10.00.00", "2026-10-21 10.00.00", "2026-10-25 09.00.00", "2026-10-22 10.00.00", "last.json",
                 "2026-10-23 10.00.00", "2026-10-24 10.00.00", "2026-10-24 10.00.00 2", "notes"]
    check(SaveBackup.toDelete(names) == ["2026-10-21 10.00.00", "2026-10-20 10.00.00"], "rotation: \(SaveBackup.toDelete(names))")
    check(SaveBackup.toDelete(Array(names.prefix(3))).isEmpty, "fewer than five")
    let signature = ModSignature(mods: ["B", "A"], cheats: ["z", "y"])
    check(signature == ModSignature(mods: ["A", "B"], cheats: ["y", "z"]) && signature.mods == ["A", "B"], "signature sorted")
    check(!SaveBackup.needed(ModSignature(mods: [], cheats: []), last: nil), "nothing on: no backup")
    check(SaveBackup.needed(signature, last: nil) && !SaveBackup.needed(signature, last: signature)
          && SaveBackup.needed(signature, last: ModSignature(mods: ["A"], cheats: ["y", "z"])), "backup per new set")
    var date = DateComponents()
    (date.year, date.month, date.day, date.hour, date.minute, date.second) = (2026, 10, 25, 9, 5, 7)
    let when = Calendar.current.date(from: date)!
    check(SaveBackup.folderName(when) == "2026-10-25 09.05.07" && SaveBackup.isBackupName("2026-10-25 09.05.07"), "backup name")

    let save = data.appendingPathComponent("user/savedata")
    try? fm.createDirectory(at: save.appendingPathComponent("0/slot"), withIntermediateDirectories: true)
    try? Data("save".utf8).write(to: save.appendingPathComponent("0/slot/userdata0000"))
    for name in names where SaveBackup.isBackupName(name) {
        try? fm.createDirectory(at: files.backups.appendingPathComponent(name), withIntermediateDirectories: true)
    }
    let made = try? SaveBackup.backUp(saveFolder: save, backups: files.backups, signature: signature, now: when)
    check(made?.lastPathComponent == "2026-10-25 09.05.07"
          && fm.fileExists(atPath: files.backups.appendingPathComponent("2026-10-25 09.05.07/savedata/0/slot/userdata0000").path), "backup copied")
    check(SaveBackup.lastSignature(backups: files.backups) == signature, "signature recorded")
    let kept = ((try? fm.contentsOfDirectory(atPath: files.backups.path)) ?? []).filter(SaveBackup.isBackupName).sorted()
    check(kept == ["2026-10-23 10.00.00", "2026-10-24 10.00.00", "2026-10-24 10.00.00 2", "2026-10-25 09.00.00", "2026-10-25 09.05.07"],
          "newest five kept: \(kept)")
    let again = try? SaveBackup.backUp(saveFolder: save, backups: files.backups, signature: signature, now: when)
    check(again?.lastPathComponent == "2026-10-25 09.05.07 2", "same second: a second folder")
    let empty = data.appendingPathComponent("no-saves")
    var copiedNothing = false
    do { copiedNothing = try SaveBackup.backUp(saveFolder: empty, backups: files.backups, signature: ModSignature(mods: ["C"], cheats: []), now: when) == nil } catch {}
    check(copiedNothing && SaveBackup.lastSignature(backups: files.backups)?.mods == ["C"], "no saves yet: nothing copied, set recorded")
    try? fm.removeItem(at: data)
}

if failures > 0 {
    print("FAIL: launcher model, \(failures) failed")
    exit(1)
}
print("PASS: launcher model (ini, resolutions, settings, launch.conf, saves, launch plan, crash report, updates, session logs, native crashes, benchmark, mods and cheats)")
