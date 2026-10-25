// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum Upscaler: String, CaseIterable, Identifiable {
    case off, fsr3, fsr4, fsr411, taa, metalfx

    var id: String { rawValue }
    var label: String {
        switch self {
        case .off: "Off"
        case .fsr3: "FSR 3.1"
        case .fsr4: "FSR 4"
        case .fsr411: "FSR 4.1.1"
        case .taa: "TAA"
        case .metalfx: "MetalFX"
        }
    }
    /// The quality level sets the render size (TAA always renders at the output size).
    var usesPreset: Bool { self != .off && self != .taa }
    /// Offered in the launcher (the in-game menu's list). FSR 4 and 4.1.1 are gone from the Mac
    /// build: an ini naming them still reads and is kept, the game runs a fallback.
    static let launcherChoices: [Upscaler] = [.off, .fsr3, .metalfx, .taa]
}

enum FrameCap: Int, CaseIterable, Identifiable {
    case fps30 = 30, fps40 = 40, fps60 = 60, fps90 = 90, fps120 = 120
    var id: Int { rawValue }
    var label: String { "\(rawValue)" }
    /// Caps with a fixed-step frame rate patch (scripts/launch_settings.sh): 30 is the unpatched game.
    var hasFixedSteps: Bool { self == .fps30 || self == .fps60 || self == .fps90 }
}

/// bbport.ini game_timing, read by run.sh at start (scripts/launch_settings.sh picks BB_FPS).
enum GameTiming: String, CaseIterable, Identifiable {
    case fixed, variable
    var id: String { rawValue }
    var label: String {
        switch self {
        case .fixed: "Fixed steps"
        case .variable: "Variable"
        }
    }
}

enum ShaderCompile: String, CaseIterable, Identifiable {
    case balanced, wait, skip
    var id: String { rawValue }
    var label: String {
        switch self {
        case .balanced: "Balanced"
        case .wait: "Wait (no pop-in, stutters)"
        case .skip: "Skip (never stutters, pop-in)"
        }
    }
}

/// bbport.ini model_lod: the level-of-detail bias (in-game menu: Graphics → Model detail).
enum ModelDetail: Int, CaseIterable, Identifiable {
    case highest = -2, gameDefault = 0, lower = 1, lowest = 2
    var id: Int { rawValue }
    var label: String {
        switch self {
        case .highest: "Highest"
        case .gameDefault: "Game default"
        case .lower: "Lower"
        case .lowest: "Lowest"
        }
    }
}

/// bbport.ini live_resolution, read by run.sh at start (in-game menu: Advanced → Resolution).
enum LiveResolution: String, CaseIterable, Identifiable {
    case auto, off = "0", on = "1"
    var id: String { rawValue }
    var label: String {
        switch self {
        case .auto: "Auto"
        case .off: "Off"
        case .on: "On"
        }
    }
}

struct Effect: Identifiable {
    let key: String
    let label: String
    let defaultOn: Bool
    var id: String { key }
}

/// The launcher's view of bbport.ini (the file the in-game menu writes too). Keys it does not
/// know stay in the IniFile; values it does not offer (an unlisted output, FSR 4) are kept as read.
struct GameSettings: Equatable {
    /// gpu/shim/bbport_settings.h Effects (without the debug ones), same defaults.
    static let effects = [
        Effect(key: "effect_dof", label: "Depth of field", defaultOn: true),
        Effect(key: "effect_motion_blur", label: "Motion blur", defaultOn: true),
        Effect(key: "effect_chromatic_aberration", label: "Chromatic aberration", defaultOn: true),
        Effect(key: "effect_ssao", label: "Ambient occlusion (SSAO)", defaultOn: true),
        Effect(key: "effect_dynamic_shadows", label: "Shadows from dynamic lights", defaultOn: true),
        Effect(key: "effect_ssr", label: "Screen-space reflections (not in the original)", defaultOn: false),
        Effect(key: "effect_game_aa", label: "The game's own anti-aliasing", defaultOn: true),
        Effect(key: "skip_intro", label: "Skip intro videos", defaultOn: false),
        Effect(key: "debug_camera", label: "Free camera (Cross + L3)", defaultOn: false),
        Effect(key: "debug_menu", label: "Debug menu (needs font files)", defaultOn: false),
    ]
    static var visualEffects: [Effect] { effects.filter { $0.key != "skip_intro" && !$0.key.hasPrefix("debug_") } }
    static var debugEffects: [Effect] { effects.filter { $0.key.hasPrefix("debug_") } }
    static let newFileHeader = "# bbport settings (in-game menu: Cmd+, / L3+R3)\n"

    var output = Resolution.defaultOutput
    var ultrawide = false
    var wideOutput = PixelSize(3840, 1080)
    var displayOutput: PixelSize { ultrawide ? wideOutput : output }
    var upscaler = Upscaler.off
    var preset = 0
    var sharpen = true
    var sharpness = 0.3
    var showFPS = false
    var metalHUD = false
    /// bbport.ini mouse_look (PR #20): the mouse turns the camera after a click in the window,
    /// and the arrow keys are the d-pad.
    var mouseLook = false
    var effects = Dictionary(uniqueKeysWithValues: GameSettings.effects.map { ($0.key, $0.defaultOn) })
    var frameCap = FrameCap.fps60
    var gameTiming = GameTiming.fixed
    var fullscreen = false
    var shaderCompile = ShaderCompile.balanced
    // The in-game menu's Graphics and Advanced options (gpu/shim/bbport_settings.h, same defaults).
    var jitter = false
    var modelDetail = ModelDetail.gameDefault
    var reactive = false
    var reactiveScale = 1.0
    var reactiveThreshold = 0.2
    var reactiveMax = 0.9
    var objectMotion = false
    var liveResolution = LiveResolution.off
    /// bbport.ini texture_compression, read by run.sh at start (scripts/launch_settings.sh:
    /// 0 = BB_LAZY_TEXTURE_USAGE=0).
    var textureCompression = true
    /// bbport.ini cheats= (the Mods & Cheats window; scripts/patches.py applies them at launch).
    /// Names the launcher does not list stay as read.
    var cheats: [String] = []

    init() {}

    init(ini: IniFile) {
        // Booleans as the game reads them (atoi(value) != 0).
        func flag(_ key: String) -> Bool? { ini[key].map { (Int($0) ?? 0) != 0 } }
        if let v = ini["output_res"], let size = PixelSize(iniValue: v) { output = size }
        if let v = ini["ultrawide_res"], let size = PixelSize(iniValue: v), Resolution.ultrawideOutputs.contains(size) {
            ultrawide = true
            wideOutput = size
            if let original = ini["standard_output_res"].flatMap(PixelSize.init(iniValue:)) { output = original }
        }
        if let v = ini["upscaler"], let u = Upscaler(rawValue: v) { upscaler = u }
        if let v = ini["preset"], let p = Int(v), Resolution.presetScales.indices.contains(p) { preset = p }
        if let v = flag("sharpen") { sharpen = v }
        if let v = ini["sharpness"], let s = Double(v), (0...2).contains(s) { sharpness = s }
        if let v = flag("show_fps") { showFPS = v }
        if let v = flag("metal_hud") { metalHUD = v }
        if let v = flag("mouse_look") { mouseLook = v }
        for effect in Self.effects {
            if let v = flag(effect.key) { effects[effect.key] = v }
        }
        if let v = ini["fps_cap"], let n = Int(v), let cap = FrameCap(rawValue: n) { frameCap = cap }
        if let v = ini["game_timing"], let t = GameTiming(rawValue: v) { gameTiming = t }
        if let v = flag("fullscreen") { fullscreen = v }
        if let v = ini["shader_compile"], let s = ShaderCompile(rawValue: v) { shaderCompile = s }
        // Ranges as the game clamps them.
        func number(_ key: String, _ range: ClosedRange<Double>) -> Double? {
            ini[key].flatMap(Double.init).flatMap { range.contains($0) ? $0 : nil }
        }
        if let v = flag("jitter") { jitter = v }
        if let v = ini["model_lod"], let n = Int(v), let d = ModelDetail(rawValue: n) { modelDetail = d }
        if let v = flag("reactive") { reactive = v }
        if let v = number("reactive_scale", 0...16) { reactiveScale = v }
        if let v = number("reactive_threshold", 0...1) { reactiveThreshold = v }
        if let v = number("reactive_max", 0...1) { reactiveMax = v }
        if let v = flag("object_motion") { objectMotion = v }
        if let v = ini["live_resolution"], let l = LiveResolution(rawValue: v) { liveResolution = l }
        if let v = flag("texture_compression") { textureCompression = v }
        cheats = Cheats.parse(ini["cheats"])
    }

    /// Writes the launcher's settings into `ini`; every other line stays (a `renderer` key from
    /// 0.5.2 or earlier too: the game drops it the next time its menu saves).
    func apply(to ini: inout IniFile) {
        ini["output_res"] = (ultrawide ? Resolution.defaultOutput : output).iniValue
        ini["ultrawide_res"] = ultrawide ? wideOutput.iniValue : nil
        ini["standard_output_res"] = output.iniValue
        ini["upscaler"] = ultrawide ? "off" : upscaler.rawValue
        ini["preset"] = ultrawide ? "0" : String(preset)
        ini["sharpen"] = sharpen ? "1" : "0"
        ini["sharpness"] = String(format: "%.2f", sharpness)
        ini["show_fps"] = showFPS ? "1" : "0"
        ini["metal_hud"] = metalHUD ? "1" : "0"
        ini["mouse_look"] = mouseLook ? "1" : "0"
        for effect in Self.effects {
            ini[effect.key] = (effects[effect.key] ?? effect.defaultOn) ? "1" : "0"
        }
        ini["fps_cap"] = String(frameCap.rawValue)
        ini["game_timing"] = gameTiming.rawValue
        ini["fullscreen"] = fullscreen ? "1" : "0"
        ini["shader_compile"] = shaderCompile.rawValue
        ini["jitter"] = jitter ? "1" : "0"
        ini["model_lod"] = String(modelDetail.rawValue)
        ini["reactive"] = reactive ? "1" : "0"
        ini["reactive_scale"] = String(format: "%.2f", reactiveScale)
        ini["reactive_threshold"] = String(format: "%.2f", reactiveThreshold)
        ini["reactive_max"] = String(format: "%.2f", reactiveMax)
        ini["object_motion"] = objectMotion ? "1" : "0"
        ini["live_resolution"] = ultrawide ? "0" : liveResolution.rawValue
        ini["texture_compression"] = textureCompression ? "1" : "0"
        ini["cheats"] = Cheats.format(cheats)
    }

    /// The scene size the game renders.
    var renderSize: PixelSize {
        ultrawide ? wideOutput : (upscaler.usesPreset ? Resolution.renderSize(output: output, preset: preset) : output)
    }
}
