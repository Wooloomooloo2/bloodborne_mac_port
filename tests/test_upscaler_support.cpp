// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Saved upscaler choices must be safe before the first rendered frame: a provider this build or
// device cannot run (FSR 3.1 below Apple7, the removed FSR 4 choices, MetalFX without a scaler)
// runs as its fallback, and the ini keeps what the user chose.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include "bbport_settings.h"

namespace {

std::string ReadFile(const char* path) {
    std::string text;
    if (FILE* file = std::fopen(path, "r")) {
        char buffer[512];
        size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, n);
        }
        std::fclose(file);
    }
    return text;
}

void WriteFile(const char* path, const char* text) {
    FILE* file = std::fopen(path, "w");
    assert(file);
    std::fputs(text, file);
    std::fclose(file);
}

bool SavedUpscalerIs(const char* path, int upscaler) {
    const std::string line = std::string("\nupscaler=") + BbSettings::UpscalerName(upscaler) + "\n";
    return ReadFile(path).find(line) != std::string::npos;
}

} // namespace

int main() {
    using namespace BbSettings;
    char path[] = "/tmp/bbport-upscaler-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    auto& s = Get();
    ConfigureUpscalerSupport(false, false, true);
    ChooseUpscaler(UpscalerSpatial);
    assert(s.upscaler == UpscalerSpatial && UpscalerAvailable(UpscalerSpatial));
    Save(); Load();
    ConfigureUpscalerSupport(false, false, true);
    assert(s.upscaler == UpscalerSpatial && s.requested_upscaler == UpscalerSpatial);
    ConfigureUpscalerSupport(false, false, false);
    assert(s.upscaler != UpscalerSpatial && s.requested_upscaler == UpscalerSpatial);
    assert(FallbackUpscaler(UpscalerSpatial, false, false, Performance, true) == UpscalerSpatial);
    assert(FallbackUpscaler(UpscalerSpatial, false, false, Performance, false) == UpscalerOff);
    ChooseUpscaler(UpscalerTaa);
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);

    // The fallback rule on its own: available choices run; others fall to MetalFX, else TAA at
    // Native AA, else off. FSR 3.1 is never a fallback.
    for (bool metalfx : {false, true}) {
        for (int preset = 0; preset < PresetCount; ++preset) {
            const int fallback =
                metalfx ? UpscalerMetalFx : preset == NativeAA ? UpscalerTaa : UpscalerOff;
            assert(FallbackUpscaler(UpscalerOff, metalfx, false, preset) == UpscalerOff);
            assert(FallbackUpscaler(UpscalerTaa, metalfx, false, preset) == UpscalerTaa);
            assert(FallbackUpscaler(UpscalerMetalFx, metalfx, false, preset) ==
                   (metalfx ? UpscalerMetalFx : fallback));
            assert(FallbackUpscaler(UpscalerFsr3, metalfx, false, preset) == fallback);
            assert(FallbackUpscaler(UpscalerFsr3, metalfx, true, preset) == UpscalerFsr3);
            assert(FallbackUpscaler(UpscalerFsr4, metalfx, true, preset) == fallback);
            assert(FallbackUpscaler(UpscalerFsr411, metalfx, true, preset) == fallback);
        }
    }

    // Every saved choice, with and without MetalFX, at a scaled and the native preset.
    for (bool metalfx : {false, true}) {
        for (int preset : {int(Performance), int(NativeAA)}) {
            for (int requested = 0; requested < UpscalerCount; ++requested) {
                char text[128];
                std::snprintf(text, sizeof(text), "upscaler=%s\npreset=%d\noutput_res=2560x1440\n",
                              UpscalerName(requested), preset);
                WriteFile(path, text);
                Load();
                ConfigureUpscalerSupport(metalfx);
                const bool available = requested == UpscalerOff || requested == UpscalerTaa ||
                                       (requested == UpscalerMetalFx && metalfx);
                const int runs = available           ? requested
                                 : metalfx            ? UpscalerMetalFx
                                 : preset == NativeAA ? UpscalerTaa
                                                      : UpscalerOff;
                assert(s.requested_upscaler == requested);
                assert(s.upscaler == runs);
                assert(UpscalerAvailable(runs));
                assert(UpscalerAvailable(requested) == available);
                assert(!UpscalerAvailable(UpscalerFsr3) && !UpscalerAvailable(UpscalerFsr4));
                assert(s.metalfx_supported == metalfx && !s.fsr3_supported);
                assert(s.preset == preset && s.output_res == 2);
                // Startup patch settings describe what runs from the first frame.
                assert(s.startup_upscaler == s.upscaler);
                ConfigureUpscalerSupport(metalfx);
                assert(s.upscaler == runs);
                // A Save() with the choice untouched writes the user's value back.
                Save();
                assert(SavedUpscalerIs(path, requested));
                s.upscaler = UpscalerOff;
                s.requested_upscaler = UpscalerOff;
                Load();
                assert(s.requested_upscaler == requested);
            }
        }
    }

    // The menu's choice is what is saved (M5 Task 1 ruling): picking Off after loading FSR 4
    // persists Off; loading FSR 4 and leaving it persists FSR 4.
    WriteFile(path, "upscaler=fsr4\npreset=1\nfsr4_auto_exposure=0\nfsr4_invert_jitter=1\n");
    Load();
    ConfigureUpscalerSupport(true);
    assert(s.upscaler == UpscalerMetalFx && s.requested_upscaler == UpscalerFsr4);
    Save();
    assert(SavedUpscalerIs(path, UpscalerFsr4));
    // The removed FSR 4 checks stay in the file as the user left them (read, ignored).
    assert(ReadFile(path).find("\nfsr4_auto_exposure=0\n") != std::string::npos);
    assert(ReadFile(path).find("\nfsr4_invert_jitter=1\n") != std::string::npos);
    ChooseUpscaler(UpscalerOff);
    assert(s.upscaler == UpscalerOff && s.requested_upscaler == UpscalerOff);
    Save();
    assert(SavedUpscalerIs(path, UpscalerOff));
    Load();
    ConfigureUpscalerSupport(true);
    assert(s.upscaler == UpscalerOff && s.requested_upscaler == UpscalerOff);
    // A menu choice that cannot run (the menu disables it) runs as its fallback but is saved.
    ChooseUpscaler(UpscalerFsr3);
    assert(s.upscaler == UpscalerMetalFx && s.requested_upscaler == UpscalerFsr3);
    Save();
    assert(SavedUpscalerIs(path, UpscalerFsr3));
    ChooseUpscaler(UpscalerMetalFx);
    Save();
    assert(s.upscaler == UpscalerMetalFx && SavedUpscalerIs(path, UpscalerMetalFx));
    // With the Metal FidelityFX back end (M6) FSR 3.1 runs where the device supports it.
    ConfigureUpscalerSupport(true, true);
    ChooseUpscaler(UpscalerFsr3);
    assert(s.upscaler == UpscalerFsr3 && s.requested_upscaler == UpscalerFsr3);
    assert(UpscalerAvailable(UpscalerFsr3) && s.fsr3_supported);
    // A saved FSR 3.1 choice runs as saved from the first frame.
    Save();
    s.upscaler = UpscalerOff;
    Load();
    ConfigureUpscalerSupport(true, true);
    assert(s.upscaler == UpscalerFsr3 && s.startup_upscaler == UpscalerFsr3);
    // Without it (below Apple7) the choice falls back again and stays saved.
    ConfigureUpscalerSupport(true, false);
    assert(s.upscaler == UpscalerMetalFx && s.requested_upscaler == UpscalerFsr3);
    ChooseUpscaler(UpscalerMetalFx);

    s.startup_preset = Quality;
    s.startup_upscaler = UpscalerFsr3;
    s.upscaler = UpscalerFsr3;
    s.preset = Performance;
    assert(RenderPreset() == Performance && !ResolutionNeedsRestart());
    setenv("BB_RENDER_RES", "1706x960", 1);
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart()); // provider change at the same patched size is live
    s.upscaler = UpscalerTaa;          // TAA needs native guest targets
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const int output = s.startup_output_res;
    s.output_res = (output + 1) % OutputCount;
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    setenv("BB_RENDER_RES", "", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    unsetenv("BB_RENDER_RES");
    std::remove(path);
    std::puts("Upscaler support: saved choices and fallbacks PASS");
}
