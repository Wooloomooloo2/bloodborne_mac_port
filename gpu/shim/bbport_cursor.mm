// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_cursor.h"

#import <AppKit/AppKit.h>
#include <chrono>
#include <cstdio>
#include <string>

namespace BbCursor {

namespace {
const auto g_loaded = std::chrono::steady_clock::now(); // the GPU host's start
}

// SDL hides the cursor through the window's cursor rectangles, which AppKit only applies when
// the mouse moves: a still mouse (playing with a controller) kept showing the cursor. This
// hides it now; AppKit shows it again on the next mouse motion, as SDL's motion events do.
void HideUntilMouseMoves() {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSCursor setHiddenUntilMouseMoves:YES];
    });
}

void ActivateAfterWindowMove(const char* why) {
    // Only while starting: later moves are the player's, and an app they switched to keeps
    // the focus.
    if (std::chrono::steady_clock::now() - g_loaded > std::chrono::seconds(30)) {
        return;
    }
    std::string reason = why;
    // After the Space switch has settled (it hands the front to that display's last app).
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if ([NSApp isActive]) {
            return;
        }
        NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
        std::printf("Window: %s, %s is in front: activating the game\n", reason.c_str(),
                    front ? front.localizedName.UTF8String : "no app");
        [NSApp activateIgnoringOtherApps:YES];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            std::printf("Window: game %s\n", [NSApp isActive] ? "active" : "still NOT active");
        });
    });
}

} // namespace BbCursor
