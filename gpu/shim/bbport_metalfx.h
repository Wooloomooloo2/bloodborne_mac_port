// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Apple MetalFX temporal upscaling for the Metal-native renderer. The scaler is a
// Metal::Object (id<MTLFXTemporalScaler>); textures cross as void* (id<MTLTexture>). Encode runs
// inside a Scheduler::Record closure and encodes into the frame's own command buffer, in order
// with the commands that wrote the inputs (no command buffer or submission of its own).
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
class Device;
class Encoder;
} // namespace Metal

namespace BbMetalFx {

/// Whether `device` supports the MetalFX temporal scaler.
bool Supported(Metal::Device& device);
bool SpatialSupported(Metal::Device& device);

/// What a scaler is made for: the MTLPixelFormat of each texture, the input (render) and output
/// sizes, and whether the colour is linear HDR (auto exposure) or the tonemapped frame.
struct Config {
    uint32_t color_format{}, depth_format{}, motion_format{}, output_format{};
    uint32_t width{}, height{}, out_width{}, out_height{};
    bool hdr{};
    bool spatial{};

    bool operator==(const Config&) const = default;
};

/// Makes a temporal scaler for `config` (tens of milliseconds; the first in a process longer).
/// Empty Object on failure (logged).
Metal::Object CreateScaler(Metal::Device& device, const Config& config);

/// One frame's inputs. The colour and depth may be larger than the scaler's input size, with the
/// scene in their top-left content_width x content_height (BB_RENDER_RES targets).
struct Frame {
    void* color{};  ///< id<MTLTexture>, the scaler's colour format
    void* depth{};  ///< id<MTLTexture>, the scaler's depth format
    void* motion{}; ///< id<MTLTexture>, pixels, previous minus current
    void* output{}; ///< id<MTLTexture>, output size, ShaderWrite and RenderTarget usage
    uint32_t content_width{}, content_height{}; ///< the scene rectangle (<= the input size)
    float jitter_x{}, jitter_y{};               ///< pixels, the signs already applied
    float motion_scale_x{1.0f}, motion_scale_y{1.0f};
    bool reset{}; ///< discard history (first frame, resize, camera cut, settings change)
};

/// Encodes one upscale into `e`'s command buffer (the open encoder ends first) and keeps the
/// scaler alive until that command buffer completes. Call inside a Scheduler::Record closure:
/// the scaler's per-frame properties are set here, on the thread that encodes.
void Encode(Metal::Encoder& e, void* scaler, const Frame& frame);
void EncodeSpatial(Metal::Encoder& e, void* scaler, const Frame& frame);

/// The jitter signs per axis: BB_METALFX_JITTER="sx,sy" (default 1,1; a malformed value is
/// ignored) times BB_METALFX_JITTER_SIGN (one factor for both axes, default 1). Null: unset.
inline std::array<float, 2> JitterSigns(const char* per_axis, const char* both) {
    std::array<float, 2> signs{1.0f, 1.0f};
    float x = 1.0f, y = 1.0f;
    if (per_axis && std::sscanf(per_axis, "%f,%f", &x, &y) == 2) {
        signs = {x, y};
    }
    float s = 1.0f;
    if (both && std::sscanf(both, "%f", &s) == 1) {
        signs = {signs[0] * s, signs[1] * s};
    }
    return signs;
}

} // namespace BbMetalFx
