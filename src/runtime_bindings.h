// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
/* Keyboard and mouse bindings for the pad emulation (runtime_pad.c): bbport.ini bind_<action>=
 * lists up to BIND_SLOTS inputs, comma-separated, by SDL scancode name ("Space", "Left Shift",
 * "W") or mouse name ("Mouse Left", "Mouse Right", "Mouse Middle", "Mouse Back", "Mouse Forward",
 * "Wheel Up", "Wheel Down"). An absent key keeps the default, an empty one unbinds the action.
 * Scancodes are key positions, so other keyboard layouts keep the same shape. Cmd is reserved
 * (Mac shortcuts, it gives the cursor back). The launcher's Controls page writes these keys
 * (launcher/Model/Bindings.swift: same actions, names and defaults). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BIND_SLOTS 4

enum {
    BIND_MOVE_FORWARD, BIND_MOVE_BACK, BIND_MOVE_LEFT, BIND_MOVE_RIGHT, BIND_WALK,
    BIND_CAMERA_UP, BIND_CAMERA_DOWN, BIND_CAMERA_LEFT, BIND_CAMERA_RIGHT,
    BIND_CROSS, BIND_CIRCLE, BIND_SQUARE, BIND_TRIANGLE,
    BIND_R1, BIND_R2, BIND_L1, BIND_L2, BIND_R3, BIND_L3,
    BIND_DPAD_UP, BIND_DPAD_DOWN, BIND_DPAD_LEFT, BIND_DPAD_RIGHT,
    BIND_OPTIONS, BIND_TOUCH_LEFT, BIND_TOUCH_RIGHT,
    BIND_COUNT
};

/* Mouse state for runtime_bindings_held: SDL_BUTTON_MASK bits (left .. forward side button) and,
 * while a wheel step is being pressed, these. */
#define BIND_WHEEL_UP_MASK (1u << 8)
#define BIND_WHEEL_DOWN_MASK (1u << 9)

/* Reads the bindings from an ini file (NULL or unreadable: the defaults); logs what differs from
 * the defaults and every name it ignores. Can be called again (tests). */
void runtime_bindings_load(const char *ini_path);
/* The actions held (bit per BIND_*), from SDL's keyboard state (NULL: none) and the mouse mask. */
uint64_t runtime_bindings_held(const bool *keys, uint32_t mouse);
/* bbport.ini camera_keys_dpad (default on): while the mouse turns the camera, the camera keys
 * press the d-pad instead (the arrow keys then scroll menus and switch weapons and items). */
bool runtime_bindings_camera_keys_dpad(void);
/* Whether any action is bound to the wheel (only then are wheel steps turned into presses). */
bool runtime_bindings_use_wheel(void);
/* The ini key of an action ("bind_cross") and its default list ("Space,Return,Keypad Enter"),
 * for logs and tests. */
const char *runtime_bindings_key(int action);
const char *runtime_bindings_default(int action);
