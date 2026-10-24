// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the FidelityFX Metal back end's state (shim/ffx_metal/ffx_metal.h), shared by its
// source files. Plain C++: Metal objects cross as void*.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "ffx_metal/ffx_metal.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
struct CompiledShader;
class Encoder;
}

namespace FfxMetal {

struct State;

/// One effect context (an FSR 3 upscaler context is one).
struct EffectContext {
    bool active{};
    FfxEffect effect{};
};

/// The back end's state, at the start of the caller's scratch memory (ffx_vk.cpp's
/// BackendContext_VK); the effect contexts follow it.
struct BackendContext {
    Metal::Device* device{};
    State* state{};      ///< resources and queued jobs (ffx_metal_resources.mm); null while
                                ///< no context is live
    uint32_t max_contexts{};
    uint32_t ref_count{};       ///< live effect contexts
    EffectContext* contexts{};  ///< max_contexts entries, in the scratch memory
};

inline BackendContext& Backend(FfxInterface* iface) {
    return *static_cast<BackendContext*>(iface->scratchBuffer);
}

/// FFX's static samplers: bindings 1000 + index (s_PointClamp, s_LinearClamp) in set 0.
enum SamplerIndex : uint32_t { kPointClamp, kLinearClamp, kSamplerCount };
constexpr uint32_t kSamplerBindingBase = 1000;

/// A compiled FFX blob: the compute pipeline and its shader's reflection (MSL slots of the
/// blob's (set, binding) pairs, threads per threadgroup). Owned by the per-device cache.
struct Pipeline {
    void* pipeline{};                       ///< id<MTLComputePipelineState>, unretained
    const Metal::CompiledShader* shader{};
    /// MSL sampler slot of each SamplerIndex, -1 when the shader does not use it.
    int32_t sampler_slots[kSamplerCount]{-1, -1};
};

/// PipelineFor with the reflection; null on failure (cause in `error`).
const Pipeline* PipelineInfoFor(Metal::Device& device, const FfxShaderBlob& blob,
                                std::string* error, std::string_view name);

/// One resource slot: a texture the back end created (owned) or one the caller registered for a
/// dispatch (dynamic, unowned).
struct Slot {
    bool used{};
    bool dynamic{};      ///< registered per dispatch (fpRegisterResource), dropped by Unregister
    uint32_t context{};  ///< effect context that created or registered it
    FfxResourceDescription description{};
    FfxResourceStates state{};
    Metal::Object texture;      ///< owned textures (id<MTLTexture>)
    void* handle{};             ///< id<MTLTexture>, unretained (owned or the caller's)
    uint64_t bytes{};           ///< allocated size of an owned texture
    std::vector<Metal::Object> mip_views; ///< per-mip storage views, made on first use
    Metal::Object staging;      ///< init data (id<MTLBuffer>) until the first ExecuteGpuJobs
    std::string name;
};

/// The back end's resource table (slot 0 is the null resource) and the jobs scheduled since the
/// last fpExecuteGpuJobs. Behind BackendContext::state; the scratch memory holds only a pointer so
/// that the C++ containers are released with the last effect context.
struct State {
    std::vector<Slot> resources;
    std::vector<uint32_t> free_slots;
    std::vector<uint32_t> pending_uploads; ///< owned resources with `staging`
    std::vector<FfxGpuJobDescription> jobs;
    /// Staged constant buffers (fpStageConstantBufferDataFunc): read by the compute jobs of the
    /// same fpExecuteGpuJobs (Encoder::SetBytes copies them), so a ring as in ffx_vk.cpp.
    std::unique_ptr<uint8_t[]> constant_ring;
    uint32_t constant_offset{};
    Metal::Object samplers[kSamplerCount]; ///< id<MTLSamplerState>, made with the first pipeline
    State() : resources(1) {}
};

/// The state of `backend`, made on first use.
State& StateOf(BackendContext& backend);

/// Sets the resource and job callbacks (ffx_metal_resources.mm) on `iface`.
void InstallResourceCallbacks(FfxInterface* iface);

/// Sets the pipeline and constant buffer callbacks (ffx_metal_compute.mm) on `iface`.
void InstallComputeCallbacks(FfxInterface* iface);

/// Encodes one FFX_GPU_JOB_COMPUTE job (ffx_metal_compute.mm).
FfxErrorCode ExecuteComputeJob(FfxInterface& iface, Metal::Encoder& encoder,
                               const FfxComputeJobDescription& job);

/// An FFX (UTF-16/32) name as ASCII, for labels and logs.
inline std::string Narrow(const wchar_t* name) {
    std::string out;
    for (; name && *name; ++name) {
        out.push_back(*name < 0x80 ? char(*name) : '?');
    }
    return out;
}

/// Releases the resources of effect context `context` (all of them when the last context goes:
/// the state is deleted).
void ReleaseContext(BackendContext& backend, uint32_t context);

/// The id<MTLTexture> view of one mip of resource `resource` for UAV bindings (arrayIndex =
/// mip), made on first use and kept with the resource; null for a null resource or a mip past
/// the texture's.
void* StorageView(FfxInterface& iface, FfxResourceInternal resource, uint32_t mip);

/// What a compute job binds for `resource` at `mip` (clamped to its last mip, as ffx_vk.cpp
/// does): the texture itself for mip 0 (SRVs, and UAV writes land on level 0), else its
/// StorageView. Null for the null resource.
void* BindingTexture(FfxInterface& iface, FfxResourceInternal resource, uint32_t mip);

} // namespace FfxMetal
