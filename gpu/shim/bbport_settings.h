// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>
#include <array>

/// Keyboard shortcut that opens the in-game menu (log lines and the ini header; ASCII).
/// Mac keyboards have no Insert key, so macOS uses the Settings convention Cmd+,.
#ifdef __APPLE__
#define BBPORT_MENU_KEYS "Cmd+,"
#else
#define BBPORT_MENU_KEYS "Insert"
#endif

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerMetalFx = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: removed from the Mac build (M5); an ini that names them still loads
/// (they run as a fallback, the file keeps the name).
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", true},
    {"effect_dof", "Depth of field (DoF)", true},
    {"effect_motion_blur", "Motion blur", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", true},
    {"effect_game_aa", "Built-in game anti-aliasing", true},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", true},
    {"effect_ssr", "Screen-space reflections (not in original)", false},
    {"skip_intro", "Skip intro videos at launch", false},
    {"debug_camera", "Free camera (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets.
/// 3456x1944: 16:9 at the full width of the 16-inch MacBook Pro's panel (3456x2234), pixel for
/// pixel.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3456, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 1944, 2160};
inline constexpr int OutputCount = 5;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size

/// Values::shader_compile.
enum ShaderCompile { ShaderWait, ShaderSkip, ShaderBalanced, ShaderCompileCount };

struct Values {
    std::atomic<int> upscaler{UpscalerOff};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{false};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{false};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// Frame pacing at the FPS cap (vk_presenter.cpp PacingSeconds): even frame times on
    /// variable-refresh displays, about one frame more input latency. bbport.ini frame_pacing; live.
    std::atomic<bool> frame_pacing{false};
    /// Mouse look (bbport_mouse.cpp): bbport.ini mouse_look, mouse_sensitivity, mouse_invert_y.
    /// Off by default: on, the first left click in the window captures the cursor, for gamepad
    /// players too. Players turn it on in the menu (Display > Mouse camera).
    std::atomic<bool> mouse_look{false};
    std::atomic<float> mouse_sensitivity{1.0f};
    std::atomic<bool> mouse_invert_y{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    // Fixed at launch: native scene/UI patch dimensions, requiring a restart.
    int ultrawide_width = 0, ultrawide_height = 0;
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (off on a Mac), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// A draw whose pipeline is still compiling in the background: Wait waits for it (a hitch,
    /// correct image), Skip leaves the draw out until it is ready (no hitch, an object may be
    /// missing for a few frames), Balanced waits within a small time budget and skips past it.
    /// bbport.ini shader_compile=wait|skip|balanced; live.
    std::atomic<int> shader_compile{ShaderBalanced};
    /// Providers this build and device can run (ConfigureUpscalerSupport); off and TAA always.
    std::atomic<bool> metalfx_supported{false}; ///< macOS: Apple MetalFX temporal scaler
    std::atomic<bool> fsr3_supported{false};    ///< FSR 3.1: Apple7+ (shim/ffx_metal)
    /// What the user chose (the ini, BB_UPSCALER or the menu): Save() writes it. From
    /// ConfigureUpscalerSupport on, `upscaler` is what runs, which differs while that choice is
    /// unavailable (FallbackUpscaler).
    std::atomic<int> requested_upscaler{UpscalerOff};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();
/// Output dimensions actually selected for this session, including native ultrawide.
std::array<int, 2> OutputSize();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Records which providers can run and, before the first frame, resolves the user's choice to
/// the one that runs (FallbackUpscaler; logged once, never saved).
void ConfigureUpscalerSupport(bool metalfx, bool fsr3 = false);
/// Whether `upscaler` can run with the providers ConfigureUpscalerSupport recorded.
bool UpscalerAvailable(int upscaler);
/// The provider that runs for a choice: the choice itself when available, else MetalFX when
/// available, else TAA at Native AA (anti-aliasing at the same scene size), else off. Never an
/// unavailable provider (FSR 3.1 below Apple7, the removed FSR 4 choices).
int FallbackUpscaler(int requested, bool metalfx, bool fsr3, int preset);
/// The menu's choice: becomes the requested (saved) value and runs, or its fallback.
void ChooseUpscaler(int upscaler);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// The launcher's "show every time" choice in launch.conf (BB_LAUNCH_CONF, set by the launcher):
/// 1 or 0, or -1 when the launcher did not start the game. Read once, then cached.
int ShowLauncher();
/// Rewrites BB_SHOW_LAUNCHER in launch.conf; every other line is kept.
void SetShowLauncher(bool show);

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
