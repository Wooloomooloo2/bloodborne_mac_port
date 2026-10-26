// SPDX-License-Identifier: GPL-2.0-or-later
// Native game-menu fields. Values cross the existing game/renderer bridge;
// pointers and native widget objects never cross processes.
#ifndef BBMAC_SETTINGS_H
#define BBMAC_SETTINGS_H
#include <stdint.h>
enum BbMacSetting {
    BBMAC_UPSCALER, BBMAC_PRESET, BBMAC_OUTPUT, BBMAC_SHARPEN, BBMAC_SHARPNESS,
    BBMAC_JITTER, BBMAC_OBJECT_MOTION, BBMAC_REACTIVE, BBMAC_REACTIVE_SCALE,
    BBMAC_REACTIVE_THRESHOLD, BBMAC_REACTIVE_MAX, BBMAC_DEBUG_VIEW,
    BBMAC_SHOW_FPS, BBMAC_FRAME_PACING, BBMAC_MODEL_LOD, BBMAC_LIVE_RESOLUTION,
    BBMAC_SHADER_COMPILE,
    BBMAC_EFFECT_FIRST,
    BBMAC_EFFECT_LAST = BBMAC_EFFECT_FIRST + 9,
    BBMAC_FRAME_CAP, BBMAC_GAME_TIMING, BBMAC_FULLSCREEN, BBMAC_METAL_HUD,
    BBMAC_TEXTURE_COMPRESSION, BBMAC_SHOW_LAUNCHER,
    BBMAC_RESTART_NEEDED, BBMAC_PROVIDER_MASK, BBMAC_RESTART,
    BBMAC_NATIVE_ULTRAWIDE,
    BBMAC_COUNT
};
// Reads return -1 for an invalid field (and SHOW_LAUNCHER when unavailable).
// Writes reject invalid/read-only values; in split mode they are asynchronous and the next
// host snapshot reflects them. Floats are hundredths (sharpness 30 = 0.30); enums use BbSettings.
// Frame cap is FPS; timing 0 fixed/1 variable; launch-only edits need restart.
// PROVIDER_MASK uses (1 << BbSettings::Upscaler); NATIVE_ULTRAWIDE is read-only.
#ifdef __cplusplus
extern "C" {
#endif
int32_t bbgpu_mac_setting_get(uint32_t field);
void bbgpu_mac_setting_set(uint32_t field, int32_t value);
#ifdef __cplusplus
}
#endif
#endif
