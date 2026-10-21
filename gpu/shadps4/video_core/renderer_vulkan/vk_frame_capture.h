// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame analyzer. Creating the file named by BB_CAPTURE_TRIGGER records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented. Input for placing a temporal
// upscaler (scene color, depth, motion vectors, UI) in Bloodborne's frame.

#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"

namespace VideoCore {
struct ImageInfo;
}

namespace Vulkan {

class FrameCapture {
public:
    /// VideoOut thread, per flip: frame boundary, presented buffer, trigger file check.
    static void OnFlip(VAddr presented_address);

    /// VideoOut: a display buffer the game registered. A pass drawing into one starts a frame
    /// in the GPU command stream (the flip itself runs a frame behind).
    static void AddDisplayBuffer(VAddr address);
    static bool IsDisplayBuffer(VAddr address);

    /// GPU thread: whether passes are being looked at (armed or recording; cheap).
    static bool Active() {
        return state.load(std::memory_order_relaxed) != Idle;
    }
    /// GPU thread, at every draw/dispatch: starts or ends a recording at frame boundaries.
    static void Poll();

    static void BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                          const VideoCore::ImageInfo* depth);
    static void Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    static void Sampled(const VideoCore::ImageInfo& info, bool storage);
    /// Contents of a bound buffer (first 1 KiB), kept for small passes and a pass's first draw.
    static void Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size);
    static void Note(const char* text);
    /// bbport: the event trace written next to the capture (frame_*.trace.txt): passes, draws,
    /// dispatches, buffer bindings and copy-shader copies in command order, for dependency
    /// analysis. Only while a frame is being recorded.
    static bool Tracing() {
        return state.load(std::memory_order_relaxed) == Recording;
    }
    static void Trace(std::string line);

    /// bbport: records the next full frame, as the trigger file does (the overlay's "Capture
    /// frame" button). Written to BB_CAPTURE_DIR, else ~/Library/Logs/Bloodborne/captures.
    static void Arm() {
        u32 idle = Idle;
        state.compare_exchange_strong(idle, Armed, std::memory_order_acq_rel);
    }
    /// Captures written so far (the overlay shows when its request is done).
    static u64 Written() {
        return written.load(std::memory_order_acquire);
    }

private:
    enum : u32 { Idle, Armed, Recording };
    static inline std::atomic<u32> state{Idle};
    static inline std::atomic<u64> flips{0};
    static inline std::atomic<VAddr> last_presented{0};
    static inline std::atomic<u64> written{0};
};

} // namespace Vulkan
