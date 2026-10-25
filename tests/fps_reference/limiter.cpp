// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from Paleblood af97b90, game/frame_timing/frame_limiter.cpp.
// See UPSTREAM.md. CPU-only stock-behavior reference; never installed in the game.
#include "limiter.h"
#include "scalar.h"
#include "src/fps/sprj_flipper.h"
#include <bit>
#include <cstdint>

namespace {

enum class Target { original, fps30, fps60, uncapped };
constexpr int frame_ring_size = 32, frame_history_size = 16;
constexpr std::uint32_t k_interval_30 = 0x3d088889, k_interval_60 = 0x3c888889;
thread_local PalebloodReference::Clock* active_clock;
thread_local bool active_spin;
thread_local void* active_notify;
Target target_fps() { return Target::original; }

struct Timeval {
    int64_t seconds;
    int64_t microseconds;
};

// An object told when the game runs at a 60 FPS target.
struct FpsNotify {
    uint8_t unknown_0x00[0x4c];
    uint8_t flag_a, flag_b;
    uint32_t target_fps;
};
static_assert(offsetof(FpsNotify, flag_a) == 0x4c && offsetof(FpsNotify, target_fps) == 0x50);
constexpr uint32_t notify_fps = 60;

// Constants of the original (read-only data)
constexpr float us_per_second = 1000000.0f, minus_us_per_second = -1000000.0f;
constexpr double us_per_second_double = 1000000.0;
constexpr float min_wait = 0.001f, sleep_threshold = 0.005f, sleep_margin = -0.005f;
constexpr float thousand = 1000.0f, late_tolerance = 0.01f, history_frames = 16.0f;
constexpr uint32_t interval_240_bits = 0x3b888889;   // 1/240 s: the Uncap FPS++ patch's target_frame_seconds
constexpr uint64_t min_budget_divisor = 3;            // catching up never cuts below a third
constexpr uint32_t ring_mask = frame_ring_size - 1;

// What each flip mode sets: sync target_frame_seconds, the mode flag, the target_frame_seconds, and the two windows the
// catch-up looks back over.
struct FlipModeSettings {
    uint32_t frame_advance_count;
    uint8_t mode_flag;
    uint32_t interval_bits;
    uint32_t foreground_history_count, background_history_count;
};
constexpr FlipModeSettings flip_modes[] = {
    {2, 1, k_interval_30, 1, 30},
    {2, 1, k_interval_30, 0, 30},
    {1, 1, k_interval_60, 0, 30},
    {1, 0, k_interval_30, 1, 30},
    {1, 0, k_interval_30, 0, 30},
};
constexpr uint32_t flip_mode_count = sizeof flip_modes / sizeof flip_modes[0];

void set_mode(SprjFlipper *f, uint32_t frame_advance_count, uint8_t mode_flag, uint32_t interval_bits, uint32_t spin,
              uint32_t sleep)
{
    f->frame_advance_count = frame_advance_count;
    f->allow_frame_skip = mode_flag;
    f->target_frame_seconds = std::bit_cast<float>(interval_bits);
    f->foreground_history_count = spin;
    f->background_history_count = sleep;
}

bool sleeping_wait() { return false; }
uint64_t now_us() { return active_clock->Now(); }
void kernel_usleep(uint32_t us) { active_clock->Sleep(us); }

uint64_t seconds_to_us(float seconds) { return engine::double_to_u64(static_cast<double>(seconds) * us_per_second_double); }

// Applies the flip mode, the option's fixed mode, the reset and the overrides. Returns whether
// the reset ran (it skips this frame's wait).
bool apply_mode(SprjFlipper *f, Target target)
{
    const uint8_t pending = f->secondary_mode_requested;
    f->secondary_mode_active = pending;
    f->secondary_mode_requested = 0;
    const uint32_t mode = pending ? f->secondary_flip_mode_raw : f->primary_flip_mode_raw;
    if (target != Target::original) {
        // As the community patches rewrite this part: always one fixed mode (for 30 and 60 only
        // while the real mode is in range), with no catch-up windows, and none of the overrides
        // below (the reset flag is neither honoured nor cleared).
        if (target == Target::uncapped)
            set_mode(f, 1, 0, interval_240_bits, 0, 0);
        else if (mode < flip_mode_count)
            set_mode(f, 1, 1, target == Target::fps30 ? k_interval_30 : k_interval_60, 0, 0);
        return false;
    }
    if (mode < flip_mode_count) {
        const FlipModeSettings &m = flip_modes[mode];
        set_mode(f, m.frame_advance_count, m.mode_flag, m.interval_bits, m.foreground_history_count, m.background_history_count);
    }
    bool reset = false;
    if (f->reset_to_30_fps_pending) {
        set_mode(f, 1, 0, k_interval_30, 0, 0);
        f->reset_to_30_fps_pending = 0;
        reset = true;
    }
    if (f->override_foreground_history_count >= 0) f->foreground_history_count = static_cast<uint32_t>(f->override_foreground_history_count);
    if (f->override_background_history_count >= 0) f->background_history_count = static_cast<uint32_t>(f->override_background_history_count);
    if (f->reset_frame_history) {
        f->foreground_history_count = 0;
        f->background_history_count = 0;
        f->reset_frame_history = 0;
    }
    if (f->force_frame_advance) {
        f->foreground_history_count = 0;
        f->background_history_count = 0;
    }
    return reset;
}

// The frame's budget: the target_frame_seconds, shortened while recent frames ran delayed_or_behind (never below a third).
// late_sum and counted are the recent delayed_or_behind frames' total time and how many were looked at.
uint64_t frame_budget_us(const SprjFlipper *f, uint32_t window, uint64_t &late_sum, uint32_t &counted)
{
    uint64_t budget_us = seconds_to_us(f->target_frame_seconds);
    late_sum = 0;
    counted = 0;
    for (uint32_t back = 0; counted < window; back++) {
        const SprjFlipperFrameHistoryEntry &record = f->frame_history[(f->frame_history_index - back) & ring_mask];
        late_sum += record.elapsed_microseconds;
        counted++;
        if (!record.delayed_or_behind) break;
    }
    if (!f->frame_behind) return budget_us;
    uint64_t limit = static_cast<uint64_t>(counted + 1) * budget_us;
    if (late_sum + budget_us <= limit) return budget_us;
    const uint64_t third = budget_us / min_budget_divisor;
    if (limit <= late_sum) return third;
    limit -= late_sum;
    if (budget_us < limit) limit = budget_us;
    if (third > limit) limit = third;
    return limit;
}

// Waits out the rest of the budget: spins on the clock, sleeping first when the game would (or
// BB_LIMITER_WAIT=sleep says so) and there is time.
void wait(SprjFlipper *f, uint64_t budget_us, bool spin_only, uint64_t &now)
{
    const float budget_s = engine::u64_to_float(budget_us) / us_per_second;
    float remaining = budget_s - engine::u64_to_float(now - f->previous_frame_microseconds) / us_per_second;
    if (0.0f > remaining) return;
    for (;;) {
        const float wait_s = engine::min_of(budget_s, remaining);
        if (!(wait_s > 0.0f)) return;
        if ((!spin_only || sleeping_wait()) && wait_s > min_wait && wait_s > sleep_threshold) {
            const int32_t us = engine::truncate(((wait_s + sleep_margin) * thousand) * thousand);
            if (us > 0) kernel_usleep(static_cast<uint32_t>(us));
        }
        now = now_us();
        f->current_frame_microseconds = now;
        remaining = budget_s + engine::u64_to_float(now - f->previous_frame_microseconds) / minus_us_per_second;
        if (0.0f > remaining) return;
    }
}

// The frame rate from the last 16 frame times (summed in the original's order).
void record_history(SprjFlipper *f, float frame_s)
{
    float sum = f->fps_frame_time_history[1];
    f->fps_frame_time_history[0] = sum;
    sum = sum + 0.0f;
    for (int i = 1; i + 1 < frame_history_size; i++) {
        const float v = f->fps_frame_time_history[i + 1];
        f->fps_frame_time_history[i] = v;
        sum = engine::add(sum, v);   // both can be NaN: the original's order (NaN payload)
    }
    f->fps_frame_time_history[frame_history_size - 1] = frame_s;
    const float total = sum + frame_s;
    f->calculated_fps = total > min_wait ? history_frames / total : 0.0f;
}

}  // namespace

static void reference_pace_frame(SprjFlipper *f)
{
    const bool spin_only = active_spin;
    const Target target = target_fps();
    const bool reset = apply_mode(f, target);

    // The previous frame's end is this frame's start.
    f->previous_frame_microseconds = f->current_frame_microseconds;
    uint64_t now = now_us();
    f->current_frame_microseconds = now;

    const uint32_t window = spin_only ? f->foreground_history_count : f->background_history_count;
    uint64_t late_sum;
    uint32_t counted;
    const uint64_t budget_us = frame_budget_us(f, window, late_sum, counted);
    if (!reset && !f->force_no_sleep) wait(f, budget_us, spin_only, now);

    // Record the frame in the frame_history.
    if (f->force_no_sleep) f->frame_behind = 1;
    f->frame_history_index = (f->frame_history_index + 1) & ring_mask;
    SprjFlipperFrameHistoryEntry &record = f->frame_history[f->frame_history_index];
    record.elapsed_microseconds = now - f->previous_frame_microseconds;
    const uint8_t was_late = f->frame_behind;
    record.delayed_or_behind = was_late;

    const uint64_t elapsed_microseconds = f->current_frame_microseconds - f->previous_frame_microseconds;
    const float frame_s = engine::u64_to_float(elapsed_microseconds) / us_per_second;
    f->measured_frame_seconds = frame_s;

    // On time: within the target_frame_seconds (with 1% tolerance) for this frame and the delayed_or_behind ones before it.
    uint8_t window_used = window == 0;
    if (was_late) window_used = counted >= window;
    const float target_frame_seconds = f->target_frame_seconds;
    const uint64_t tolerance_us = seconds_to_us(target_frame_seconds * late_tolerance);
    const uint64_t allowed = static_cast<uint64_t>(counted + 1) * seconds_to_us(target_frame_seconds) + tolerance_us;
    uint8_t on_time = elapsed_microseconds + late_sum < allowed ? 1 : window_used;
    if (f->force_no_sleep) on_time = 0;
    f->frame_behind = on_time ^ 1;
    // The patches keep the sync target_frame_seconds when the frame was delayed_or_behind (the original zeroes it).
    if (!on_time && target == Target::original) f->frame_advance_count = 0;

    record_history(f, frame_s);

    f->force_no_sleep = 0;
    if (FpsNotify *notify = static_cast<FpsNotify*>(active_notify); notify && f->request_window_60_hz) {
        notify->flag_a = 1;
        notify->flag_b = 1;
        notify->target_fps = notify_fps;
    }
    f->request_window_60_hz = 0;
}

void PalebloodReference::PaceFrame(SprjFlipper& state, Clock& clock, bool spin_only, void* notify) {
    active_clock = &clock;
    active_spin = spin_only;
    active_notify = notify;
    reference_pace_frame(&state);
}
