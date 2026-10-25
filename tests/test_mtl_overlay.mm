// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: unit test for the settings overlay on Metal (Metal-native M5 Task 9): BbOverlay::Init on
// a Metal::Device, then BbOverlay::Render into an offscreen BGRA8 render target, as the presenter
// does into the drawable. Hidden, nothing is drawn (the target keeps its contents); the FPS
// counter draws in the top-right corner only; Cmd+, opens the menu (drawn over the middle, the
// game's input captured, text input consumed) and Escape closes it; with fsr3 saved and MetalFX
// running, clicking the MetalFX segment saves MetalFX. Runs plain and under
// MTL_DEBUG_LAYER=1 (build_macos.sh --test). Links libbbgpu (the overlay, ImGui and its Metal
// backend).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "video_core/renderer_metal/mtl_device.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// The game window's drawable on a 16" MacBook Pro (menu layout as in the game gate).
constexpr u32 Width = 2992, Height = 1672;
constexpr u8 Fill[4] = {30, 20, 10, 255}; // BGRA

struct Target {
    id<MTLCommandQueue> queue;
    id<MTLTexture> texture;
};

/// Clears the target to Fill, then runs one present's overlay step.
void Present(Target& t, bool clear, BbFrameRates::Sample rates = {60, 120}) {
    id<MTLCommandBuffer> cmdbuf = [t.queue commandBuffer];
    if (clear) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = t.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor =
            MTLClearColorMake(Fill[2] / 255.0, Fill[1] / 255.0, Fill[0] / 255.0, 1.0);
        [[cmdbuf renderCommandEncoderWithDescriptor:pass] endEncoding];
    }
    BbOverlay::Render((__bridge void*)cmdbuf, (__bridge void*)t.texture, Width, Height, rates);
    [cmdbuf commit];
    [cmdbuf waitUntilCompleted];
    Expect(cmdbuf.status == MTLCommandBufferStatusCompleted, "overlay command buffer completed");
}

/// Fraction of the pixels in [x0,x1)x[y0,y1) that differ from Fill.
double Changed(Target& t, u32 x0, u32 y0, u32 x1, u32 y1) {
    std::vector<u8> pixels(size_t(Width) * Height * 4);
    [t.texture getBytes:pixels.data()
            bytesPerRow:Width * 4
             fromRegion:MTLRegionMake2D(0, 0, Width, Height)
            mipmapLevel:0];
    size_t changed = 0;
    for (u32 y = y0; y < y1; ++y) {
        for (u32 x = x0; x < x1; ++x) {
            const u8* p = &pixels[(size_t(y) * Width + x) * 4];
            changed += std::memcmp(p, Fill, 4) != 0;
        }
    }
    return double(changed) / double(size_t(x1 - x0) * (y1 - y0));
}

std::array<int, 4> CounterBounds(Target& t) {
    constexpr int w = 600, h = 240;
    std::vector<u8> pixels(w * h * 4);
    [t.texture getBytes:pixels.data() bytesPerRow:w * 4
             fromRegion:MTLRegionMake2D(Width - w, 0, w, h) mipmapLevel:0];
    std::array<int, 4> bounds{w, h, -1, -1};
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        if (std::memcmp(&pixels[(y * w + x) * 4], Fill, 4) == 0) continue;
        bounds[0] = std::min(bounds[0], x); bounds[1] = std::min(bounds[1], y);
        bounds[2] = std::max(bounds[2], x); bounds[3] = std::max(bounds[3], y);
    }
    return bounds;
}

SDL_Event Key(SDL_Keycode key, SDL_Keymod mod, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.key = key;
    event.key.mod = mod;
    event.key.down = down;
    event.key.scancode = key == SDLK_COMMA ? SDL_SCANCODE_COMMA : SDL_SCANCODE_ESCAPE;
    return event;
}

void StickChord() {
    for (const bool down : {true, false}) {
        for (const u8 button : {SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
            event.gbutton.button = button;
            event.gbutton.down = down;
            BbOverlay::HandleEvent(event);
        }
    }
}

void Click(Target& t, float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x; event.motion.y = y;
    BbOverlay::HandleEvent(event);
    Present(t, true);
    for (const bool down : {true, false}) {
        event = {};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.x = x; event.button.y = y;
        BbOverlay::HandleEvent(event);
        Present(t, true);
    }
}

void PadButton(Target& t, u8 button) {
    for (const bool down : {true, false}) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        event.gbutton.button = button;
        event.gbutton.down = down;
        BbOverlay::HandleEvent(event);
        Present(t, true);
    }
}

} // namespace

int main() {
    // Changes the menu would save go to a scratch file, never a real bbport.ini.
    char config[] = "/tmp/bb-overlay-test-XXXXXX";
    const int fd = mkstemp(config);
    if (fd < 0) {
        std::printf("FAIL: no temporary file\n");
        return 1;
    }
    close(fd);
    setenv("BB_CONFIG", config, 1);
    unsetenv("BB_OVERLAY");
    unsetenv("BB_MENU_OPEN");
    unsetenv("BB_METAL_FRAMEGEN");
    @autoreleasepool {
        Metal::Device device;
        id<MTLDevice> mtl = (__bridge id<MTLDevice>)device.NativeDevice();
        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:Width
                                                              height:Height
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        Target t{[mtl newCommandQueue], [mtl newTextureWithDescriptor:desc]};
        BbSettings::Load();
        auto& s = BbSettings::Get();
        s.show_fps = false;

        // Before Init: timing only, nothing drawn, no crash.
        Present(t, true);
        Expect(Changed(t, 0, 0, Width, Height) == 0.0, "no drawing before Init");
        BbOverlay::Init(device, u32(MTLPixelFormatBGRA8Unorm));
        Expect(!BbOverlay::Visible(), "hidden after Init");

        // Hidden: the drawable keeps the game image; a present without a drawable is fine.
        Present(t, true);
        BbOverlay::Render(nullptr, nullptr, 0, 0);
        Expect(Changed(t, 0, 0, Width, Height) == 0.0, "hidden overlay draws nothing");

        // FPS counter: top-right corner only (several frames: the window sizes itself first).
        s.show_fps = true;
        Expect(BbOverlay::Visible(), "visible with the FPS counter on");
        for (int i = 0; i < 4; ++i) {
            Present(t, true);
        }
        Expect(Changed(t, Width - 320, 0, Width, 90) > 0.05, "FPS counter drawn top right");
        Expect(Changed(t, 0, Height / 2, Width / 2, Height) == 0.0,
               "FPS counter leaves the rest of the frame");
        const auto counter = [&] {
            std::vector<u8> pixels(420 * 180 * 4);
            [t.texture getBytes:pixels.data() bytesPerRow:420 * 4
                     fromRegion:MTLRegionMake2D(Width - 420, 0, 420, 180) mipmapLevel:0];
            return pixels;
        };
        const auto counter_before = counter();
        for (int i = 0; i < 4; ++i) Present(t, true, {60, 90});
        Expect(counter() == counter_before, "framegen off hides displayed FPS");
        for (int i = 0; i < 4; ++i) Present(t, true, {30, 90});
        Expect(counter() != counter_before, "framegen off still updates the single FPS reading");
        for (int i = 0; i < 4; ++i) Present(t, true, {60, 120});
        Expect(counter() == counter_before, "FPS labels return to the same measured values");
        const auto normal_bounds = CounterBounds(t);
        s.framegen = true;
        s.framegen_supported = true;
        for (int i = 0; i < 4; ++i) Present(t, true);
        Expect(counter() == counter_before, "enabling framegen waits for restart to show both rates");
        s.startup_framegen = true;
        for (int i = 0; i < 4; ++i) Present(t, true);
        const auto framegen_bounds = CounterBounds(t);
        Expect(framegen_bounds[2] - framegen_bounds[0] > normal_bounds[2] - normal_bounds[0],
               "framegen reserves a wider FPS counter");
        Expect(framegen_bounds[3] - framegen_bounds[1] > normal_bounds[3] - normal_bounds[1],
               "framegen adds a second FPS row");
        const auto framegen_counter = counter();
        for (int i = 0; i < 4; ++i) Present(t, true, {60, 90});
        Expect(counter() != framegen_counter, "displayed FPS changes independently of real FPS");
        s.framegen = false;
        for (int i = 0; i < 4; ++i) Present(t, true);
        Expect(counter() == framegen_counter, "disabling framegen keeps both rates until restart");
        s.framegen_supported = false;
        for (int i = 0; i < 4; ++i) Present(t, true);
        Expect(counter() == counter_before, "unsupported framegen uses the single FPS display");
        s.framegen_supported = true;
        const std::array<BbFrameRates::Sample, 6> changing_rates{{
            {9, 18}, {99, 198}, {100, 200}, {999, 1998}, {0, 0}, {60, 120}}};
        for (const auto rates : changing_rates) {
            for (int i = 0; i < 4; ++i) Present(t, true, rates);
            Expect(CounterBounds(t) == framegen_bounds,
                   "FPS digit counts and unavailable readings cannot shift the counter bounds");
        }
        // A generated/real pair reuses one UI frame. Replaying must draw exactly
        // the same pixels without sampling the FPS clock or consuming input twice.
        Target repeated{t.queue, [mtl newTextureWithDescriptor:desc]};
        auto repeat_cb = [t.queue commandBuffer];
        MTLRenderPassDescriptor* repeat_pass = [MTLRenderPassDescriptor renderPassDescriptor];
        repeat_pass.colorAttachments[0].texture = repeated.texture;
        repeat_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        repeat_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        repeat_pass.colorAttachments[0].clearColor =
            MTLClearColorMake(Fill[2] / 255.0, Fill[1] / 255.0, Fill[0] / 255.0, 1.0);
        [[repeat_cb renderCommandEncoderWithDescriptor:repeat_pass] endEncoding];
        BbOverlay::Replay((__bridge void*)repeat_cb, (__bridge void*)repeated.texture);
        [repeat_cb commit];
        [repeat_cb waitUntilCompleted];
        std::vector<u8> first(size_t(Width) * Height * 4), second(first.size());
        [t.texture getBytes:first.data() bytesPerRow:Width * 4
                   fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
        [repeated.texture getBytes:second.data() bytesPerRow:Width * 4
                   fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
        Expect(repeat_cb.status == MTLCommandBufferStatusCompleted && first == second,
               "generated/real pair uses identical overlay draw data");
        s.startup_framegen = false;
        for (int i = 0; i < 4; ++i) Present(t, true);
        Expect(counter() == counter_before && CounterBounds(t) == normal_bounds,
               "disabling framegen restores the single FPS display and normal bounds");
        for (const auto rates : changing_rates) {
            for (int i = 0; i < 4; ++i) Present(t, true, rates);
            Expect(CounterBounds(t) == normal_bounds,
                   "single FPS counter bounds stay stable across digit counts and unavailable readings");
        }
        s.show_fps = false;

        // Cmd+, opens the menu; the game's input is held while it is open.
        Expect(BbOverlay::HandleEvent(Key(SDLK_COMMA, SDL_KMOD_GUI, true)), "Cmd+, consumed");
        Expect(BbOverlay::HandleEvent(Key(SDLK_COMMA, SDL_KMOD_GUI, false)), "its key-up consumed");
        Expect(BbOverlay::Visible() && BbOverlay::CapturesInput(), "menu open captures input");
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.text = "7";
        Expect(BbOverlay::HandleEvent(text), "typed text goes to the open menu");
        for (int i = 0; i < 4; ++i) {
            Present(t, true);
        }
        Expect(Changed(t, Width / 2 - 100, Height / 2 - 100, Width / 2 + 100, Height / 2 + 100) >
                   0.9,
               "menu drawn over the middle");
        // Contents kept under the menu's edges (load action Load): the corners stay the game's.
        Expect(Changed(t, 0, 0, 16, 16) == 0.0, "corner outside the menu keeps the frame");

        // Compare only the footer: switches elsewhere in the menu animate after a mode change.
        const auto footer = [&] {
            std::vector<u8> pixels(size_t(Width) * Height * 4);
            [t.texture getBytes:pixels.data() bytesPerRow:Width * 4
                     fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
            u32 bottom = Height;
            while (bottom && std::memcmp(&pixels[((bottom - 1) * Width + Width / 2) * 4],
                                          Fill, 4) == 0) --bottom;
            const u32 top = bottom > 64 ? bottom - 64 : 0;
            return std::vector<u8>(pixels.begin() + size_t(top) * Width * 4,
                                   pixels.begin() + size_t(bottom) * Width * 4);
        };
        const auto normal_footer = footer();
        for (int i = 0; i < 4; ++i) Present(t, true, {60, 90});
        Expect(footer() == normal_footer, "framegen off hides displayed FPS in the settings footer");
        s.framegen = s.startup_framegen = true;
        for (int i = 0; i < 4; ++i) Present(t, true);
        const auto framegen_footer = footer();
        for (int i = 0; i < 4; ++i) Present(t, true, {60, 90});
        Expect(footer() != framegen_footer, "framegen on updates displayed FPS in the settings footer");
        s.framegen_supported = false;
        for (int i = 0; i < 4; ++i) Present(t, true);
        const auto unsupported_footer = footer();
        for (int i = 0; i < 4; ++i) Present(t, true, {60, 90});
        Expect(footer() == unsupported_footer, "unavailable framegen hides displayed FPS in the footer");
        s.framegen = s.startup_framegen = false;
        s.framegen_supported = true;

        // Escape closes it; then nothing is drawn and typed text reaches the game again.
        Expect(BbOverlay::HandleEvent(Key(SDLK_ESCAPE, SDL_KMOD_NONE, true)), "Escape consumed");
        Expect(!BbOverlay::Visible(), "menu closed by Escape");
        Expect(!BbOverlay::HandleEvent(text), "typed text not consumed with the menu closed");
        Present(t, true);
        Expect(Changed(t, 0, 0, Width, Height) == 0.0, "closed menu draws nothing");

        // The menu's upscaler control: with fsr3 saved and MetalFX running in its place, a click
        // on the (selected) MetalFX segment saves MetalFX as the choice.
        {
            FILE* file = std::fopen(config, "w");
            std::fputs("upscaler=fsr3\npreset=1\n", file);
            std::fclose(file);
        }
        BbSettings::Load();
        BbSettings::ConfigureUpscalerSupport(true);
        Expect(s.upscaler == BbSettings::UpscalerMetalFx &&
                   s.requested_upscaler == BbSettings::UpscalerFsr3,
               "fsr3 runs as MetalFX");
        Expect(BbOverlay::HandleEvent(Key(SDLK_COMMA, SDL_KMOD_GUI, true)), "menu reopened");
        BbOverlay::HandleEvent(Key(SDLK_COMMA, SDL_KMOD_GUI, false));
        for (int i = 0; i < 3; ++i) {
            Present(t, true);
        }
        // The MetalFX segment of the Display tab at this size (drawable pixels; window id 0
        // means density 1).
        constexpr float mx = 1862.0f, my = 354.0f;
        SDL_Event mouse{};
        mouse.type = SDL_EVENT_MOUSE_MOTION;
        mouse.motion.x = mx;
        mouse.motion.y = my;
        BbOverlay::HandleEvent(mouse);
        Present(t, true);
        for (const bool down : {true, false}) {
            mouse = {};
            mouse.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            mouse.button.button = SDL_BUTTON_LEFT;
            mouse.button.down = down;
            mouse.button.x = mx;
            mouse.button.y = my;
            BbOverlay::HandleEvent(mouse);
            Present(t, true);
        }
        Expect(s.requested_upscaler == BbSettings::UpscalerMetalFx &&
                   s.upscaler == BbSettings::UpscalerMetalFx,
               "click on the running MetalFX makes it the choice");
        // The menu saves 0.3 s after the last change, on a present.
        usleep(400 * 1000);
        Present(t, true);
        {
            char text[4096] = {};
            FILE* file = std::fopen(config, "r");
            const size_t n = file ? std::fread(text, 1, sizeof(text) - 1, file) : 0;
            if (file) std::fclose(file);
            text[n] = 0;
            Expect(std::strstr(text, "\nupscaler=metalfx\n") != nullptr,
                   "the saved ini says upscaler=metalfx");
        }
        BbOverlay::HandleEvent(Key(SDLK_ESCAPE, SDL_KMOD_NONE, true));

        // The L3+R3 menu's frame-generation choice saves immediately, but the
        // current renderer retains its launch state until Apply and Restart.
        s.framegen = s.startup_framegen = false;
        s.framegen_supported = true;
        s.framegen_forced = false;
        StickChord();
        Expect(BbOverlay::CapturesInput(), "L3+R3 opens the settings menu");
        for (int i = 0; i < 4; ++i) Present(t, true);
        // The Frame generation row, below the upscaler card, at this drawable size.
        constexpr float fx = 1300.0f, fy = 548.0f;
        Click(t, fx, fy);
        Expect(s.framegen && BbSettings::FrameGenerationNeedsRestart() && !s.startup_framegen,
               "framegen toggle requests a restart without changing the running mode");
        StickChord();
        Expect(!BbOverlay::CapturesInput(), "L3+R3 closes the settings menu");
        {
            char saved[4096] = {};
            FILE* file = std::fopen(config, "r");
            if (file) { std::fread(saved, 1, sizeof(saved) - 1, file); std::fclose(file); }
            Expect(std::strstr(saved, "\nframegen=1\n") != nullptr,
                   "closing the menu persists the frame-generation preference");
        }
        StickChord();
        for (int i = 0; i < 4; ++i) Present(t, true);
        Click(t, fx, fy);
        Expect(!s.framegen && !BbSettings::FrameGenerationNeedsRestart(),
               "reverting the toggle clears its restart requirement");
        // The clicked row is also a controller navigation target.
        PadButton(t, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        PadButton(t, SDL_GAMEPAD_BUTTON_DPAD_UP);
        PadButton(t, SDL_GAMEPAD_BUTTON_SOUTH);
        Expect(s.framegen, "Cross activates the focused frame-generation row");
        PadButton(t, SDL_GAMEPAD_BUTTON_SOUTH);
        Expect(!s.framegen, "Cross can turn frame generation off again");
        s.framegen_supported = false;
        for (int i = 0; i < 3; ++i) Present(t, true);
        Click(t, fx, fy);
        Expect(!s.framegen, "unsupported interpolation cannot be enabled from the menu");
        s.framegen_supported = true;
        s.framegen_forced = true;
        for (int i = 0; i < 3; ++i) Present(t, true);
        Click(t, fx, fy);
        Expect(!s.framegen, "a launch environment override disables the menu toggle");
        s.framegen_forced = false;
        StickChord();
    }
    std::remove(config);
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: Metal-native settings overlay\n");
    return 0;
}
