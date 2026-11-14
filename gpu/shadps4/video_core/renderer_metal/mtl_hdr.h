// SPDX-License-Identifier: GPL-2.0-or-later
// Scene-based HDR tone mapping and Metal EDR output. See docs/HDR.md for provenance.
#pragma once

#include <memory>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
class Device;
class Encoder;

struct DisplayRange {
    float current{1.0f};
    float potential{1.0f};
};
/// Window thread only. `window` is the SDL window's native NSWindow, never a guest pointer.
DisplayRange QueryDisplayRange(void* window);

struct NativeHdrParams {
    float headroom{1.0f}, paper_white{1.0f}, peak{}, gamma{0.45454545f};
    float matrix[12]{1,0,0,0, 0,1,0,0, 0,0,1,0};
    u32 has_lut{}, has_ui{}, ui_linear{}, flags{}; // bit 0: BGR UI; bit 1: BGR LUT input
};
static_assert(sizeof(NativeHdrParams) == 80);

class HdrPasses {
public:
    explicit HdrPasses(Device& device);
    ~HdrPasses();
    void ConvertNative(Encoder& encoder, void* lighting, void* bloom, void* lut, void* ui,
                       void* output, const NativeHdrParams& params, void* ui_white = nullptr);
    /// Copy immutable linear HDR output, or decode the complete SDR fallback frame.
    void CopyFrame(Encoder& encoder, void* source, void* output, bool native);
    void ClearOverlay(void* command_buffer, void* texture);
    /// Present queue: letterboxed linear scene + a premultiplied sRGB overlay -> EDR layer.
    /// The overlay remains at SDR brightness; current headroom is applied again at present.
    void Present(void* command_buffer, void* frame, u32 width, u32 height, void* overlay,
                 void* target, u32 target_width, u32 target_height, float headroom);
    /// SDR preview of an EDR drawable, for the existing diagnostic PNG snapshot tool.
    Object Snapshot(Device& device, void* command_buffer, void* texture, u32 width, u32 height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Metal
