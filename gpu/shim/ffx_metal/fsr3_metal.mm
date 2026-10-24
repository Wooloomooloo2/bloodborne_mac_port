// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: one FSR 3.1 upscaler context on the FidelityFX Metal back end (fsr3_metal.h): what
// ffx-vulkan's portable wrapper did for the Vulkan renderer (context creation, the shared
// resources, the dispatch description), on Metal textures.

#include "ffx_metal/fsr3_metal.h"

#include <algorithm>
#include <cstdio>
#include <vector>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include "ffx_metal/ffx_metal.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace FfxMetal {

namespace {

// The shared resources, in FfxFsr3UpscalerSharedResourceDescriptions order.
enum SharedIndex : u32 { kDilatedDepth, kDilatedMotion, kPrevNearestDepth, kSharedCount };

Fsr3Metal::InterfaceHook interface_hook = nullptr;

} // namespace

struct Fsr3Metal::Impl {
    std::vector<uint8_t> scratch; ///< the back end's state (FfxMetal::GetInterface)
    FfxFsr3UpscalerContext context{};
    CreateInfo info{};
    Metal::Object shared[kSharedCount]; ///< id<MTLTexture>
    u64 bytes{};                        ///< FSR's own textures and the shared ones
    bool dispatch_failed_logged{};
};

Fsr3Metal::Fsr3Metal() = default;

Fsr3Metal::~Fsr3Metal() {
    Destroy();
}

bool Fsr3Metal::Create(Metal::Device& device, const CreateInfo& info, std::string* error) {
    Destroy();
    auto fail = [&](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return false;
    };
    auto state = std::make_unique<Impl>();
    state->info = info;
    state->scratch.resize(ScratchSize(1));
    FfxFsr3UpscalerContextDescription description{};
    // Native AA resolves the HDR scene colour; the scaled presets the tonemapped frame.
    description.flags = info.hdr ? FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
                                       FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE
                                 : 0u;
    description.maxRenderSize = {info.max_render_w, info.max_render_h};
    description.maxUpscaleSize = {info.max_output_w, info.max_output_h};
    if (const FfxErrorCode code =
            GetInterface(&description.backendInterface, device, state->scratch.data(),
                         state->scratch.size(), 1);
        code != FFX_OK) {
        return fail("FfxMetal::GetInterface failed (" + std::to_string(code) + ")");
    }
    if (interface_hook) {
        interface_hook(&description.backendInterface);
    }
    const u64 before = LiveTextureBytes();
    if (const FfxErrorCode code = ffxFsr3UpscalerContextCreate(&state->context, &description);
        code != FFX_OK) {
        // AMD's create returns without cleanup: its back-end context and the textures made
        // before the failing step (pipelines come last) are still in the scratch memory.
        ReleaseInterface(&description.backendInterface);
        char text[96];
        std::snprintf(text, sizeof(text), "ffxFsr3UpscalerContextCreate failed (0x%x)",
                      unsigned(code));
        return fail(text);
    }
    state->bytes = LiveTextureBytes() - before;

    // The resources FSR shares with frame generation, which the caller owns.
    FfxFsr3UpscalerSharedResourceDescriptions shared{};
    if (ffxFsr3UpscalerGetSharedResourceDescriptions(&state->context, &shared) != FFX_OK) {
        ffxFsr3UpscalerContextDestroy(&state->context);
        return fail("ffxFsr3UpscalerGetSharedResourceDescriptions failed");
    }
    const FfxCreateResourceDescription* descriptions[kSharedCount] = {
        &shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth};
    constexpr const char* kLabels[kSharedCount] = {"FSR3 dilated depth", "FSR3 dilated motion",
                                                   "FSR3 previous nearest depth"};
    for (u32 i = 0; i < kSharedCount; ++i) {
        const FfxResourceDescription& r = descriptions[i]->resourceDescription;
        u64 bytes = 0;
        // Cleared by render passes and written by FSR's passes; the R32_UINT depth with atomics.
        state->shared[i] = Metal::CreateTexture(device,
                                                Metal::TextureDesc{
                                                    .pixel_format = PixelFormat(r.format),
                                                    .width = r.width,
                                                    .height = r.height,
                                                    .render_target = true,
                                                    .shader_write = true,
                                                    .shader_atomic =
                                                        r.format == FFX_SURFACE_FORMAT_R32_UINT,
                                                },
                                                kLabels[i], &bytes);
        if (!state->shared[i]) {
            ffxFsr3UpscalerContextDestroy(&state->context);
            return fail(std::string("no texture for ") + kLabels[i]);
        }
        state->bytes += bytes;
    }
    impl = std::move(state);
    return true;
}

bool Fsr3Metal::Dispatch(Metal::Encoder& encoder, const DispatchInfo& info) {
    if (!impl) {
        return false;
    }
    FfxFsr3UpscalerDispatchDescription d{};
    d.commandList = CommandList(encoder);
    d.color = Resource(info.color, FFX_RESOURCE_USAGE_READ_ONLY, L"FSR3 color");
    d.depth = Resource(info.depth, FFX_RESOURCE_USAGE_READ_ONLY, L"FSR3 depth");
    d.motionVectors = Resource(info.motion, FFX_RESOURCE_USAGE_READ_ONLY, L"FSR3 motion");
    // Optional inputs: a null resource (FSR substitutes its defaults). No exposure texture (auto
    // exposure with HDR input, 1.0 otherwise) and no transparency-and-composition mask.
    d.reactive = Resource(info.reactive, FFX_RESOURCE_USAGE_READ_ONLY, L"FSR3 reactive");
    d.dilatedDepth = Resource(impl->shared[kDilatedDepth].Get(), FFX_RESOURCE_USAGE_UAV,
                              L"FSR3 dilated depth");
    d.dilatedMotionVectors = Resource(impl->shared[kDilatedMotion].Get(), FFX_RESOURCE_USAGE_UAV,
                                      L"FSR3 dilated motion");
    d.reconstructedPrevNearestDepth = Resource(impl->shared[kPrevNearestDepth].Get(),
                                               FFX_RESOURCE_USAGE_UAV,
                                               L"FSR3 previous nearest depth");
    d.output = Resource(info.output, FFX_RESOURCE_USAGE_UAV, L"FSR3 output");
    d.jitterOffset = {info.jitter_x, info.jitter_y};
    d.motionVectorScale = {1.0f, 1.0f}; // render pixels
    d.renderSize = {info.render_w, info.render_h};
    d.upscaleSize = {info.output_w, info.output_h};
    d.enableSharpening = info.sharpen;
    d.sharpness = std::clamp(info.sharpness, 0.0f, 1.0f);
    d.frameTimeDelta = info.frame_time_ms;
    d.preExposure = 1.0f;
    d.reset = info.reset;
    d.cameraNear = info.camera_near;
    d.cameraFar = info.camera_far;
    d.cameraFovAngleVertical = info.vertical_fov;
    d.viewSpaceToMetersFactor = 1.0f;
    if (const FfxErrorCode code = ffxFsr3UpscalerContextDispatch(&impl->context, &d);
        code != FFX_OK) {
        if (!impl->dispatch_failed_logged) {
            impl->dispatch_failed_logged = true;
            std::printf("FSR 3: ffxFsr3UpscalerContextDispatch failed (0x%x) at %ux%u -> %ux%u\n",
                        unsigned(code), info.render_w, info.render_h, info.output_w,
                        info.output_h);
            std::fflush(stdout);
        }
        return false;
    }
    return true;
}

void Fsr3Metal::Destroy() {
    if (!impl) {
        return;
    }
    ffxFsr3UpscalerContextDestroy(&impl->context);
    impl.reset(); // the shared textures, then the back end's scratch memory
}

bool Fsr3Metal::Matches(const CreateInfo& info) const {
    const CreateInfo* made = impl ? &impl->info : nullptr;
    return made && made->max_render_w == info.max_render_w &&
           made->max_render_h == info.max_render_h && made->max_output_w == info.max_output_w &&
           made->max_output_h == info.max_output_h && made->hdr == info.hdr;
}

u64 Fsr3Metal::MemoryBytes() const {
    return impl ? impl->bytes : 0;
}

void Fsr3Metal::SetInterfaceHookForTests(InterfaceHook hook) {
    interface_hook = hook;
}

} // namespace FfxMetal
