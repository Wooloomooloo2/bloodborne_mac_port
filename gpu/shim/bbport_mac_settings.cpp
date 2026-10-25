// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_mac_settings.h"

#include <cmath>
#include "bbport_settings.h"

namespace BbMacSettings {
namespace {
int32_t Hundredths(float value) {
    return static_cast<int32_t>(std::lround(value * 100.0f));
}

bool Boolean(int32_t value) {
    return value == 0 || value == 1;
}
} // namespace

static_assert(BbSettings::EffectCount == BBMAC_EFFECT_LAST - BBMAC_EFFECT_FIRST + 1);

bool RestartNeeded() {
    const auto& s = BbSettings::Get();
    if (s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution || BbSettings::ResolutionNeedsRestart() ||
        s.fps_cap != s.startup_fps_cap || s.game_timing != s.startup_game_timing ||
        s.fullscreen != s.startup_fullscreen || s.metal_hud != s.startup_metal_hud ||
        s.texture_compression != s.startup_texture_compression) {
        return true;
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        if (s.effects[e] != s.startup_effects[e]) return true;
    }
    return false;
}

int32_t Get(uint32_t field) {
    const auto& s = BbSettings::Get();
    if (field >= BBMAC_EFFECT_FIRST && field <= BBMAC_EFFECT_LAST) {
        return s.effects[field - BBMAC_EFFECT_FIRST];
    }
    switch (field) {
    case BBMAC_UPSCALER: return s.requested_upscaler;
    case BBMAC_PRESET: return s.preset;
    case BBMAC_OUTPUT: return s.output_res;
    case BBMAC_SHARPEN: return s.sharpen;
    case BBMAC_SHARPNESS: return Hundredths(s.sharpness);
    case BBMAC_JITTER: return s.jitter;
    case BBMAC_OBJECT_MOTION: return s.object_motion;
    case BBMAC_REACTIVE: return s.reactive;
    case BBMAC_REACTIVE_SCALE: return Hundredths(s.reactive_scale);
    case BBMAC_REACTIVE_THRESHOLD: return Hundredths(s.reactive_threshold);
    case BBMAC_REACTIVE_MAX: return Hundredths(s.reactive_max);
    case BBMAC_DEBUG_VIEW: return s.debug_view;
    case BBMAC_SHOW_FPS: return s.show_fps;
    case BBMAC_FRAME_PACING: return s.frame_pacing;
    case BBMAC_MODEL_LOD: return s.model_lod;
    case BBMAC_LIVE_RESOLUTION: return s.live_resolution;
    case BBMAC_SHADER_COMPILE: return s.shader_compile;
    case BBMAC_FRAME_CAP: return s.fps_cap;
    case BBMAC_GAME_TIMING: return s.game_timing;
    case BBMAC_FULLSCREEN: return s.fullscreen;
    case BBMAC_METAL_HUD: return s.metal_hud;
    case BBMAC_TEXTURE_COMPRESSION: return s.texture_compression;
    case BBMAC_SHOW_LAUNCHER: return BbSettings::ShowLauncher();
    case BBMAC_RESTART_NEEDED: return RestartNeeded();
    case BBMAC_PROVIDER_MASK: {
        int32_t mask = 0;
        for (int u = 0; u < BbSettings::UpscalerCount; ++u) {
            if (BbSettings::UpscalerAvailable(u)) mask |= 1 << u;
        }
        return mask;
    }
    case BBMAC_RESTART: return 0;
    case BBMAC_NATIVE_ULTRAWIDE: return s.ultrawide_width != 0;
    default: return -1;
    }
}

bool Set(uint32_t field, int32_t value) {
    auto& s = BbSettings::Get();
    if (field >= BBMAC_EFFECT_FIRST && field <= BBMAC_EFFECT_LAST) {
        if (!Boolean(value)) return false;
        s.effects[field - BBMAC_EFFECT_FIRST] = value != 0;
        BbSettings::Save();
        return true;
    }
    switch (field) {
    case BBMAC_UPSCALER:
        if (s.ultrawide_width || value < 0 || value >= BbSettings::UpscalerCount ||
            !BbSettings::UpscalerAvailable(value)) return false;
        BbSettings::ChooseUpscaler(value);
        break;
    case BBMAC_PRESET:
        if (s.ultrawide_width || value < 0 || value >= BbSettings::PresetCount) return false;
        s.preset = value;
        break;
    case BBMAC_OUTPUT:
        if (s.ultrawide_width || value < 0 || value >= BbSettings::OutputCount) return false;
        s.output_res = value;
        break;
    case BBMAC_SHARPNESS:
        if (value < 0 || value > 200) return false;
        s.sharpness = value / 100.0f;
        break;
    case BBMAC_REACTIVE_SCALE:
        if (value < 0 || value > 1600) return false;
        s.reactive_scale = value / 100.0f;
        break;
    case BBMAC_REACTIVE_THRESHOLD:
        if (value < 0 || value > 100) return false;
        s.reactive_threshold = value / 100.0f;
        break;
    case BBMAC_REACTIVE_MAX:
        if (value < 0 || value > 100) return false;
        s.reactive_max = value / 100.0f;
        break;
    case BBMAC_DEBUG_VIEW:
        if (value < 0 || value >= BbSettings::DebugViewCount) return false;
        s.debug_view = value;
        break;
    case BBMAC_MODEL_LOD:
        if (value < -2 || value > 2) return false;
        s.model_lod = value;
        break;
    case BBMAC_LIVE_RESOLUTION:
        if (s.ultrawide_width || value < -1 || value > 1) return false;
        s.live_resolution = value;
        break;
    case BBMAC_SHADER_COMPILE:
        if (value < 0 || value >= BbSettings::ShaderCompileCount) return false;
        s.shader_compile = value;
        break;
    case BBMAC_FRAME_CAP:
        if (value != 30 && value != 40 && value != 60 && value != 90 && value != 120) return false;
        s.fps_cap = value;
        break;
    case BBMAC_SHARPEN:
    case BBMAC_JITTER:
    case BBMAC_OBJECT_MOTION:
    case BBMAC_REACTIVE:
    case BBMAC_SHOW_FPS:
    case BBMAC_FRAME_PACING:
    case BBMAC_GAME_TIMING:
    case BBMAC_FULLSCREEN:
    case BBMAC_METAL_HUD:
    case BBMAC_TEXTURE_COMPRESSION:
    case BBMAC_SHOW_LAUNCHER:
        if (!Boolean(value)) return false;
        switch (field) {
        case BBMAC_SHARPEN: s.sharpen = value != 0; break;
        case BBMAC_JITTER: s.jitter = value != 0; break;
        case BBMAC_OBJECT_MOTION: s.object_motion = value != 0; break;
        case BBMAC_REACTIVE: s.reactive = value != 0; break;
        case BBMAC_SHOW_FPS: s.show_fps = value != 0; break;
        case BBMAC_FRAME_PACING: s.frame_pacing = value != 0; break;
        case BBMAC_GAME_TIMING: s.game_timing = value; break;
        case BBMAC_FULLSCREEN: s.fullscreen = value != 0; break;
        case BBMAC_METAL_HUD: s.metal_hud = value != 0; break;
        case BBMAC_TEXTURE_COMPRESSION: s.texture_compression = value != 0; break;
        case BBMAC_SHOW_LAUNCHER:
            if (BbSettings::ShowLauncher() < 0) return false;
            BbSettings::SetShowLauncher(value != 0);
            return true;
        }
        break;
    default: return false; // status fields and restart are handled by the C bridge
    }
    BbSettings::Save();
    return true;
}
} // namespace BbMacSettings
