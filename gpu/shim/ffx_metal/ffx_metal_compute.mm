// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the FidelityFX Metal back end's pipelines, constant buffers and compute jobs
// (Metal-native M6). fpCreatePipeline maps every (space, binding) of a blob's reflection tables to
// the MSL slot the compiled shader gave it (CompiledShader::Find; no HostKernel, whose slot arrays
// stop at binding 7); the compute PSOs come from the per-device cache (PipelineInfoFor) and are
// never destroyed with a context. A compute job binds the textures (UAV mips > 0 through per-mip
// storage views), the constant buffers (setBytes) and FFX's two static samplers, then dispatches
// with the shader's own threadgroup size, on the Encoder's serial compute encoder.

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstring>
#include "common/logging/log.h"
#include "ffx_metal/ffx_metal_private.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace FfxMetal {

namespace {

using Kind = Metal::ShaderBinding::Kind;

// Fills `out` (at most `max` entries) from one of the blob's reflection tables; false when a
// binding is a static (bindless, space != 0) resource or the compiled shader does not have it.
bool MapBindings(const Metal::CompiledShader& shader, Kind kind, uint32_t count,
                 const char** names, const uint32_t* bindings, const uint32_t* counts,
                 const uint32_t* spaces, FfxResourceBinding* out, uint32_t max,
                 uint32_t& out_count, std::string& error) {
    out_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t space = spaces ? spaces[i] : 0;
        const char* name = names && names[i] ? names[i] : "?";
        const Metal::ShaderBinding* binding = space == 0 ? shader.Find(0, bindings[i], kind) : nullptr;
        if (!binding) {
            error = fmt::format("{} (space {}, binding {}) is not in the compiled shader", name,
                                space, bindings[i]);
            return false;
        }
        const uint32_t array_size = counts ? counts[i] : 1;
        for (uint32_t element = 0; element < array_size; ++element) {
            if (out_count >= max) {
                error = fmt::format("{}: more than {} bindings", name, max);
                return false;
            }
            FfxResourceBinding& entry = out[out_count++];
            entry = FfxResourceBinding{};
            entry.slotIndex = binding->msl_index + element;
            entry.arrayIndex = element;
            for (size_t c = 0; name[c] && c + 1 < FFX_RESOURCE_NAME_SIZE; ++c) {
                entry.name[c] = wchar_t(name[c]);
            }
        }
    }
    return true;
}

// FFX's static samplers (decision 5), once per back end.
void EnsureSamplers(BackendContext& backend, State& state) {
    if (state.samplers[kPointClamp]) {
        return;
    }
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
        const bool linear = i == kLinearClamp;
        Metal::SamplerDesc desc;
        desc.min_filter = desc.mag_filter =
            linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        desc.mip_filter = linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        desc.address_u = desc.address_v = desc.address_w = MTLSamplerAddressModeClampToEdge;
        desc.min_lod = 0.0f;
        desc.max_lod = FLT_MAX; // VK_LOD_CLAMP_NONE
        state.samplers[i] = Metal::CreateSampler(*backend.device, desc);
    }
}

FfxErrorCode CreatePipeline(FfxInterface* iface, FfxEffect effect, FfxPass pass,
                            uint32_t permutation, const FfxPipelineDescription* desc, FfxUInt32,
                            FfxPipelineState* out) {
    if (!iface || !desc || !out) {
        return FFX_ERROR_INVALID_POINTER;
    }
    BackendContext& backend = Backend(iface);
    const std::string label = "ffx " + Narrow(desc->name);
    auto fail = [&](const std::string& cause) {
        LOG_ERROR(Render, "FidelityFX pipeline {}: {}", label, cause);
        return FFX_ERROR_BACKEND_API_ERROR;
    };
    if (desc->indirectWorkload) {
        return fail("indirect dispatches are not supported (the back end has no buffers)");
    }
    // FFX's two static samplers, in this order (all the upscaler asks for).
    for (size_t i = 0; i < desc->samplerCount; ++i) {
        const FfxSamplerDescription& sampler = desc->samplers[i];
        const FfxFilterType filter =
            i == kPointClamp ? FFX_FILTER_TYPE_MINMAGMIP_POINT : FFX_FILTER_TYPE_MINMAGMIP_LINEAR;
        if (i >= kSamplerCount || sampler.filter != filter ||
            sampler.addressModeU != FFX_ADDRESS_MODE_CLAMP ||
            sampler.addressModeV != FFX_ADDRESS_MODE_CLAMP) {
            return fail(fmt::format("sampler {} is not s_PointClamp / s_LinearClamp", i));
        }
    }

    FfxShaderBlob blob{};
    if (iface->fpGetPermutationBlobByIndex(effect, pass, FFX_BIND_COMPUTE_SHADER_STAGE, permutation,
                                           &blob) != FFX_OK ||
        !blob.data) {
        return fail(fmt::format("no blob for pass {} permutation {:#x}", int(pass), permutation));
    }
    std::string error;
    const Pipeline* info = PipelineInfoFor(*backend.device, blob, &error, label);
    if (!info) {
        return FFX_ERROR_BACKEND_API_ERROR; // logged by PipelineInfoFor
    }
    const Metal::CompiledShader& shader = *info->shader;
    if (blob.srvBufferCount != 0 || blob.uavBufferCount != 0) {
        return fail("buffer bindings are not supported");
    }
    for (uint32_t i = 0; i < blob.samplerCount; ++i) {
        const uint32_t binding = blob.boundSamplers[i];
        if (binding < kSamplerBindingBase || binding >= kSamplerBindingBase + kSamplerCount ||
            info->sampler_slots[binding - kSamplerBindingBase] < 0) {
            return fail(fmt::format("sampler {} (binding {}) is not in the compiled shader",
                                    blob.boundSamplerNames ? blob.boundSamplerNames[i] : "?",
                                    binding));
        }
    }

    uint32_t srv_count = 0, uav_count = 0, cb_count = 0;
    if (!MapBindings(shader, Kind::Texture, blob.srvTextureCount, blob.boundSRVTextureNames,
                     blob.boundSRVTextures, blob.boundSRVTextureCounts, blob.boundSRVTextureSpaces,
                     out->srvTextureBindings, FFX_MAX_NUM_SRVS, srv_count, error) ||
        !MapBindings(shader, Kind::StorageTexture, blob.uavTextureCount,
                     blob.boundUAVTextureNames, blob.boundUAVTextures, blob.boundUAVTextureCounts,
                     blob.boundUAVTextureSpaces, out->uavTextureBindings, FFX_MAX_NUM_UAVS,
                     uav_count, error) ||
        !MapBindings(shader, Kind::Buffer, blob.cbvCount, blob.boundConstantBufferNames,
                     blob.boundConstantBuffers, blob.boundConstantBufferCounts,
                     blob.boundConstantBufferSpaces, out->constantBufferBindings,
                     FFX_MAX_NUM_CONST_BUFFERS, cb_count, error)) {
        return fail(error);
    }
    if (desc->samplerCount != 0) {
        EnsureSamplers(backend, StateOf(backend));
    }

    out->rootSignature = const_cast<Pipeline*>(info);
    out->cmdSignature = nullptr;
    out->pipeline = info->pipeline;
    out->srvTextureCount = srv_count;
    out->uavTextureCount = uav_count;
    out->srvBufferCount = 0;
    out->uavBufferCount = 0;
    out->constCount = cb_count;
    out->staticTextureSrvCount = out->staticBufferSrvCount = 0;
    out->staticTextureUavCount = out->staticBufferUavCount = 0;
    std::wcsncpy(out->name, desc->name, FFX_RESOURCE_NAME_SIZE - 1);
    out->name[FFX_RESOURCE_NAME_SIZE - 1] = 0;
    return FFX_OK;
}

// The PSO and its reflection belong to the per-device cache (decision 6): only the state goes.
FfxErrorCode DestroyPipeline(FfxInterface* iface, FfxPipelineState* pipeline, FfxUInt32) {
    if (!iface) {
        return FFX_ERROR_INVALID_POINTER;
    }
    if (pipeline) {
        pipeline->pipeline = nullptr;
        pipeline->rootSignature = nullptr;
        pipeline->cmdSignature = nullptr;
    }
    return FFX_OK;
}

FfxErrorCode StageConstantBufferData(FfxInterface* iface, void* data, FfxUInt32 size,
                                     FfxConstantBuffer* constant_buffer) {
    if (!iface || !data || !constant_buffer) {
        return FFX_ERROR_INVALID_POINTER;
    }
    constexpr uint32_t kRingSize = FFX_CONSTANT_BUFFER_RING_BUFFER_SIZE;
    const uint32_t aligned = (size + 255u) & ~255u;
    if (aligned > kRingSize) {
        return FFX_ERROR_INVALID_SIZE;
    }
    State& state = StateOf(Backend(iface));
    if (!state.constant_ring) {
        state.constant_ring = std::make_unique<uint8_t[]>(kRingSize);
    }
    if (state.constant_offset + aligned > kRingSize) {
        state.constant_offset = 0;
    }
    uint8_t* slot = state.constant_ring.get() + state.constant_offset;
    std::memcpy(slot, data, size);
    state.constant_offset += aligned;
    constant_buffer->data = reinterpret_cast<uint32_t*>(slot);
    constant_buffer->num32BitEntries = size / sizeof(uint32_t);
    return FFX_OK;
}

} // namespace

FfxErrorCode ExecuteComputeJob(FfxInterface& iface, Metal::Encoder& encoder,
                               const FfxComputeJobDescription& job) {
    const auto* info = static_cast<const Pipeline*>(job.pipeline.rootSignature);
    if (!info || !job.pipeline.pipeline) {
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    State& state = StateOf(Backend(&iface));
    // FFX touches no untracked (guest) buffers: only its own hazard-tracked textures.
    encoder.SetComputePipeline(job.pipeline.pipeline, Metal::Access::None);

    // Null resources bind nil (reads give zero): Encoder slots persist across dispatches, so
    // skipping them, as ffx_vk.cpp does with its per-job descriptor sets, would leave an earlier
    // job's texture bound.
    for (uint32_t i = 0; i < job.pipeline.uavTextureCount; ++i) {
        const FfxTextureUAV& uav = job.uavTextures[i];
        void* texture = nullptr;
        if (uav.resource.internalIndex != 0) {
            texture = BindingTexture(iface, uav.resource, uav.mip);
            if (!texture) {
                return FFX_ERROR_INVALID_ARGUMENT;
            }
        }
        encoder.SetTexture(Metal::Stage::Compute, job.pipeline.uavTextureBindings[i].slotIndex,
                           texture);
    }
    for (uint32_t i = 0; i < job.pipeline.srvTextureCount; ++i) {
        const FfxTextureSRV& srv = job.srvTextures[i];
        void* texture = nullptr;
        if (srv.resource.internalIndex != 0) {
            texture = BindingTexture(iface, srv.resource, 0);
            if (!texture) {
                return FFX_ERROR_INVALID_ARGUMENT;
            }
        }
        encoder.SetTexture(Metal::Stage::Compute, job.pipeline.srvTextureBindings[i].slotIndex,
                           texture);
    }
    // The MSL struct of a constant buffer is padded to its 16-byte alignment (cbFSR3Upscaler:
    // 148 bytes staged, 160 declared): the bytes go zero-padded.
    for (uint32_t i = 0; i < job.pipeline.constCount; ++i) {
        const FfxConstantBuffer& cb = job.cbs[i];
        const uint32_t size = cb.num32BitEntries * uint32_t(sizeof(uint32_t));
        std::array<uint8_t, FFX_BUFFER_SIZE> padded{};
        if (!cb.data || size == 0 || size > padded.size()) {
            return FFX_ERROR_INVALID_ARGUMENT;
        }
        std::memcpy(padded.data(), cb.data, size);
        encoder.SetBytes(Metal::Stage::Compute, job.pipeline.constantBufferBindings[i].slotIndex,
                         padded.data(), (size + 15u) & ~15u);
    }
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
        if (info->sampler_slots[i] >= 0) {
            if (!state.samplers[i]) {
                return FFX_ERROR_INVALID_ARGUMENT;
            }
            encoder.SetSampler(Metal::Stage::Compute, uint32_t(info->sampler_slots[i]),
                               state.samplers[i].Get());
        }
    }
    const auto& local = info->shader->local_size;
    encoder.Dispatch({job.dimensions[0], job.dimensions[1], job.dimensions[2]},
                     {std::max(local[0], 1u), std::max(local[1], 1u), std::max(local[2], 1u)});
    return FFX_OK;
}

void InstallComputeCallbacks(FfxInterface* iface) {
    iface->fpStageConstantBufferDataFunc = StageConstantBufferData;
    iface->fpCreatePipeline = CreatePipeline;
    iface->fpDestroyPipeline = DestroyPipeline;
}

} // namespace FfxMetal
