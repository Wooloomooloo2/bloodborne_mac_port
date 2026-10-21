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
    std::fprintf(f, "# a comment\nupscaler=fsr3\nfps_cap=40\nfullscreen=1\nrenderer=metal\npreset=2\n");
    std::fclose(f);
    Load();
    assert(Get().upscaler == UpscalerFsr3 && Get().preset == 2);
    Get().preset = 3;
    Save();
    std::string text = Read(path);
    assert(Count(text, "fps_cap=40") == 1 && Count(text, "fullscreen=1") == 1);
    assert(Count(text, "renderer=") == 0); // the removed renderer setting is dropped
    assert(Count(text, "preset=3") == 1 && Count(text, "preset=2") == 0);
    assert(Count(text, "upscaler=fsr3") == 1 && Count(text, "# a comment") == 0);
    Save(); // again: still once each
    text = Read(path);
    assert(Count(text, "fps_cap=40") == 1 && Count(text, "fullscreen=1") == 1);
    assert(Count(text, "preset=3") == 1);
    std::remove(path);
    std::puts("PASS: settings menu keeps the launcher's keys");
    return 0;
}
