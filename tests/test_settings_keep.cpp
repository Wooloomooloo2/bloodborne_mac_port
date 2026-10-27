// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game menu's Save() keeps the lines it does not own (the launcher's fps_cap, fullscreen).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include "bbport_settings.h"

static std::string Read(const char* path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

/// Lines equal to `line`.
static int Count(const std::string& text, const std::string& line) {
    int n = 0;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
        n += l == line;
    }
    return n;
}

int main() {
    using namespace BbSettings;
    char path[] = "/tmp/bbport-settings-keep-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    FILE* f = std::fopen(path, "w");
    assert(f);
    std::fprintf(f, "# a comment\nupscaler=fsr3\nfps_cap=40\nfullscreen=1\nrenderer=metal\npreset=2\nhdr=1\nhdr_paper_white=1.25\nhdr_peak=4\n");
    std::fclose(f);
    Load();
    assert(Get().upscaler == UpscalerFsr3 && Get().preset == 2);
    assert(Get().hdr && Get().startup_hdr && Get().hdr_paper_white == 1.25f && Get().hdr_peak == 4.0f);
    Get().preset = NativeAA;
    assert(NativeHdrSupported());
    Get().preset = 1;
    assert(!NativeHdrSupported());
    Get().preset = NativeAA;
    setenv("BB_RENDER_RES", "1920x1080", 1);
    assert(!NativeHdrSupported());
    unsetenv("BB_RENDER_RES");
    Get().hdr = false; Get().hdr_paper_white = 0.75f; Get().hdr_peak = 0.0f;
    Get().preset = 3;
    Save();
    std::string text = Read(path);
    assert(Count(text, "fps_cap=40") == 1 && Count(text, "fullscreen=1") == 1);
    assert(Count(text, "hdr=0") == 1 && Count(text, "hdr_paper_white=0.75") == 1 && Count(text, "hdr_peak=0.00") == 1);
    assert(Get().startup_hdr); // changing the saved choice cannot change a running layer
    assert(Count(text, "renderer=") == 0); // the removed renderer setting is dropped
    assert(Count(text, "preset=3") == 1 && Count(text, "preset=2") == 0);
    assert(Count(text, "upscaler=fsr3") == 1 && Count(text, "# a comment") == 0);
    Save(); // again: still once each
    text = Read(path);
    assert(Count(text, "fps_cap=40") == 1 && Count(text, "fullscreen=1") == 1);
    assert(Count(text, "preset=3") == 1);
    setenv("BB_ULTRAWIDE_RES", "3840x1080", 1);
    f = std::fopen(path, "w");
    assert(f);
    std::fprintf(f, "output_res=3840x2160\nupscaler=metalfx\npreset=3\nlive_resolution=1\nultrawide_res=3840x1080\nstandard_output_res=2560x1440\n");
    std::fclose(f);
    Load();
    assert(Get().output_res == OutputDefault && Get().upscaler == UpscalerOff);
    assert(!Get().hdr && !Get().startup_hdr && Get().hdr_paper_white == 1.0f && Get().hdr_peak == 0.0f);
    assert(Get().preset == NativeAA && Get().live_resolution == 0);
    Save();
    text = Read(path);
    assert(Count(text, "ultrawide_res=3840x1080") == 1);
    assert(Count(text, "standard_output_res=2560x1440") == 1);
    assert((OutputSize() == std::array<int, 2>{3840, 1080}));
    unsetenv("BB_ULTRAWIDE_RES");
    Load();
    assert((OutputSize() == std::array<int, 2>{1920, 1080}));
    setenv("BB_ULTRAWIDE_RES", "3840x1080garbage", 1);
    Load();
    assert(Get().ultrawide_width == 0);
    unsetenv("BB_ULTRAWIDE_RES");
    std::remove(path);
    std::puts("PASS: settings menu keeps the launcher's keys");
    return 0;
}
