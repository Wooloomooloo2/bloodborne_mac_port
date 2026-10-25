// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

/// Keys Save() writes; every other key=value line of the file is kept as it is (the launcher's
/// fps_cap and fullscreen, the removed FSR 4 checks fsr4_auto_exposure/fsr4_invert_jitter, ...).
/// `renderer` (metal|vulkan until 0.5.2) is owned but no longer written: saving drops it.
bool OwnedKey(const std::string& key) {
    static constexpr const char* owned[] = {
        "upscaler", "preset", "sharpen", "sharpness", "jitter", "reactive", "object_motion",
        "reactive_scale", "reactive_threshold", "reactive_max", "debug_view", "show_fps",
        "frame_pacing",
        "model_lod", "output_res",
        "live_resolution", "shader_compile", "renderer"};
    for (const char* k : owned) {
        if (key == k) {
            return true;
        }
    }
    for (const auto& effect : Effects) {
        if (key == effect.key) {
            return true;
        }
    }
    return false;
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "frame_pacing") {
        v.frame_pacing = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "shader_compile") {
        v.shader_compile = value == "skip"       ? BbSettings::ShaderSkip
                           : value == "balanced" ? BbSettings::ShaderBalanced
                                                 : BbSettings::ShaderWait;
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

std::array<int, 2> OutputSize() {
    const auto& v = Get();
    if (v.ultrawide_width) return {v.ultrawide_width, v.ultrawide_height};
    const int index = FixedRenderSession() ? v.startup_output_res : v.output_res.load();
    return {OutputWidths[index], OutputHeights[index]};
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_SHADER_COMPILE", "shader_compile"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    // Native wide patches own scene/UI dimensions. Disable generic host resizing.
    // Match the launcher's validated presets; observe actual scene size at the G-buffer.
    v.ultrawide_width = v.ultrawide_height = 0;
    if (const char* wide = std::getenv("BB_ULTRAWIDE_RES"); wide && wide[0]) {
        constexpr std::array<int, 2> sizes[] = {
            {2560, 720}, {3840, 1080}, {5120, 1440}, {2560, 1080}, {3440, 1440},
            {1280, 800}, {1920, 1200}, {2560, 1600}};
        for (const auto& size : sizes) {
            if (wide == std::to_string(size[0]) + "x" + std::to_string(size[1])) {
                v.ultrawide_width = size[0];
                v.ultrawide_height = size[1];
                v.output_res = OutputDefault;
                v.live_resolution = 0;
                v.active_render_width = v.active_render_height = 0;
                v.upscaler = UpscalerOff;
                v.preset = NativeAA;
                break;
            }
        }
    }
    // The choice as asked for; ConfigureUpscalerSupport resolves what runs before the renderer
    // reads it.
    v.requested_upscaler = v.upscaler.load();
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
}

void ConfigureUpscalerSupport(bool metalfx, bool fsr3) {
    auto& v = Get();
    v.metalfx_supported = metalfx;
    v.fsr3_supported = fsr3;
    const int requested = v.requested_upscaler;
    const int runs = FallbackUpscaler(requested, metalfx, fsr3, v.preset);
    if (runs != requested) {
        std::printf("Upscaler: %s %s; using %s (the saved choice is kept)\n",
                    UpscalerName(requested),
                    IsFsr4(requested) ? "was removed" : "is unavailable on this device",
                    UpscalerName(runs));
    }
    v.upscaler = runs;
    // Before the first frame: the choice the guest's startup patches were applied for.
    v.startup_upscaler = runs;
}

bool UpscalerAvailable(int upscaler) {
    const auto& v = Get();
    return FallbackUpscaler(upscaler, v.metalfx_supported, v.fsr3_supported, NativeAA) ==
           upscaler;
}

int FallbackUpscaler(int requested, bool metalfx, bool fsr3, int preset) {
    switch (requested) {
    case UpscalerOff:
    case UpscalerTaa:
        return requested;
    case UpscalerMetalFx:
        if (metalfx) return requested;
        break;
    case UpscalerFsr3:
        if (fsr3) return requested;
        break;
    default: // the removed FSR 4 choices, unknown values
        break;
    }
    if (metalfx) {
        return UpscalerMetalFx;
    }
    return preset == NativeAA ? UpscalerTaa : UpscalerOff;
}

void ChooseUpscaler(int upscaler) {
    auto& v = Get();
    v.requested_upscaler = upscaler;
    v.upscaler = FallbackUpscaler(upscaler, v.metalfx_supported, v.fsr3_supported, v.preset);
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    std::vector<std::string> kept;
    if (FILE* old = std::fopen(Path(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), old)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos ||
                OwnedKey(text.substr(0, eq))) {
                continue;
            }
            kept.push_back(text);
        }
        std::fclose(old);
    }
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    // The user's choice, never a fallback that runs in its place.
    const int saved_upscaler = v.requested_upscaler;
    std::printf("Settings: saved %s (upscaler=%s preset=%d jitter=%d)\n", Path(),
                UpscalerName(saved_upscaler), v.preset.load(), int(v.jitter.load()));
    std::fprintf(file,
                 "# bbport settings (in-game menu: " BBPORT_MENU_KEYS " / L3+R3)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nframe_pacing=%d\n",
                 UpscalerName(saved_upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()), int(v.frame_pacing.load()));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res]);
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    static constexpr const char* compile_modes[] = {"wait", "skip", "balanced"};
    std::fprintf(file, "shader_compile=%s\n",
                 compile_modes[std::clamp(v.shader_compile.load(), 0, 2)]);
    for (const std::string& line : kept) {
        std::fprintf(file, "%s\n", line.c_str());
    }
    std::fclose(file);
}

namespace {

const char* LaunchConfPath() {
    const char* env = std::getenv("BB_LAUNCH_CONF");
    return env && env[0] ? env : nullptr;
}

/// launch.conf's lines, without line ends.
std::vector<std::string> LaunchConfLines() {
    std::vector<std::string> lines;
    if (FILE* file = std::fopen(LaunchConfPath(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            lines.push_back(text);
        }
        std::fclose(file);
    }
    return lines;
}

constexpr std::string_view ShowLauncherKey = "BB_SHOW_LAUNCHER=";
int show_launcher = -2; ///< -2: not read yet

} // namespace

int ShowLauncher() {
    if (show_launcher == -2) {
        show_launcher = -1;
        if (LaunchConfPath()) {
            show_launcher = 1; // the launcher's default when the key is absent
            for (const std::string& line : LaunchConfLines()) {
                if (line.starts_with(ShowLauncherKey)) {
                    std::string value = line.substr(ShowLauncherKey.size());
                    std::erase(value, '\'');
                    std::erase(value, '"');
                    show_launcher = value == "0" ? 0 : 1;
                }
            }
        }
    }
    return show_launcher;
}

void SetShowLauncher(bool show) {
    if (!LaunchConfPath()) {
        return;
    }
    std::vector<std::string> lines = LaunchConfLines();
    const std::string entry = std::string(ShowLauncherKey) + (show ? "1" : "0");
    bool replaced = false;
    for (std::string& line : lines) {
        if (line.starts_with(ShowLauncherKey)) {
            line = entry;
            replaced = true;
        }
    }
    if (!replaced) {
        lines.push_back(entry);
    }
    FILE* file = std::fopen(LaunchConfPath(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", LaunchConfPath());
        return;
    }
    for (const std::string& line : lines) {
        std::fprintf(file, "%s\n", line.c_str());
    }
    std::fclose(file);
    show_launcher = show ? 1 : 0;
    std::printf("Settings: saved %s (BB_SHOW_LAUNCHER=%d)\n", LaunchConfPath(), show_launcher);
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "metalfx"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings
