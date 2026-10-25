// SPDX-License-Identifier: GPL-2.0-or-later
// Native menu edits and the existing overlay share settings, provider selection, restart
// baselines and one ini writer. No game, renderer or original dump is needed for this fixture.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include "bbport_mac_settings.h"
#include "bbport_settings.h"

namespace {
std::string Read(const char* path) {
    std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void Write(const char* path, const char* text) {
    std::ofstream file(path);
    file << text;
    assert(file.good());
}

int Count(const std::string& text, const std::string& line) {
    int count = 0;
    std::istringstream lines(text);
    for (std::string entry; std::getline(lines, entry);) count += entry == line;
    return count;
}
} // namespace

int main() {
    using BbMacSettings::Get;
    using BbMacSettings::Set;
    char config[] = "/tmp/bbport-mac-settings-XXXXXX";
    char launch[] = "/tmp/bbport-mac-launch-XXXXXX";
    int fd = mkstemp(config);
    assert(fd >= 0);
    close(fd);
    fd = mkstemp(launch);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", config, 1);
    setenv("BB_LAUNCH_CONF", launch, 1);
    const char* overrides[] = {"BB_UPSCALER", "BB_UPSCALE_PRESET", "BB_RENDER_RES", "BB_ULTRAWIDE_RES",
        "BB_FSR_SHARPNESS", "BB_JITTER", "BB_REACTIVE", "BB_REACTIVE_SCALE",
        "BB_REACTIVE_THRESHOLD", "BB_REACTIVE_MAX", "BB_OBJECT_MOTION", "BB_SHADER_COMPILE"};
    for (const char* name : overrides) unsetenv(name);

    // An empty file uses the port's precise defaults (all ten effects included).
    BbSettings::Load();
    assert(Get(BBMAC_UPSCALER) == BbSettings::UpscalerOff);
    assert(Get(BBMAC_PRESET) == BbSettings::NativeAA);
    assert(Get(BBMAC_OUTPUT) == BbSettings::OutputDefault);
    assert(Get(BBMAC_SHARPEN) == 1 && Get(BBMAC_SHARPNESS) == 30);
    assert(Get(BBMAC_JITTER) == 0 && Get(BBMAC_OBJECT_MOTION) == 0 && Get(BBMAC_REACTIVE) == 0);
    assert(Get(BBMAC_REACTIVE_SCALE) == 100 && Get(BBMAC_REACTIVE_THRESHOLD) == 20);
    assert(Get(BBMAC_REACTIVE_MAX) == 90 && Get(BBMAC_DEBUG_VIEW) == BbSettings::DebugNone);
    assert(Get(BBMAC_SHOW_FPS) == 0 && Get(BBMAC_FRAME_PACING) == 0);
    assert(Get(BBMAC_MODEL_LOD) == 0 && Get(BBMAC_LIVE_RESOLUTION) == 0);
    assert(Get(BBMAC_SHADER_COMPILE) == BbSettings::ShaderBalanced);
    assert(Get(BBMAC_FRAME_CAP) == 60 && Get(BBMAC_GAME_TIMING) == 0);
    assert(Get(BBMAC_FULLSCREEN) == 0 && Get(BBMAC_METAL_HUD) == 0);
    assert(Get(BBMAC_TEXTURE_COMPRESSION) == 1 && Get(BBMAC_RESTART_NEEDED) == 0);
    assert(Get(BBMAC_NATIVE_ULTRAWIDE) == 0);
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        assert(Get(BBMAC_EFFECT_FIRST + e) == BbSettings::Effects[e].default_on);
    }
    assert(Get(BBMAC_COUNT) == -1 && Get(UINT32_MAX) == -1);

    // A direct launch cannot change launch.conf. The parent process has not cached it yet.
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        unsetenv("BB_LAUNCH_CONF");
        assert(Get(BBMAC_SHOW_LAUNCHER) == -1);
        assert(!Set(BBMAC_SHOW_LAUNCHER, 0));
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    Write(launch, "# launcher options\nBB_SHOW_LAUNCHER='0'\nBB_GAME='/some path/game'\nFUTURE_OPTION=keep\n");
    assert(Get(BBMAC_SHOW_LAUNCHER) == 0);
    assert(Set(BBMAC_SHOW_LAUNCHER, 1) && Get(BBMAC_SHOW_LAUNCHER) == 1);
    assert(Count(Read(launch), "BB_SHOW_LAUNCHER=1") == 1);
    assert(Read(launch).find("BB_GAME='/some path/game'\nFUTURE_OPTION=keep\n") != std::string::npos);

    // Keep the user's unsupported provider and every unowned launcher / future framegen key.
    Write(config, "upscaler=fsr411\npreset=0\nfps_cap=40\ngame_timing=variable\nfullscreen=1\n"
                  "metal_hud=1\ntexture_compression=0\nfsr4_invert_jitter=1\n"
                  "mouse_look=1\nmouse_sensitivity=2.25\nmouse_invert_y=1\n"
                  "future_launcher=leave me alone\nframegen=fsr3\nframegen_multiplier=3\n");
    BbSettings::Load();
    assert(BbSettings::Get().mouse_look && BbSettings::Get().mouse_sensitivity == 2.25f &&
           BbSettings::Get().mouse_invert_y);
    BbSettings::ConfigureUpscalerSupport(true, false);
    assert(Get(BBMAC_UPSCALER) == BbSettings::UpscalerFsr411);
    assert(BbSettings::Get().upscaler == BbSettings::UpscalerMetalFx);
    const int basic = (1 << BbSettings::UpscalerOff) | (1 << BbSettings::UpscalerTaa);
    assert(Get(BBMAC_PROVIDER_MASK) == (basic | (1 << BbSettings::UpscalerMetalFx)));
    assert(!Set(BBMAC_UPSCALER, BbSettings::UpscalerFsr3));
    assert(!Set(BBMAC_UPSCALER, BbSettings::UpscalerFsr4));
    assert(!Set(BBMAC_UPSCALER, BbSettings::UpscalerFsr411));
    assert(Set(BBMAC_SHARPNESS, 57));
    assert(Get(BBMAC_SHARPNESS) == 57);
    auto text = Read(config);
    assert(Count(text, "upscaler=fsr411") == 1 && Count(text, "sharpness=0.57") == 1);
    for (const char* line : {"fsr4_invert_jitter=1", "future_launcher=leave me alone",
                             "framegen=fsr3", "framegen_multiplier=3"}) {
        assert(Count(text, line) == 1);
    }
    BbSettings::ConfigureUpscalerSupport(true, true);
    assert(Get(BBMAC_PROVIDER_MASK) == (basic | (1 << BbSettings::UpscalerMetalFx) |
                                              (1 << BbSettings::UpscalerFsr3)));
    assert(Set(BBMAC_UPSCALER, BbSettings::UpscalerFsr3));
    assert(BbSettings::Get().upscaler == BbSettings::UpscalerFsr3);
    assert(Count(Read(config), "upscaler=fsr3") == 1);

    // Startup-only changes report pending, and reverting them clears pending without a restart.
    assert(Get(BBMAC_RESTART_NEEDED) == 0);
    const int launch_fields[] = {BBMAC_FRAME_CAP, BBMAC_GAME_TIMING, BBMAC_FULLSCREEN,
        BBMAC_METAL_HUD, BBMAC_TEXTURE_COMPRESSION, BBMAC_OBJECT_MOTION,
        BBMAC_MODEL_LOD, BBMAC_LIVE_RESOLUTION};
    for (const int field : launch_fields) {
        const int initial = Get(field);
        const int changed = field == BBMAC_FRAME_CAP ? 120 : initial ? 0 : 1;
        assert(Set(field, changed) && Get(BBMAC_RESTART_NEEDED) == 1);
        assert(Set(field, initial) && Get(BBMAC_RESTART_NEEDED) == 0);
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const int field = BBMAC_EFFECT_FIRST + e;
        const int initial = Get(field);
        assert(Set(field, !initial) && Get(BBMAC_RESTART_NEEDED) == 1);
        assert(Set(field, initial) && Get(BBMAC_RESTART_NEEDED) == 0);
    }
    assert(Set(BBMAC_SHOW_FPS, 1) && Set(BBMAC_FRAME_PACING, 1));
    assert(Set(BBMAC_SHADER_COMPILE, BbSettings::ShaderSkip));
    assert(Get(BBMAC_RESTART_NEEDED) == 0);
    assert(Set(BBMAC_OUTPUT, 3) && Get(BBMAC_RESTART_NEEDED) == 0);
    assert(Set(BBMAC_OUTPUT, BbSettings::Get().startup_output_res));
    setenv("BB_RENDER_RES", "1920x1080", 1);
    assert(Set(BBMAC_PRESET, BbSettings::Performance) && Get(BBMAC_RESTART_NEEDED) == 1);
    assert(Set(BBMAC_PRESET, BbSettings::NativeAA) && Get(BBMAC_RESTART_NEEDED) == 0);
    unsetenv("BB_RENDER_RES");

    // Bounds and read-only fields reject writes without touching atomics or the ini.
    const struct { uint32_t field; int32_t value; } invalid[] = {
        {BBMAC_COUNT, 1}, {UINT32_MAX, 1}, {BBMAC_PROVIDER_MASK, 0}, {BBMAC_RESTART_NEEDED, 1},
        {BBMAC_NATIVE_ULTRAWIDE, 1},
        {BBMAC_RESTART, 1}, {BBMAC_UPSCALER, -1}, {BBMAC_OUTPUT, BbSettings::OutputCount},
        {BBMAC_PRESET, BbSettings::PresetCount}, {BBMAC_SHOW_FPS, 2}, {BBMAC_EFFECT_FIRST, -1},
        {BBMAC_SHARPNESS, 201}, {BBMAC_REACTIVE_SCALE, 1601}, {BBMAC_REACTIVE_THRESHOLD, -1},
        {BBMAC_REACTIVE_MAX, 101}, {BBMAC_FRAME_CAP, 45}, {BBMAC_GAME_TIMING, -1},
        {BBMAC_MODEL_LOD, -3}, {BBMAC_LIVE_RESOLUTION, 2},
        {BBMAC_SHADER_COMPILE, BbSettings::ShaderCompileCount},
    };
    text = Read(config);
    for (const auto& edit : invalid) assert(!Set(edit.field, edit.value));
    assert(Read(config) == text);

    // Native and overlay writes can overlap without losing preserved lines or truncating.
    std::thread overlay([] {
        for (int i = 0; i < 8; ++i) {
            BbSettings::Get().show_fps = (i & 1) != 0;
            BbSettings::Save();
        }
    });
    std::thread native([] {
        for (int i = 0; i < 8; ++i) assert(BbMacSettings::Set(BBMAC_SHARPNESS, 80 + i));
    });
    overlay.join();
    native.join();
    text = Read(config);
    assert(Count(text, "sharpness=0.87") == 1 && Count(text, "show_fps=1") == 1);
    assert(Count(text, "fps_cap=40") == 1 && Count(text, "game_timing=variable") == 1);
    assert(Count(text, "fullscreen=1") == 1 && Count(text, "metal_hud=1") == 1);
    assert(Count(text, "texture_compression=0") == 1);
    assert(Count(text, "mouse_look=1") == 1 && Count(text, "mouse_sensitivity=2.25") == 1);
    assert(Count(text, "mouse_invert_y=1") == 1);
    assert(Count(text, "future_launcher=leave me alone") == 1 && Count(text, "framegen=fsr3") == 1);
    BbSettings::Load();
    BbSettings::ConfigureUpscalerSupport(true, true);
    assert(Get(BBMAC_SHARPNESS) == 87 && Get(BBMAC_SHOW_FPS) == 1);
    assert(Get(BBMAC_FRAME_CAP) == 40 && Get(BBMAC_GAME_TIMING) == 1);
    assert(Get(BBMAC_FULLSCREEN) == 1 && Get(BBMAC_METAL_HUD) == 1);
    assert(Get(BBMAC_TEXTURE_COMPRESSION) == 0 && Get(BBMAC_RESTART_NEEDED) == 0);
    assert(BbSettings::Get().mouse_look && BbSettings::Get().mouse_sensitivity == 2.25f &&
           BbSettings::Get().mouse_invert_y);

    // Wide patches own scene targets; generic provider / size edits must leave them alone.
    setenv("BB_ULTRAWIDE_RES", "3840x1080", 1);
    BbSettings::Load();
    text = Read(config);
    assert(Get(BBMAC_NATIVE_ULTRAWIDE) == 1);
    assert(!Set(BBMAC_UPSCALER, BbSettings::UpscalerMetalFx));
    assert(!Set(BBMAC_PRESET, BbSettings::Performance));
    assert(!Set(BBMAC_OUTPUT, 0) && !Set(BBMAC_LIVE_RESOLUTION, 1));
    assert(Read(config) == text);

    // Unsupported launch values fall back to the same defaults as the launcher.
    unsetenv("BB_ULTRAWIDE_RES");
    Write(config, "fps_cap=45\ngame_timing=unknown\n");
    BbSettings::Load();
    assert(Get(BBMAC_FRAME_CAP) == 60 && Get(BBMAC_GAME_TIMING) == 0);
    assert(Get(BBMAC_FULLSCREEN) == 0 && Get(BBMAC_METAL_HUD) == 0);
    assert(Get(BBMAC_TEXTURE_COMPRESSION) == 1 && Get(BBMAC_RESTART_NEEDED) == 0);

    std::remove(config);
    std::remove(launch);
    std::puts("PASS: native Mac settings share persistence, providers and restart state");
}
