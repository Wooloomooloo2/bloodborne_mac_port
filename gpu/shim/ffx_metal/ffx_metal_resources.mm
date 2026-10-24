// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the FidelityFX Metal back end's resources, clears, copies and job queue (Metal-native
// M6). Mirrors ffx_vk.cpp's split: effect-owned resources are created once (fpCreateResource),
// the caller's textures are registered per dispatch and dropped by fpUnregisterResources. Every
// texture is a private MTLTexture with render target and shader write usage (clears are render
// passes, UAVs are storage views); resource states are ignored (barriers are no-ops, decision 3 of
// the plan). Compute jobs are encoded by ffx_metal_compute.mm.

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include "common/logging/log.h"
#include "ffx_metal/ffx_metal_private.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace FfxMetal {

namespace {

std::atomic<uint64_t> live_texture_bytes{0};

struct FormatPair {
    FfxSurfaceFormat ffx;
    MTLPixelFormat metal;
};

// The formats FSR 3 and the shared FFX resource lists use; typeless and 3-channel formats have
// no Metal twin.
constexpr FormatPair kFormats[] = {
    {FFX_SURFACE_FORMAT_R32G32B32A32_UINT, MTLPixelFormatRGBA32Uint},
    {FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT, MTLPixelFormatRGBA32Float},
    {FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, MTLPixelFormatRGBA16Float},
    {FFX_SURFACE_FORMAT_R32G32_FLOAT, MTLPixelFormatRG32Float},
    {FFX_SURFACE_FORMAT_R8_UINT, MTLPixelFormatR8Uint},
    {FFX_SURFACE_FORMAT_R32_UINT, MTLPixelFormatR32Uint},
    {FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, MTLPixelFormatRGBA8Unorm},
    {FFX_SURFACE_FORMAT_R8G8B8A8_SNORM, MTLPixelFormatRGBA8Snorm},
    {FFX_SURFACE_FORMAT_R8G8B8A8_SRGB, MTLPixelFormatRGBA8Unorm_sRGB},
    {FFX_SURFACE_FORMAT_B8G8R8A8_UNORM, MTLPixelFormatBGRA8Unorm},
    {FFX_SURFACE_FORMAT_B8G8R8A8_SRGB, MTLPixelFormatBGRA8Unorm_sRGB},
    {FFX_SURFACE_FORMAT_R11G11B10_FLOAT, MTLPixelFormatRG11B10Float},
    {FFX_SURFACE_FORMAT_R10G10B10A2_UNORM, MTLPixelFormatRGB10A2Unorm},
    {FFX_SURFACE_FORMAT_R16G16_FLOAT, MTLPixelFormatRG16Float},
    {FFX_SURFACE_FORMAT_R16G16_UINT, MTLPixelFormatRG16Uint},
    {FFX_SURFACE_FORMAT_R16G16_SINT, MTLPixelFormatRG16Sint},
    {FFX_SURFACE_FORMAT_R16_FLOAT, MTLPixelFormatR16Float},
    {FFX_SURFACE_FORMAT_R16_UINT, MTLPixelFormatR16Uint},
    {FFX_SURFACE_FORMAT_R16_UNORM, MTLPixelFormatR16Unorm},
    {FFX_SURFACE_FORMAT_R16_SNORM, MTLPixelFormatR16Snorm},
    {FFX_SURFACE_FORMAT_R8_UNORM, MTLPixelFormatR8Unorm},
    {FFX_SURFACE_FORMAT_R8G8_UNORM, MTLPixelFormatRG8Unorm},
    {FFX_SURFACE_FORMAT_R8G8_UINT, MTLPixelFormatRG8Uint},
    {FFX_SURFACE_FORMAT_R32_FLOAT, MTLPixelFormatR32Float},
    {FFX_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP, MTLPixelFormatRGB9E5Float},
};

// The integer formats of kFormats.
bool IsIntegerFormat(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatR8Uint:
    case MTLPixelFormatR32Uint:
    case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRG16Sint:
    case MTLPixelFormatR16Uint:
    case MTLPixelFormatRG8Uint:
        return true;
    default:
        return false;
    }
}

FfxSurfaceFormat SurfaceFormat(uint32_t metal_format) {
    for (const FormatPair& pair : kFormats) {
        if (uint32_t(pair.metal) == metal_format) {
            return pair.ffx;
        }
    }
    return FFX_SURFACE_FORMAT_UNKNOWN;
}

uint32_t MipCount(const FfxResourceDescription& description) {
    const uint32_t full = std::bit_width(std::max(description.width, description.height));
    return description.mipCount == 0 ? full : std::min(description.mipCount, full);
}

uint32_t MipSize(uint32_t size, uint32_t mip) {
    return std::max(size >> mip, 1u);
}

// A slot index for a new resource.
uint32_t AllocateSlot(State& state) {
    if (!state.free_slots.empty()) {
        const uint32_t slot = state.free_slots.back();
        state.free_slots.pop_back();
        return slot;
    }
    state.resources.emplace_back();
    return uint32_t(state.resources.size() - 1);
}

Slot* Find(State& state, FfxResourceInternal resource) {
    if (resource.internalIndex <= 0 || size_t(resource.internalIndex) >= state.resources.size()) {
        return nullptr;
    }
    Slot& slot = state.resources[resource.internalIndex];
    return slot.used ? &slot : nullptr;
}

// The encoder's command buffers do not retain what they use (unretained references): with
// `encoder`, the slot's views and texture live until its command buffer completes. Without
// (fpDestroyResource, fpDestroyBackendContext), the caller guarantees an idle GPU.
void FreeSlot(State& state, uint32_t index, Metal::Encoder* encoder = nullptr) {
    Slot& slot = state.resources[index];
    if (slot.texture) {
        live_texture_bytes.fetch_sub(slot.bytes, std::memory_order_relaxed);
    }
    if (encoder) {
        for (Metal::Object& view : slot.mip_views) {
            if (view) {
                encoder->KeepAlive(std::move(view));
            }
        }
        if (slot.texture) {
            encoder->KeepAlive(std::move(slot.texture));
        }
    }
    std::erase(state.pending_uploads, index);
    slot = Slot{};
    state.free_slots.push_back(index);
}

FfxErrorCode CreateResource(FfxInterface* iface, const FfxCreateResourceDescription* desc,
                            FfxUInt32 context, FfxResourceInternal* out) {
    if (!iface || !desc || !out) {
        return FFX_ERROR_INVALID_POINTER;
    }
    BackendContext& backend = Backend(iface);
    const FfxResourceDescription& rd = desc->resourceDescription;
    if (desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_INVALID ||
        desc->heapType != FFX_HEAP_TYPE_DEFAULT || rd.type != FFX_RESOURCE_TYPE_TEXTURE2D ||
        rd.width == 0 || rd.height == 0) {
        LOG_ERROR(Render, "FidelityFX: unsupported resource {}", Narrow(desc->name));
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t pixel_format = PixelFormat(rd.format);
    if (pixel_format == uint32_t(MTLPixelFormatInvalid)) {
        LOG_ERROR(Render, "FidelityFX: resource {} has no Metal format ({})", Narrow(desc->name),
                  int(rd.format));
        return FFX_ERROR_INVALID_ARGUMENT;
    }

    FfxResourceDescription description = rd;
    description.mipCount = MipCount(rd);
    // Decision 4: every texture takes clears (render target) and UAV writes (shader write).
    // Atomics (FSR's SPD counter) need their own usage bit, on 32-bit integer formats.
    Metal::TextureDesc texture_desc;
    texture_desc.pixel_format = pixel_format;
    texture_desc.width = rd.width;
    texture_desc.height = rd.height;
    texture_desc.levels = description.mipCount;
    texture_desc.render_target = true;
    texture_desc.shader_write = true;
    texture_desc.shader_atomic = rd.format == FFX_SURFACE_FORMAT_R32_UINT &&
                                 (rd.usage & FFX_RESOURCE_USAGE_UAV) != 0;
    const std::string name = Narrow(desc->name);
    uint64_t bytes = 0;
    Metal::Object texture = Metal::CreateTexture(*backend.device, texture_desc, name, &bytes);
    if (!texture) {
        LOG_ERROR(Render, "FidelityFX: no texture for resource {} ({}x{})", name, rd.width,
                  rd.height);
        return FFX_ERROR_OUT_OF_MEMORY;
    }

    Metal::Object staging;
    const uint32_t row_bytes = rd.width * Metal::BytesPerBlock(pixel_format);
    const size_t image_bytes = size_t(row_bytes) * rd.height;
    if (desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER ||
        desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE) {
        if (row_bytes == 0 ||
            (desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER && !desc->initData.buffer)) {
            return FFX_ERROR_INVALID_ARGUMENT;
        }
        // A shared buffer of the mip 0 image, copied in at the first ExecuteGpuJobs.
        id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)backend.device->NativeDevice();
        id<MTLBuffer> buffer = [mtl_device newBufferWithLength:image_bytes
                                                       options:MTLResourceStorageModeShared];
        if (!buffer) {
            return FFX_ERROR_OUT_OF_MEMORY;
        }
        const size_t size = std::min(desc->initData.size, image_bytes);
        if (desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER) {
            std::memset(buffer.contents, 0, image_bytes);
            std::memcpy(buffer.contents, desc->initData.buffer, size);
        } else {
            std::memset(buffer.contents, desc->initData.value, image_bytes);
        }
        staging = Metal::Object::Adopt((__bridge_retained void*)buffer);
    }

    State& state = StateOf(backend);
    const uint32_t index = AllocateSlot(state);
    Slot& slot = state.resources[index];
    slot.used = true;
    slot.context = context;
    slot.description = description;
    slot.state = desc->initialState;
    slot.handle = texture.Get();
    slot.texture = std::move(texture);
    slot.bytes = bytes;
    slot.mip_views.resize(description.mipCount);
    slot.name = name;
    if (staging) {
        slot.staging = std::move(staging);
        state.pending_uploads.push_back(index);
    }
    live_texture_bytes.fetch_add(bytes, std::memory_order_relaxed);
    out->internalIndex = int32_t(index);
    return FFX_OK;
}

FfxErrorCode RegisterResource(FfxInterface* iface, const FfxResource* in, FfxUInt32 context,
                              FfxResourceInternal* out) {
    if (!iface || !in || !out) {
        return FFX_ERROR_INVALID_POINTER;
    }
    if (!in->resource) {
        out->internalIndex = 0; // the null resource
        return FFX_OK;
    }
    if (in->description.type != FFX_RESOURCE_TYPE_TEXTURE2D) {
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    State& state = StateOf(Backend(iface));
    const uint32_t index = AllocateSlot(state);
    Slot& slot = state.resources[index];
    slot.used = true;
    slot.dynamic = true;
    slot.context = context;
    slot.description = in->description;
    slot.description.mipCount = MipCount(in->description);
    slot.state = in->state;
    slot.handle = in->resource;
    slot.mip_views.resize(slot.description.mipCount);
    slot.name = Narrow(in->name);
    out->internalIndex = int32_t(index);
    return FFX_OK;
}

FfxErrorCode UnregisterResources(FfxInterface* iface, FfxCommandList command_list,
                                 FfxUInt32 context) {
    if (!iface) {
        return FFX_ERROR_INVALID_POINTER;
    }
    State& state = StateOf(Backend(iface));
    auto* encoder = reinterpret_cast<Metal::Encoder*>(command_list);
    for (uint32_t i = 1; i < state.resources.size(); ++i) {
        if (state.resources[i].used && state.resources[i].dynamic &&
            state.resources[i].context == context) {
            FreeSlot(state, i, encoder);
        }
    }
    return FFX_OK;
}

FfxResource GetResource(FfxInterface* iface, FfxResourceInternal resource) {
    FfxResource out{};
    if (const Slot* slot = Find(StateOf(Backend(iface)), resource)) {
        out.resource = slot->handle;
        out.description = slot->description;
        out.state = slot->state;
        for (size_t i = 0; i + 1 < FFX_RESOURCE_NAME_SIZE && i < slot->name.size(); ++i) {
            out.name[i] = wchar_t(slot->name[i]);
        }
    }
    return out;
}

FfxResourceDescription GetResourceDescription(FfxInterface* iface, FfxResourceInternal resource) {
    const Slot* slot = Find(StateOf(Backend(iface)), resource);
    return slot ? slot->description : FfxResourceDescription{};
}

FfxErrorCode DestroyResource(FfxInterface* iface, FfxResourceInternal resource, FfxUInt32) {
    if (!iface) {
        return FFX_ERROR_INVALID_POINTER;
    }
    State& state = StateOf(Backend(iface));
    if (!Find(state, resource)) {
        return resource.internalIndex == 0 ? FFX_OK : FFX_ERROR_INVALID_ARGUMENT;
    }
    FreeSlot(state, uint32_t(resource.internalIndex));
    return FFX_OK;
}

// The upscaler uses neither static (bindless) resources nor mapped ones.
FfxErrorCode RegisterStaticResource(FfxInterface*, const FfxStaticResourceDescription*,
                                    FfxUInt32) {
    return FFX_ERROR_BACKEND_API_ERROR;
}
FfxErrorCode MapResource(FfxInterface*, FfxResourceInternal, void**) {
    return FFX_ERROR_BACKEND_API_ERROR;
}
FfxErrorCode UnmapResource(FfxInterface*, FfxResourceInternal) {
    return FFX_ERROR_BACKEND_API_ERROR;
}

FfxErrorCode ScheduleGpuJob(FfxInterface* iface, const FfxGpuJobDescription* job) {
    if (!iface || !job) {
        return FFX_ERROR_INVALID_POINTER;
    }
    switch (job->jobType) {
    case FFX_GPU_JOB_CLEAR_FLOAT:
    case FFX_GPU_JOB_COPY:
    case FFX_GPU_JOB_BARRIER:
    case FFX_GPU_JOB_DISCARD:
    case FFX_GPU_JOB_COMPUTE:
        StateOf(Backend(iface)).jobs.push_back(*job);
        return FFX_OK;
    default:
        return FFX_ERROR_BACKEND_API_ERROR;
    }
}

// The jobs cover whole resources (the SDK has no mip or region selector for them): a clear
// fills every mip, a copy copies every mip both resources share.
// An integer target takes the bits of the float colour, as ffx_vk.cpp's VkClearColorValue union
// does: FSR clears its reconstructed depth (R32_UINT, asuint(depth)) with 1.0f, i.e. 0x3f800000.
void ClearTexture(Metal::Encoder& encoder, const Slot& target, const float color[4]) {
    std::array<double, 4> value{color[0], color[1], color[2], color[3]};
    const MTLPixelFormat format = MTLPixelFormat(PixelFormat(target.description.format));
    if (IsIntegerFormat(format)) {
        const bool is_signed = format == MTLPixelFormatRG16Sint;
        for (int i = 0; i < 4; ++i) {
            value[i] = is_signed ? double(std::bit_cast<int32_t>(color[i]))
                                 : double(std::bit_cast<uint32_t>(color[i]));
        }
    }
    for (uint32_t mip = 0; mip < target.description.mipCount; ++mip) {
        Metal::RenderPassDesc pass;
        Metal::ColorAttachment& attachment = pass.colors[0];
        attachment.texture = target.handle;
        attachment.level = mip;
        attachment.clear = true;
        attachment.clear_color = {value[0], value[1], value[2], value[3]};
        encoder.BeginRenderPass(pass);
        encoder.EndRenderPass();
    }
}

void CopyTexture(Metal::Encoder& encoder, const Slot& src, const Slot& dst) {
    const uint32_t mips = std::min(src.description.mipCount, dst.description.mipCount);
    for (uint32_t mip = 0; mip < mips; ++mip) {
        const Metal::Size3 size{std::min(MipSize(src.description.width, mip),
                                         MipSize(dst.description.width, mip)),
                                std::min(MipSize(src.description.height, mip),
                                         MipSize(dst.description.height, mip)),
                                1};
        encoder.CopyTexture(src.handle, mip, 0, {}, dst.handle, mip, 0, {}, size);
    }
}

void UploadInitData(Metal::Encoder& encoder, State& state) {
    for (const uint32_t index : state.pending_uploads) {
        Slot& slot = state.resources[index];
        const uint32_t row_bytes =
            slot.description.width *
            Metal::BytesPerBlock(PixelFormat(slot.description.format));
        encoder.CopyBufferToTexture({slot.staging.Get(), 0}, row_bytes,
                                    row_bytes * slot.description.height, slot.handle, 0, 0, {},
                                    {slot.description.width, slot.description.height, 1});
        encoder.KeepAlive(std::move(slot.staging)); // until the command buffer completes
    }
    state.pending_uploads.clear();
}

FfxErrorCode ExecuteGpuJobs(FfxInterface* iface, FfxCommandList command_list, FfxUInt32) {
    if (!iface || !command_list) {
        return FFX_ERROR_INVALID_POINTER;
    }
    State& state = StateOf(Backend(iface));
    auto& encoder = *reinterpret_cast<Metal::Encoder*>(command_list);
    UploadInitData(encoder, state);

    FfxErrorCode result = FFX_OK;
    // A copy of the list: compute jobs reach back into the interface.
    std::vector<FfxGpuJobDescription> jobs = std::move(state.jobs);
    state.jobs.clear();
    for (const FfxGpuJobDescription& job : jobs) {
        switch (job.jobType) {
        case FFX_GPU_JOB_CLEAR_FLOAT: {
            const Slot* target = Find(state, job.clearJobDescriptor.target);
            if (!target) {
                result = FFX_ERROR_INVALID_ARGUMENT;
                break;
            }
            ClearTexture(encoder, *target, job.clearJobDescriptor.color);
            break;
        }
        case FFX_GPU_JOB_COPY: {
            const Slot* src = Find(state, job.copyJobDescriptor.src);
            const Slot* dst = Find(state, job.copyJobDescriptor.dst);
            if (!src || !dst) {
                result = FFX_ERROR_INVALID_ARGUMENT;
                break;
            }
            CopyTexture(encoder, *src, *dst);
            break;
        }
        case FFX_GPU_JOB_COMPUTE:
            if (const FfxErrorCode code = ExecuteComputeJob(*iface, encoder, job.computeJobDescriptor);
                code != FFX_OK) {
                result = code;
            }
            break;
        case FFX_GPU_JOB_BARRIER: // hazard tracking orders the jobs (decision 3)
        case FFX_GPU_JOB_DISCARD:
            break;
        default:
            result = FFX_ERROR_BACKEND_API_ERROR;
            break;
        }
    }
    return result;
}

} // namespace

uint32_t PixelFormat(FfxSurfaceFormat format) {
    for (const FormatPair& pair : kFormats) {
        if (pair.ffx == format) {
            return uint32_t(pair.metal);
        }
    }
    return uint32_t(MTLPixelFormatInvalid);
}

FfxResource Resource(void* texture, FfxResourceUsage usage, const wchar_t* name) {
    FfxResource out{};
    if (!texture) {
        return out;
    }
    id<MTLTexture> mtl_texture = (__bridge id<MTLTexture>)texture;
    out.resource = texture;
    out.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    out.description.format = SurfaceFormat(uint32_t(mtl_texture.pixelFormat));
    out.description.width = uint32_t(mtl_texture.width);
    out.description.height = uint32_t(mtl_texture.height);
    out.description.depth = 1;
    out.description.mipCount = uint32_t(mtl_texture.mipmapLevelCount);
    out.description.flags = FFX_RESOURCE_FLAGS_NONE;
    out.description.usage = usage;
    out.state = FFX_RESOURCE_STATE_COMPUTE_READ;
    for (size_t i = 0; name && name[i] && i + 1 < FFX_RESOURCE_NAME_SIZE; ++i) {
        out.name[i] = name[i];
    }
    return out;
}

uint64_t LiveTextureBytes() {
    return live_texture_bytes.load(std::memory_order_relaxed);
}

State& StateOf(BackendContext& backend) {
    if (!backend.state) {
        backend.state = new State;
    }
    return *backend.state;
}

void InstallResourceCallbacks(FfxInterface* iface) {
    iface->fpCreateResource = CreateResource;
    iface->fpRegisterResource = RegisterResource;
    iface->fpGetResource = GetResource;
    iface->fpUnregisterResources = UnregisterResources;
    iface->fpRegisterStaticResource = RegisterStaticResource;
    iface->fpGetResourceDescription = GetResourceDescription;
    iface->fpDestroyResource = DestroyResource;
    iface->fpMapResource = MapResource;
    iface->fpUnmapResource = UnmapResource;
    iface->fpScheduleGpuJob = ScheduleGpuJob;
    iface->fpExecuteGpuJobs = ExecuteGpuJobs;
}

void ReleaseContext(BackendContext& backend, uint32_t context) {
    if (!backend.state) {
        return;
    }
    State& state = *backend.state;
    for (uint32_t i = 1; i < state.resources.size(); ++i) {
        if (state.resources[i].used && state.resources[i].context == context) {
            FreeSlot(state, i);
        }
    }
    if (backend.ref_count <= 1) { // the last effect context: the containers go too
        delete backend.state;
        backend.state = nullptr;
    }
}

void* BindingTexture(FfxInterface& iface, FfxResourceInternal resource, uint32_t mip) {
    Slot* slot = Find(StateOf(Backend(&iface)), resource);
    if (!slot || slot->mip_views.empty()) {
        return nullptr;
    }
    mip = std::min(mip, uint32_t(slot->mip_views.size() - 1));
    return mip == 0 ? slot->handle : StorageView(iface, resource, mip);
}

void* StorageView(FfxInterface& iface, FfxResourceInternal resource, uint32_t mip) {
    Slot* slot = Find(StateOf(Backend(&iface)), resource);
    if (!slot || mip >= slot->mip_views.size()) {
        return nullptr;
    }
    Metal::Object& view = slot->mip_views[mip];
    if (!view) {
        view = Metal::CreateTextureView(slot->handle,
                                        Metal::TexturePixelFormat(slot->handle),
                                        Metal::TextureType::Tex2D, mip, 1, 0, 1,
                                        {MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                         MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha});
    }
    return view.Get();
}

} // namespace FfxMetal
