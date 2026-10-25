// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SprjFlipper: presentation cadence and measured frame time.
// Layout recovered from constructor 0x802034520 and update 0x802034770
// (Ghidra addresses). Size 0x2c8.


#include <cstddef>
#include <cstdint>

enum class SprjFlipMode : std::uint32_t {
    Fps30 = 0,
    Fps30WithoutSkip = 1,
    Fps60WithoutSkip = 2,
    Fps30WithoutTearing = 3,
    Fps30WithoutSkipTearing = 4,
};

struct SprjFlipperFrameHistoryEntry {
    std::uint64_t elapsed_microseconds;
    std::uint8_t delayed_or_behind;
    std::uint8_t pad09[7];
};

struct SprjFlipper {
    void* vftable;
    std::uint32_t primary_flip_mode_raw;
    std::uint32_t secondary_flip_mode_raw;
    std::uint32_t frame_advance_count;
    std::uint8_t allow_frame_skip;
    std::uint8_t pad15[3];
    float target_frame_seconds;
    std::uint8_t pad1c[4];
    std::uint64_t previous_frame_microseconds;
    std::uint64_t current_frame_microseconds;
    std::uint64_t phase_timestamps_microseconds[6];
    SprjFlipperFrameHistoryEntry frame_history[32];
    std::uint32_t frame_history_index;
    float measured_frame_seconds;
    std::uint32_t foreground_history_count;
    std::uint32_t background_history_count;
    std::uint8_t frame_behind;
    std::uint8_t reset_frame_history;   // +0x271: both history counts zeroed once
    std::uint8_t force_frame_advance;   // +0x272: both history counts zeroed this frame (Update reads it, never clears it)
    std::uint8_t force_no_sleep;        // +0x273: no wait this frame, the frame counts as behind; cleared by Update
    std::uint8_t request_window_60_hz;
    std::uint8_t secondary_mode_active;
    std::uint8_t secondary_mode_requested;
    std::uint8_t pad277;
    float fps_frame_time_history[16];
    float calculated_fps;
    std::int32_t override_foreground_history_count;
    std::int32_t override_background_history_count;
    std::uint8_t reset_to_30_fps_pending;
    std::uint8_t use_realtime_clock;
    std::uint8_t pad2c6[2];
};

static_assert(sizeof(SprjFlipperFrameHistoryEntry) == 0x10, "history entry");
static_assert(sizeof(SprjFlipper) == 0x2c8, "SprjFlipper");
static_assert(offsetof(SprjFlipper, primary_flip_mode_raw) == 0x08, "");
static_assert(offsetof(SprjFlipper, frame_advance_count) == 0x10, "");
static_assert(offsetof(SprjFlipper, target_frame_seconds) == 0x18, "");
static_assert(offsetof(SprjFlipper, frame_history) == 0x60, "");
static_assert(offsetof(SprjFlipper, frame_history_index) == 0x260, "");
static_assert(offsetof(SprjFlipper, measured_frame_seconds) == 0x264, "");
static_assert(offsetof(SprjFlipper, calculated_fps) == 0x2b8, "");
static_assert(offsetof(SprjFlipper, reset_to_30_fps_pending) == 0x2c4, "");
static_assert(offsetof(SprjFlipper, force_no_sleep) == 0x273, "");
