// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: one FSR 3.1 upscaler context on the FidelityFX Metal back end (ffx_metal.h), for the
// renderer's resolve step (vk_temporal_upscaler.cpp, ResolveFsr3). Owns the back end's scratch
// memory, AMD's context and the three shared resources the caller provides on Vulkan (dilated
// depth, dilated motion vectors, reconstructed previous depth). Plain C++: textures cross as
// void* (id<MTLTexture>, unretained).

#pragma once

#include <memory>
#include <string>

#include "common/types.h"

struct FfxInterface;

namespace Metal {
class Device;
class Encoder;
} // namespace Metal

namespace FfxMetal {

class Fsr3Metal {
public:
    /// The context's maximum render and output sizes, and whether the colour is linear HDR (FSR's
    /// HDR and auto-exposure flags) or the tonemapped frame (no flags). Depth is not inverted;
    /// motion vectors are at render resolution and unjittered.
    struct CreateInfo {
        u32 max_render_w, max_render_h, max_output_w, max_output_h;
        bool hdr;
    };
    /// One frame. Textures: id<MTLTexture>, alive until the GPU has run the frame. `color`,
    /// `depth` and `motion` hold the scene in their top-left render_w x render_h; `output`
    /// (output_w x output_h) has ShaderWrite usage. `reactive` may be null (no mask).
    struct DispatchInfo {
        void* color;
        void* depth;
        void* motion; ///< RG16F, pixels, previous minus current
        void* reactive; ///< R8, or null
        void* output;
        u32 render_w, render_h, output_w, output_h;
        float jitter_x, jitter_y; ///< pixels, the sign convention already applied
        float frame_time_ms, camera_near, camera_far, vertical_fov; ///< vertical_fov: radians
        float sharpness;          ///< RCAS strength, 0..1
        bool sharpen, reset;
    };

    Fsr3Metal();
    /// Destroy() first if the GPU may still run a dispatch of this context.
    ~Fsr3Metal();
    Fsr3Metal(const Fsr3Metal&) = delete;
    Fsr3Metal& operator=(const Fsr3Metal&) = delete;

    /// Makes the context (its pipelines come from the back end's per-device cache: a re-creation
    /// at another size compiles nothing) and the shared resources. A live context is destroyed
    /// first (the same idle-GPU contract as Destroy). False on failure, with the cause in `error`
    /// when given; nothing is left live then.
    bool Create(Metal::Device& device, const CreateInfo& info, std::string* error);
    /// Records one upscale into `encoder` (inside a Scheduler::Record closure: AMD's host code
    /// keeps per-frame state, so dispatches run in frame order on one thread). False when there
    /// is no context or FSR refuses the frame (logged once per context).
    bool Dispatch(Metal::Encoder& encoder, const DispatchInfo& info);
    /// Releases the context and its textures. The caller has run scheduler.Finish(): no recorded
    /// or running command may still use them.
    void Destroy();
    /// A live context made for `info`.
    [[nodiscard]] bool Matches(const CreateInfo& info) const;
    /// Bytes of the textures this context holds (FSR's own and the shared ones); 0 when none.
    [[nodiscard]] u64 MemoryBytes() const;

    /// Tests: called on the back end's interface right before each context creation (null: none),
    /// e.g. to make a callback fail.
    using InterfaceHook = void (*)(FfxInterface* iface);
    static void SetInterfaceHookForTests(InterfaceHook hook);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace FfxMetal
