// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
#include <cstdio>
// bbport: SDL3 window for the presenter (a CAMetalLayer on macOS; X11 or Wayland elsewhere).
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_mouse.h"
#include "bbport_overlay.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    // bbport: a close request is handled in PollEvents (Cmd+W is ignored there), so SDL must not
    // turn it into a quit as well. Cmd+Q and the menu bar's Quit still send SDL_EVENT_QUIT.
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
#ifdef __APPLE__
    // A Metal window: SDL loads no Vulkan library for it (the app bundles none).
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN, true);
    // Retina: the Metal layer gets the display's pixels, not its points (a 1496x938 point full
    // screen is 2992x1876 pixels; without this the picture was presented at half size and
    // stretched). BB_HIGH_DPI=0 keeps point size.
    const char* high_dpi = std::getenv("BB_HIGH_DPI");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN,
                           !(high_dpi && high_dpi[0] == '0'));
#else
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
#endif
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef __APPLE__
    // The presenter draws into the view's CAMetalLayer.
    (void)wp;
    window_info.type = WindowSystemType::Metal;
    metal_view = SDL_Metal_CreateView(window);
    ASSERT_MSG(metal_view, "Failed to create Metal view: {}", SDL_GetError());
    window_info.render_surface = SDL_Metal_GetLayer(metal_view);
#else
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
#endif
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    UpdateVisible();
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
    std::printf("Window: %dx%d pixels, density %.1f\n", w, h, SDL_GetWindowPixelDensity(window));
}

WindowSDL::~WindowSDL() {
#if defined(__APPLE__)
    if (metal_view) SDL_Metal_DestroyView(metal_view);
#endif
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt, bool password) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_password = password;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    std::string shown = text;
    if (text_password) { // one dot per code point
        size_t points = 0;
        for (unsigned char c : text) points += (c & 0xC0) != 0x80;
        shown.clear();
        for (size_t i = 0; i < points; ++i) shown += "\u2022";
    }
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + shown + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    // Fullscreen has no title bar: the overlay draws the same entry as a box over the frame.
    BbOverlay::SetTextPrompt(text_active, text_prompt, shown);
}

void WindowSDL::UpdateVisible() {
    // SDL keeps these flags from the window events (MINIMIZED/RESTORED, HIDDEN/SHOWN and, from
    // NSWindow's occlusionState, OCCLUDED/EXPOSED).
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    const bool now = !(flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED));
    fullscreen.store((flags & SDL_WINDOW_FULLSCREEN) != 0, std::memory_order_relaxed);
    if (now != visible.exchange(now, std::memory_order_relaxed)) {
        std::printf("Window: %s\n", now ? "visible: presenting"
                                         : "minimised, hidden or occluded: frames not presented");
        std::fflush(stdout);
    }
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    // bbport: Cmd as of the event being handled (SDL_GetModState is the state after the pump,
    // which misses a Cmd+W whose Cmd was let go within the same frame).
    static bool cmd_held = false;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            cmd_held = (event.key.mod & SDL_KMOD_GUI) != 0;
        } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            cmd_held = false;
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // SDL sends button events only for opened gamepads. The game reads the pad in bb-probe
        // (runtime_pad.c), so in the GPU process nothing else opens it: without this the menu
        // never saw L3+R3 or its gamepad navigation.
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            SDL_OpenGamepad(event.gdevice.which);
        }
        if (BbOverlay::HandleEvent(event) || BbMouse::HandleEvent(window, event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            if (w != width || h != height) {
                std::printf("Window: %dx%d pixels\n", w, h);
            }
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_OCCLUDED:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            UpdateVisible();
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            // bbport: Cmd+W. W is "move forward", so pressing Cmd while running (to free the
            // mouse, or Cmd+, for the menu) auto-repeats the held W as Cmd+W and closed the game.
            // The window's close button and Cmd+Q still quit.
            if (cmd_held || (SDL_GetModState() & SDL_KMOD_GUI)) {
                std::printf("Window: Cmd+W ignored (Cmd+Q quits)\n");
                std::fflush(stdout);
                break;
            }
            [[fallthrough]];
        case SDL_EVENT_QUIT:
            // bbport: say which, so an unexpected close can be traced.
            std::printf("Window: %s; closing\n", event.type == SDL_EVENT_QUIT
                                                     ? "quit requested (SDL_EVENT_QUIT)"
                                                     : "window close requested");
            std::fflush(stdout);
            is_open = false;
            break;
        default:
            break;
        }
    }
    BbMouse::Update(window);
    return is_open;
}

} // namespace Frontend
