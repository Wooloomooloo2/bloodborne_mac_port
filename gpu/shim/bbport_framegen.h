// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_metal/mtl_object.h"
#include <atomic>
#include <cstdint>
#include <memory>

namespace Metal {
class Device;
class Encoder;
} // namespace Metal

namespace BbFrameGen {

bool Requested();                      // launch setting; menu changes need a restart
bool Supported(Metal::Device& device); // macOS 26+, queried on the actual device

struct Description {
    uint32_t width{}, height{}, out_width{}, out_height{}, ui_format{};
    uint64_t sequence{};
    float interval{}, near_plane{}, far_plane{}, vertical_fov{}; // seconds, radians
    bool reset{};
};

// Immutable texture allocation for one real frame. A shared pointer pins its
// pool slot through deferred recording, interpolation history, and presentation
// on the second queue.
struct Frame {
    Description description;
    Metal::Object depth, motion, before_ui, ui_change, ui_mask, color, generated;
    std::atomic<bool> interpolated{false};
};
using FramePtr = std::shared_ptr<Frame>;

// Only contiguous real frames with finite camera/timing inputs can share
// history.
bool Continuous(const Description& previous, const Description& current);
// Half a real frame's period. The game FPS cap is a minimum period (0 = uncapped).
double PresentDuration(float interval, uint32_t game_limit);

class Generator {
  public:
    explicit Generator(Metal::Device& device);
    ~Generator();
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;

    // Producer thread. At most four texture sets; an exhausted pool skips
    // generation.
    FramePtr Prepare(const Description& description);
    // Recorder thread, in scene order: before the first HUD draw, then before
    // display copy.
    void Capture(Metal::Encoder& e, const FramePtr& frame, void* depth, void* motion, void* ui);
    void FinishUi(Metal::Encoder& e, const FramePtr& frame, void* ui);
    // Recorder thread, after the game's display/gamma pass: final video-out view
    // -> generated. The first frame after any reset only seeds history;
    // interpolated stays false.
    void Interpolate(Metal::Encoder& e, const FramePtr& frame, void* displayed);
    void Invalidate(); // recorder thread; a real frame without matching scene inputs

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace BbFrameGen
