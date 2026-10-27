// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
#include "video_core/renderer_metal/mtl_hdr.h"
#include <array>
namespace Vulkan {
class Scheduler;
struct RenderState;
class GraphicsPipeline;
/// A floating-point companion to the guest SDR pipeline. Original guest targets, AA,
/// grading and display copies remain available as a complete SDR fallback.
class NativeHdr {
public:
    NativeHdr(Metal::Device& device, Scheduler& scheduler);
    ~NativeHdr();
    void OnDisplay(VAddr address);
    void OnDraw(u64 vs, u64 ps);
    void Constants(u64 hash, const void* data, u64 size);
    void GradingLut(void* texture);
    void GradingInput(bool rgb);
    void Attach(RenderState& state);
    bool UiState(RenderState& state);
    Metal::Object TakeFrame(VAddr address);
    bool Enabled() const { return bool(passes); }
    bool InUi() const { return ui_phase && scene_valid && !invalid; }
    void RejectUi() { invalid = true; }

private:
    Metal::Object Make(u32 format, u32 w, u32 h, const char* label);
    Metal::Device& device;
    Scheduler& scheduler;
    std::unique_ptr<Metal::HdrPasses> passes;
    Metal::Object scene, bloom, ui, ui_white, lut;
    struct Frame {
        VAddr address{};
        Metal::Object texture;
        u32 w{}, h{};
        u64 tick{};
        bool valid{};
    };
    std::array<Frame, 3> frames{};
    u32 next_frame{};
    u32 width{}, height{}, ui_width{}, ui_height{};
    bool scene_valid{}, ui_phase{}, ui_written{}, invalid{};
    void* ui_source{};
    Metal::NativeHdrParams params{};
};
} // namespace Vulkan
