// SPDX-License-Identifier: GPL-2.0-or-later
// Metal EDR presentation. Scene highlight recovery follows lKaniel/bloodborne-server
// at 8d5e54f (hdr_exposure.comp, hdr_gain.comp, hdr_highlight.glsl); see docs/HDR.md.
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

class HdrPasses {
public:
    explicit HdrPasses(Device& device);
    ~HdrPasses();
    /// Draw queue: SDR frame (gamma encoded) -> linear RGBA16Float. Missing captures give
    /// ordinary SDR luminance. Exposure is recomputed per frame, so cuts never reuse it.
    /// All textures must remain alive through GPU completion.
    void Convert(Encoder& encoder, void* frame, void* scene, void* hudless, void* output,
                 float headroom, float highlights);
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
