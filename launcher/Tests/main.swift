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

// GameSettings: defaults, reading, values the launcher does not offer, writing back.
do {
    let defaults = GameSettings(ini: IniFile())
    check(defaults == GameSettings(), "empty ini = defaults")
    check(defaults.output == PixelSize(1920, 1080) && defaults.upscaler == .off && defaults.preset == 0, "default display")
    check(defaults.frameCap == .fps60 && !defaults.fullscreen && defaults.renderer == .vulkan, "default cap, window, renderer")
    check(defaults.effects["effect_dof"] == true && defaults.effects["effect_ssr"] == false && defaults.effects["skip_intro"] == false, "default effects")
    check(defaults.sharpen && defaults.sharpness == 0.3 && !defaults.showFPS && !defaults.metalHUD && defaults.shaderCompile == .balanced, "default extras")

    let read = GameSettings(ini: IniFile(text: "upscaler=metalfx\npreset=2\noutput_res=2560x1440\nfps_cap=40\nfullscreen=1\neffect_dof=0\nshow_fps=1\nmetal_hud=1\nrenderer=metal\nshader_compile=skip\nsharpness=0.75\n"))
    check(read.upscaler == .metalfx && read.preset == 2 && read.output == PixelSize(2560, 1440), "read display")
    check(read.frameCap == .fps40 && read.fullscreen && read.renderer == .metal && read.shaderCompile == .skip, "read launcher keys")
    check(read.effects["effect_dof"] == false && read.showFPS && read.metalHUD && read.sharpness == 0.75, "read effects")
    check(read.renderSize == PixelSize(1506, 848), "render size follows output and preset")
    check(GameSettings(ini: IniFile(text: "upscaler=taa\npreset=3\n")).renderSize == PixelSize(1920, 1080), "TAA renders native")

    // Review focus 2: values the launcher does not offer survive unless changed.
    var other = IniFile(text: "output_res=3008x1692\nupscaler=fsr4\nfps_cap=45\npreset=9\nsharpness=7\nfsr4_auto_exposure=1\n")
    let odd = GameSettings(ini: other)
    check(odd.output == PixelSize(3008, 1692) && odd.upscaler == .fsr4, "unlisted output and FSR 4 kept")
    check(odd.frameCap == .fps60 && odd.preset == 0 && odd.sharpness == 0.3, "invalid values -> defaults")
    odd.apply(to: &other, includeRenderer: false)
    check(other["output_res"] == "3008x1692" && other["upscaler"] == "fsr4", "written back unchanged")
    check(other["fsr4_auto_exposure"] == "1" && other["renderer"] == nil, "other keys kept; no renderer key")
    var changed = read
    changed.output = PixelSize(3840, 2160)
    changed.effects["effect_ssr"] = true
    var file = IniFile(text: "# header\nupscaler=metalfx\njitter=1\n")
    changed.apply(to: &file, includeRenderer: true)
    check(file["output_res"] == "3840x2160" && file["effect_ssr"] == "1" && file["jitter"] == "1", "apply writes and keeps")
    check(file["fps_cap"] == "40" && file["fullscreen"] == "1" && file["renderer"] == "metal" && file["sharpness"] == "0.75", "apply launcher keys")
    check(file["metal_hud"] == "1", "apply writes the Metal HUD key")
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

    check(!packaged!.hasMetalRenderer, "no Metal marker")
    try? Data().write(to: port.appendingPathComponent("metal-renderer"))
    check(packaged!.hasMetalRenderer, "Metal marker")

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
    check(p.environment["BB_PREBUILT"] == "1" && p.environment["BB_PROBE"] == contents.appendingPathComponent("MacOS/bb-probe").path, "prebuilt probe")
    check(p.environment["PYTHON"] == contents.appendingPathComponent("Resources/python/bin/python3").path, "bundled Python")
    check(p.environment["VK_DRIVER_FILES"] == port.appendingPathComponent("MoltenVK_icd.json").path, "bundled MoltenVK")
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
    check(packaged!.gameCheckCommand == [contents.appendingPathComponent("Resources/python/bin/python3").path, port.appendingPathComponent("scripts/game_check.py").path], "packaged game check")
    check(checkout!.gameCheckCommand == ["/usr/bin/env", "python3", repo.appendingPathComponent("scripts/game_check.py").path], "checkout game check")
    try? fm.createDirectory(at: root.appendingPathComponent("CUSA03173"), withIntermediateDirectories: true)
    try? Data().write(to: root.appendingPathComponent("CUSA03173/eboot.bin"))
    check(checkout!.defaultGameDir()?.path == root.appendingPathComponent("CUSA03173").path, "checkout default: ../CUSA03173")
    check(packaged!.defaultGameDir() == nil, "packaged: no default game folder")
    try? fm.removeItem(at: root)
}

// A keychain that remembers what was stored (the real one is the user's; launch_settings.sh
// reads the same items with `security find-generic-password`).
final class FakeSecrets: SecretStore {
    var items: [String: String] = [:]
    var failing = false
    func has(service: String, account: String) -> Bool { items["\(service)/\(account)"] != nil }
    func store(service: String, account: String, secret: String) throws {
        if failing { throw HuntersDream.Failure(description: "keychain locked") }
        items["\(service)/\(account)"] = secret
    }
    func remove(service: String, account: String) { items["\(service)/\(account)"] = nil }
}

// OnlineSettings: the keys scripts/launch_settings.sh reads, round trip, secrets.
do {
    let defaults = OnlineSettings(ini: IniFile())
    check(defaults == OnlineSettings(), "empty ini = defaults")
    check(!defaults.online && defaults.pool == .huntersdream && defaults.upnp && defaults.p2pPort == 3658, "default online settings")
    check(defaults.shadnetServer == "" && defaults.shadnetNpid == "" && defaults.shadnetPassword == .none, "default shadNet: nothing")
    check(defaults.thdOnlineId == "" && defaults.thdToken == .none && !defaults.hasSecretInFile, "default Hunter's Dream: nothing")

    let read = OnlineSettings(ini: IniFile(text: "online=1\nonline_pool=shadnet\nshadnet_server=example.org:31313\nshadnet_npid=hunter\nshadnet_password=keychain\nthd_online_id=dreamer\nthd_token=abc123\nupnp=0\np2p_port=0\ncommunity_server=https://example.org\n"))
    check(read.online && read.pool == .shadnet && read.shadnetServer == "example.org:31313" && read.shadnetNpid == "hunter", "read shadNet keys")
    check(read.shadnetPassword == .keychain && read.thdOnlineId == "dreamer" && read.thdToken == .inFile("abc123"), "read secrets as stored")
    check(!read.upnp && read.p2pPort == 0 && read.hasSecretInFile, "read upnp, port, a token in the file")
    // The shell's rules: flags are exactly 0 or 1; a bad port or pool is ignored.
    let odd = OnlineSettings(ini: IniFile(text: "online=yes\nupnp=2\np2p_port=70000\nonline_pool=psn\nshadnet_password=\n"))
    check(!odd.online && odd.upnp && odd.p2pPort == 3658 && odd.pool == .huntersdream && odd.shadnetPassword == .none, "invalid values -> defaults")
    check(OnlineSettings.port(from: "3658") == 3658 && OnlineSettings.port(from: "0") == 0 && OnlineSettings.port(from: "65535") == 65535, "ports")
    check(OnlineSettings.port(from: "65536") == nil && OnlineSettings.port(from: "") == nil && OnlineSettings.port(from: "12a") == nil, "bad ports")
    check(OnlineSettings.port(from: "-1") == nil && OnlineSettings.port(from: "123456") == nil && OnlineSettings.port(from: "٣") == nil, "digits only, five at most")
    // The pool when the file names none (launch_settings.sh): shadNet only for a file that holds
    // only a shadNet account.
    check(OnlineSettings(ini: IniFile(text: "online=1\nshadnet_npid=hunter\n")).pool == .shadnet, "only a shadNet account: shadNet")
    check(OnlineSettings(ini: IniFile(text: "shadnet_npid=hunter\nthd_online_id=dreamer\n")).pool == .huntersdream, "both accounts: The Hunter's Dream")
    check(OnlineSettings(ini: IniFile(text: "online=1\n")).pool == .huntersdream, "no account: The Hunter's Dream")

    // Round trip: every key written as the shell reads it; other keys and the order kept.
    var file = IniFile(text: "# header\nupscaler=metalfx\ncommunity_server=https://example.org\nshadnet_webapi=http://example.org:31315\n")
    var online = read
    online.shadnetServer = "  "
    online.apply(to: &file)
    check(OnlineSettings(ini: file) == OnlineSettings(ini: IniFile(text: file.text)), "written and read back")
    let back = OnlineSettings(ini: file)
    check(back.online == read.online && back.pool == read.pool && back.shadnetNpid == read.shadnetNpid, "round trip accounts")
    check(back.shadnetPassword == .keychain && back.thdToken == .inFile("abc123") && back.thdOnlineId == "dreamer", "round trip secrets unchanged")
    check(back.upnp == read.upnp && back.p2pPort == read.p2pPort, "round trip upnp and port")
    check(file["online"] == "1" && file["online_pool"] == "shadnet" && file["upnp"] == "0" && file["p2p_port"] == "0", "keys as the shell reads them")
    check(file["shadnet_server"] == nil && file["shadnet_password"] == "keychain" && file["thd_token"] == "abc123", "blank server removed; secrets as read")
    check(file["community_server"] == "https://example.org" && file["shadnet_webapi"] == "http://example.org:31315" && file["upscaler"] == "metalfx", "other keys kept")
    check(file.text.hasPrefix("# header\n"), "header kept")
    var cleared = online
    cleared.shadnetNpid = ""
    cleared.online = false
    cleared.upnp = true
    cleared.p2pPort = 65535
    cleared.apply(to: &file)
    check(file["shadnet_npid"] == nil && file["online"] == "0" && file["upnp"] == "1" && file["p2p_port"] == "65535", "cleared account; flags written explicitly")
    // The shell reads what the launcher wrote (scripts/launch_settings.sh).
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("launcher-online-\(getpid()).ini")
    try? file.write(to: url)
    let bash = Process()
    bash.executableURL = URL(fileURLWithPath: "/bin/bash")
    bash.arguments = ["-c", "source scripts/launch_settings.sh && bb_launch_settings \"$1\" && printf '%s %s %s %s %s' \"${BB_ONLINE:--}\" \"${BB_NET_BACKEND:--}\" \"${BB_UPNP:--}\" \"${BB_P2P_PORT:--}\" \"${BB_THD_ONLINE_ID:--}\"", "_", url.path]
    var environment = ProcessInfo.processInfo.environment
    for key in ["BB_ONLINE", "BB_NET_BACKEND", "BB_UPNP", "BB_P2P_PORT", "BB_THD_ONLINE_ID", "BB_THD_TOKEN", "BB_SHADNET_NPID"] {
        environment.removeValue(forKey: key)
    }
    environment["BB_KEYCHAIN_TOOL"] = "/usr/bin/false"
    bash.environment = environment
    let pipe = Pipe()
    bash.standardOutput = pipe
    try? bash.run()
    bash.waitUntilExit()
    let sourced = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
    check(bash.terminationStatus == 0 && sourced == "- - 1 65535 dreamer", "launch_settings.sh reads the launcher's keys: \(sourced.debugDescription)")
    try? FileManager.default.removeItem(at: url)

    // Secrets: the password and the token go to the keychain, the file says `keychain`.
    let secrets = FakeSecrets()
    var account = OnlineSettings()
    account.shadnetNpid = " hunter "
    check(!account.shadnetPasswordStored(secrets: secrets) && !account.signedIn(secrets: secrets), "nothing stored yet")
    do { try account.storeShadnetPassword("s3cret", secrets: secrets) } catch { check(false, "store password: \(error)") }
    check(secrets.items["bbport-shadnet/hunter"] == "s3cret" && account.shadnetPassword == .keychain, "password under the trimmed account name")
    check(account.shadnetPasswordStored(secrets: secrets), "password reported as stored")
    account.shadnetNpid = "other"
    check(!account.shadnetPasswordStored(secrets: secrets), "another account: no password")
    check(OnlineSettings(ini: IniFile(text: "shadnet_npid=x\nshadnet_password=clear\n")).shadnetPasswordStored(secrets: secrets), "a password in the file counts")
    secrets.failing = true
    var failed = account
    do { try failed.storeShadnetPassword("x", secrets: secrets); check(false, "a failing keychain must throw") } catch {}
    check(failed == account, "a failed store changes nothing")
    secrets.failing = false
    account.pool = .shadnet
    do { try account.signIn(onlineId: "dreamer", token: "tok", secrets: secrets) } catch { check(false, "sign in: \(error)") }
    check(secrets.items["bbport-huntersdream/dreamer"] == "tok" && account.thdOnlineId == "dreamer" && account.thdToken == .keychain, "token in the keychain")
    check(account.pool == .huntersdream && account.signedIn(secrets: secrets), "signed in on The Hunter's Dream")
    var ini = IniFile()
    account.apply(to: &ini)
    check(ini["thd_online_id"] == "dreamer" && ini["thd_token"] == "keychain" && ini["online_pool"] == "huntersdream", "what online.py signin --keychain writes")
    check(OnlineSettings(ini: IniFile(text: "thd_online_id=dreamer\nthd_token=keychain\n")).signedIn(secrets: secrets), "signed in from the file's `keychain`")
    check(!OnlineSettings(ini: IniFile(text: "thd_online_id=gone\nthd_token=keychain\n")).signedIn(secrets: secrets), "`keychain` without an item: not signed in")
    check(OnlineSettings(ini: IniFile(text: "thd_online_id=gone\nthd_token=abc\n")).signedIn(secrets: secrets), "a token in the file: signed in")
    account.signOut(secrets: secrets)
    check(secrets.items["bbport-huntersdream/dreamer"] == nil && account.thdOnlineId == "" && account.thdToken == .none, "signed out: item and keys gone")
    check(!account.signedIn(secrets: secrets) && secrets.items["bbport-shadnet/hunter"] == "s3cret", "the shadNet password stays")
    account.apply(to: &ini)
    check(ini["thd_online_id"] == nil && ini["thd_token"] == nil, "signed out: no account keys")
}

// IniFile keeps an owner-only file owner-only (a secret in clear).
do {
    let fm = FileManager.default
    let url = fm.temporaryDirectory.appendingPathComponent("launcher-perms-\(getpid())/bbport.ini")
    var ini = IniFile(text: "thd_token=abc\n")
    try? ini.write(to: url)
    try? fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    ini["online"] = "1"
    try? ini.write(to: url)
    let mode = ((try? fm.attributesOfItem(atPath: url.path))?[.posixPermissions] as? Int) ?? -1
    check(mode == 0o600 && IniFile(contentsOf: url)["online"] == "1", "permissions kept across an atomic write: \(String(mode, radix: 8))")
    try? fm.removeItem(at: url.deletingLastPathComponent())
}

// HuntersDream: the device sign-in protocol (scripts/online.py thd_sign_in).
do {
    check(HuntersDream.communityAddress(nil) == "https://thehuntersdream.com" && HuntersDream.communityAddress("  ") == "https://thehuntersdream.com", "default game server")
    check(HuntersDream.communityAddress("https://example.org:20443/bb-eu/ss.info") == "https://example.org", "scheme and host only")
    check(HuntersDream.communityAddress("example.org") == "https://example.org" && HuntersDream.communityAddress("http://x.test/") == "http://x.test", "scheme added or kept")
    let community = "https://example.org"
    let start = HuntersDream.startRequest(community: community)
    check(start.url?.absoluteString == "https://example.org/auth/device/start" && start.httpMethod == "POST", "start request")
    check(start.value(forHTTPHeaderField: "Content-Type") == "application/json", "JSON request")
    let startBody = start.httpBody.flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: String] }
    check(startBody == ["DeviceName": "bbport on macOS"], "the device name, not the hostname")
    let poll = HuntersDream.pollRequest(community: community, deviceCode: "dev-1")
    let pollBody = poll.httpBody.flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: String] }
    check(poll.url?.absoluteString == "https://example.org/auth/device/poll" && pollBody == ["DeviceCode": "dev-1"], "poll request")

    func data(_ json: String) -> Data { Data(json.utf8) }
    let started = try? HuntersDream.deviceStart(data(#"{"ResKind":0,"DeviceCode":"dev-1","UserCode":"ABCD-EFGH","VerificationUri":"https://example.org/link","Interval":3,"ExpiresIn":120}"#), community: community)
    check(started == HuntersDream.DeviceStart(deviceCode: "dev-1", userCode: "ABCD-EFGH", page: URL(string: "https://example.org/link")!, interval: 3, expiresIn: 120), "start reply: \(String(describing: started))")
    let bare = try? HuntersDream.deviceStart(data(#"{"ResKind":0,"DeviceCode":"d","UserCode":"C"}"#), community: community)
    check(bare?.page.absoluteString == "https://example.org/link" && bare?.interval == 5 && bare?.expiresIn == 600, "start reply defaults")
    func failure(_ body: () throws -> Any) -> String? {
        do { _ = try body(); return nil } catch { return (error as? HuntersDream.Failure)?.description ?? "other error" }
    }
    check(failure { try HuntersDream.deviceStart(data(#"{"ResKind":0}"#), community: community) } == "no code in the reply", "start without a code")
    check(failure { try HuntersDream.deviceStart(data(#"{"ResKind":3,"Message":"too many devices"}"#), community: community) } == "too many devices", "refused with a message")
    check(failure { try HuntersDream.deviceStart(data(#"{"ResKind":1}"#), community: community) } == "refused", "refused without a message")
    check(failure { try HuntersDream.deviceStart(data("<html>404</html>"), community: community) } == "https://example.org/auth/device/start has no account service", "not an account service")
    check((try? HuntersDream.poll(data(#"{"ResKind":0,"Status":"pending"}"#), community: community)) == .pending, "poll pending")
    check((try? HuntersDream.poll(data(#"{"ResKind":0}"#), community: community)) == .pending, "poll without a status: pending")
    check((try? HuntersDream.poll(data(#"{"ResKind":0,"Status":"slow_down"}"#), community: community)) == .slowDown, "poll slow_down")
    check((try? HuntersDream.poll(data(#"{"ResKind":0,"Status":"denied"}"#), community: community)) == .denied, "poll denied")
    check((try? HuntersDream.poll(data(#"{"ResKind":0,"Status":"expired"}"#), community: community)) == .expired, "poll expired")
    check((try? HuntersDream.poll(data(#"{"ResKind":0,"Status":"approved","OnlineId":"dreamer","Token":"tok"}"#), community: community)) == .approved(onlineId: "dreamer", token: "tok"), "poll approved")
    check(failure { try HuntersDream.poll(data(#"{"ResKind":0,"Status":"approved","OnlineId":"dreamer"}"#), community: community) } == "no token in the reply", "approved without a token")
}

if failures > 0 {
    print("FAIL: launcher model, \(failures) failed")
    exit(1)
}
print("PASS: launcher model (ini, resolutions, settings, launch.conf, saves, launch plan, online, sign-in)")
