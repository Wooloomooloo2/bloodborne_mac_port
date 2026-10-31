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
#import <ImageIO/ImageIO.h>

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>
#include "bbport_overlay.h"
#include "../gpu/bbgpu_trophy.h"
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

// Optional fixture screenshots for visual QA; never captures a desktop or a running game.
void Snapshot(const Target& t, const char* name) {
    const char* folder = std::getenv("BB_OVERLAY_TEST_OUTPUT");
    if (!folder) return;
    std::vector<u8> pixels(size_t(Width)*Height*4);
    [t.texture getBytes:pixels.data() bytesPerRow:Width*4 fromRegion:MTLRegionMake2D(0,0,Width,Height) mipmapLevel:0];
    CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(pixels.data(), Width, Height, 8, Width*4, colors,
        CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | CGBitmapInfo(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(colors);
    CGImageRef image = CGBitmapContextCreateImage(context);
    NSString* path = [[NSString stringWithUTF8String:folder] stringByAppendingPathComponent:[NSString stringWithUTF8String:name]];
    CGImageDestinationRef file = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, nullptr);
    Expect(file != nullptr, "snapshot destination created");
    if (file) {
        CGImageDestinationAddImage(file, image, nullptr);
        Expect(CGImageDestinationFinalize(file), "snapshot written");
        CFRelease(file);
    }
    CGImageRelease(image);
    CGContextRelease(context);
}

/// Clears the target to Fill, then runs one present's overlay step.
void Present(Target& t, bool clear) {
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
    BbOverlay::Render((__bridge void*)cmdbuf, (__bridge void*)t.texture, Width, Height);
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

SDL_Event Key(SDL_Keycode key, SDL_Keymod mod, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.key = key;
    event.key.mod = mod;
    event.key.down = down;
    event.key.scancode = key == SDLK_COMMA ? SDL_SCANCODE_COMMA : SDL_SCANCODE_ESCAPE;
    return event;
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

        // Restored history is visible in the catalog but never announced on launch.
        RuntimeTrophySnapshot trophies{};
        trophies.unlocked[14] = 100;
        bbgpu_trophy_update(&trophies, 0);
        Expect(!BbOverlay::Visible(), "historical trophies do not replay banners");
        // F2 opens the new tab; its visible text comes from the shared generated catalog.
        BbOverlay::HandleEvent(Key(SDLK_F2, SDL_KMOD_NONE, true));
        BbOverlay::HandleEvent(Key(SDLK_F2, SDL_KMOD_NONE, false));
        for (int i = 0; i < 3; ++i) Present(t, true);
        Expect(BbOverlay::Visible() && BbOverlay::CapturesInput(), "F2 opens achievements");
        Snapshot(t, "achievements-tab.png");
        BbOverlay::HandleEvent(Key(SDLK_ESCAPE, SDL_KMOD_NONE, true));
        BbOverlay::HandleEvent(Key(SDLK_ESCAPE, SDL_KMOD_NONE, false));
        trophies.unlocked[30] = 200;
        bbgpu_trophy_update(&trophies, UINT64_C(1) << 30);
        Expect(BbOverlay::Visible(), "live unlock makes the overlay visible with FPS off");
        for (int i = 0; i < 3; ++i) Present(t, true);
        Expect(Changed(t, Width/2 - 250, 0, Width/2 + 250, 180) > .05,
               "achievement banner rendered at the top");
        Expect(!BbOverlay::CapturesInput() && !BbOverlay::HandleEvent(text),
               "unlock banner does not capture game input");
        Expect(Changed(t, 0, Height/2, Width/2, Height) == 0,
               "banner preserves the game image outside its bounds");
        Snapshot(t, "achievement-banner.png");
        trophies.unlocked[21] = 300;
        bbgpu_trophy_update(&trophies, (UINT64_C(1) << 30) | (UINT64_C(1) << 21));
        usleep(6600*1000);
        for (int i = 0; i < 3; ++i) Present(t, true);
        Expect(BbOverlay::Visible() && !BbOverlay::CapturesInput(),
               "next queued unlock appears after the first banner expires");
        s.achievement_banners = false;
        Present(t, true);
        Expect(!BbOverlay::Visible(), "notification setting dismisses the banner");
        s.achievement_banners = true;
        Expect(!BbOverlay::Visible(), "dismissed notifications do not replay");
        BbOverlay::HandleEvent(Key(SDLK_T, SDL_KMOD_GUI | SDL_KMOD_SHIFT, true));
        BbOverlay::HandleEvent(Key(SDLK_T, SDL_KMOD_GUI | SDL_KMOD_SHIFT, false));
        Expect(BbOverlay::CapturesInput(), "Cmd+Shift+T opens achievements");
        BbOverlay::HandleEvent(Key(SDLK_ESCAPE, SDL_KMOD_NONE, true));
        BbOverlay::HandleEvent(Key(SDLK_ESCAPE, SDL_KMOD_NONE, false));
    }
    std::remove(config);
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: Metal-native settings overlay\n");
    return 0;
}
