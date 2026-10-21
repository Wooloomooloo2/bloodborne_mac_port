// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: unit test for the FidelityFX Metal back end (Metal-native M6 Tasks 1-4): every FSR 3.1
// upscaler blob AMD ships (SDK 1.1.4 SPIR-V, wave32, fp16 on and off) is reached through the
// back end's FfxInterface and compiles to a Metal compute pipeline; a second request for a blob
// is served from the pipeline cache. Resources, clears and copies (Task 2); pipelines and compute
// jobs (Task 3): AMD's FSR 3 host code on the back end reproduces the Vulkan path's outputs on
// synthetic frames (tests/data/fsr3, written by the Vulkan golden tool). The renderer's wrapper
// (Fsr3Metal, Task 4) runs those cases, and is re-created across sizes without compiling or
// leaking. Links libbbgpu (the ShaderCompiler, AMD's host code); the MSL disk cache goes to a
// temporary directory (SetHostKernelCacheDir).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include "fsr3upscaler/ffx_fsr3upscaler_private.h"
#include "ffx_metal/ffx_metal.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "ffx_metal/ffx_metal_private.h"
#include "ffx_metal/fsr3_metal.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "fsr3_synthetic.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

const char* PassName(FfxFsr3UpscalerPass pass) {
    switch (pass) {
    case FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS:
        return "prepare_inputs";
    case FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID:
        return "luma_pyramid";
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID:
        return "shading_change_pyramid";
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE:
        return "shading_change";
    case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY:
        return "prepare_reactivity";
    case FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY:
        return "luma_instability";
    case FFX_FSR3UPSCALER_PASS_ACCUMULATE:
        return "accumulate";
    case FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN:
        return "accumulate_sharpen";
    case FFX_FSR3UPSCALER_PASS_RCAS:
        return "rcas";
    case FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE:
        return "generate_reactive";
    default:
        return "?";
    }
}

// The passes FSR 3.1 dispatches (DEBUG_VIEW is a debugging overlay; TCR_AUTOGENERATE is
// deprecated and has no blobs).
constexpr FfxFsr3UpscalerPass kPasses[] = {
    FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS,     FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID,
    FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID, FFX_FSR3UPSCALER_PASS_SHADING_CHANGE,
    FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY, FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY,
    FFX_FSR3UPSCALER_PASS_ACCUMULATE,         FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN,
    FFX_FSR3UPSCALER_PASS_RCAS,               FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE,
};

// The six key bits of the upscaler's permutations (LANCZOS_TYPE .. ENABLE_SHARPENING).
constexpr uint32_t kKeyBits = 6;

void TestAllFsr3BlobsCompile(Metal::Device& device) {
    Expect(FfxMetal::Supported(device), "FSR 3 supported (Apple7+)");
    const FfxDeviceCapabilities caps = FfxMetal::Capabilities(device);
    Expect(caps.waveLaneCountMin == 32 && caps.waveLaneCountMax == 32, "wave32 only");
    Expect(caps.maximumSupportedShaderModel == FFX_SHADER_MODEL_6_2, "shader model 6.2");

    std::vector<uint8_t> scratch(FfxMetal::ScratchSize(1));
    FfxInterface iface{};
    Expect(FfxMetal::GetInterface(&iface, device, scratch.data(), scratch.size(), 1) == FFX_OK,
           "GetInterface");
    Expect(iface.fpGetPermutationBlobByIndex != nullptr, "fpGetPermutationBlobByIndex set");
    if (!iface.fpGetPermutationBlobByIndex) {
        return;
    }

    struct Seen {
        FfxShaderBlob blob;
        std::string name;
    };
    std::vector<Seen> blobs;
    for (const uint32_t fp16 : {0u, uint32_t(FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16)}) {
        for (uint32_t key = 0; key < (1u << kKeyBits); ++key) {
            for (const FfxFsr3UpscalerPass pass : kPasses) {
                const uint32_t options = key | fp16;
                FfxShaderBlob blob{};
                const FfxErrorCode code = iface.fpGetPermutationBlobByIndex(
                    FFX_EFFECT_FSR3UPSCALER, pass, FFX_BIND_COMPUTE_SHADER_STAGE, options, &blob);
                const std::string name = std::string("ffx ") + PassName(pass) +
                                         (fp16 ? " fp16" : "") + " key " + std::to_string(key);
                if (code != FFX_OK || !blob.data || blob.size < 4) {
                    std::printf("FAIL: %s: no blob (code 0x%x)\n", name.c_str(), unsigned(code));
                    ++failures;
                    continue;
                }
                uint32_t magic;
                std::memcpy(&magic, blob.data, sizeof(magic));
                if (magic != 0x07230203u) {
                    std::printf("FAIL: %s: not SPIR-V (0x%08x)\n", name.c_str(), magic);
                    ++failures;
                    continue;
                }
                std::string error;
                void* pipeline = FfxMetal::PipelineFor(device, blob, &error, name);
                if (!pipeline || !error.empty()) {
                    std::printf("FAIL: %s: %s\n", name.c_str(),
                                error.empty() ? "no pipeline" : error.c_str());
                    ++failures;
                    continue;
                }
                bool known = false;
                for (const Seen& seen : blobs) {
                    known |= seen.blob.data == blob.data;
                }
                if (!known) {
                    blobs.push_back({blob, name});
                }
            }
        }
    }

    // Every blob again: served from the cache, no compilation.
    const uint32_t compiled = FfxMetal::PipelineCompileCount();
    bool cached = true;
    for (const Seen& seen : blobs) {
        std::string error;
        cached &= FfxMetal::PipelineFor(device, seen.blob, &error, seen.name) != nullptr &&
                  error.empty();
    }
    Expect(cached, "second request returns a pipeline");
    Expect(FfxMetal::PipelineCompileCount() == compiled, "second request compiles nothing");
    Expect(compiled == blobs.size(), "one compilation per distinct blob");
    if (failures == 0) {
        std::printf("PASS AllFsr3BlobsCompile (%u pipelines)\n", compiled);
    }
}

// --- Task 2: resources, clears and copies ---

id<MTLBuffer> SharedBuffer(Metal::Device& device, NSUInteger length) {
    id<MTLBuffer> buffer = [(__bridge id<MTLDevice>)device.NativeDevice()
        newBufferWithLength:length
                    options:MTLResourceStorageModeShared];
    std::memset(buffer.contents, 0, length);
    return buffer;
}

Metal::BufferRef Ref(id<MTLBuffer> buffer) {
    return {(__bridge void*)buffer, 0};
}

void TestPixelFormatTable() {
    // The formats ffx_fsr3upscaler.cpp's resource list (and the shared ones) use.
    const FfxSurfaceFormat formats[] = {
        FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_SURFACE_FORMAT_R16G16_FLOAT,
        FFX_SURFACE_FORMAT_R16_FLOAT,          FFX_SURFACE_FORMAT_R32_FLOAT,
        FFX_SURFACE_FORMAT_R32_UINT,           FFX_SURFACE_FORMAT_R8_UNORM,
        FFX_SURFACE_FORMAT_R16_UNORM,          FFX_SURFACE_FORMAT_R16_SNORM,
        FFX_SURFACE_FORMAT_R32G32_FLOAT,       FFX_SURFACE_FORMAT_R8G8_UNORM,
        FFX_SURFACE_FORMAT_R8G8B8A8_UNORM,     FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT,
    };
    bool all = true;
    for (const FfxSurfaceFormat format : formats) {
        all &= FfxMetal::PixelFormat(format) != uint32_t(MTLPixelFormatInvalid);
    }
    Expect(all, "every FSR 3 surface format has a Metal pixel format");
    Expect(FfxMetal::PixelFormat(FFX_SURFACE_FORMAT_R16G16_FLOAT) ==
               uint32_t(MTLPixelFormatRG16Float),
           "RG16F maps to RG16Float");
    Expect(FfxMetal::PixelFormat(FFX_SURFACE_FORMAT_UNKNOWN) == uint32_t(MTLPixelFormatInvalid),
           "unknown format is invalid");
    Expect(FfxMetal::PixelFormat(FFX_SURFACE_FORMAT_R16G16_TYPELESS) ==
               uint32_t(MTLPixelFormatInvalid),
           "typeless format is invalid");
    if (failures == 0) {
        std::printf("PASS PixelFormatTable\n");
    }
}

struct Backend {
    std::vector<uint8_t> scratch;
    FfxInterface iface{};
    FfxUInt32 context{};
    bool ok{};
    explicit Backend(Metal::Device& device) : scratch(FfxMetal::ScratchSize(1)) {
        ok = FfxMetal::GetInterface(&iface, device, scratch.data(), scratch.size(), 1) == FFX_OK &&
             iface.fpCreateBackendContext(&iface, FFX_EFFECT_FSR3UPSCALER, nullptr, &context) ==
                 FFX_OK;
    }
};

FfxCreateResourceDescription Describe(FfxSurfaceFormat format, uint32_t width, uint32_t height,
                                      uint32_t mips, FfxResourceInitData init) {
    FfxCreateResourceDescription desc{};
    desc.heapType = FFX_HEAP_TYPE_DEFAULT;
    desc.resourceDescription = {FFX_RESOURCE_TYPE_TEXTURE2D, format, width, height, 1, mips,
                                FFX_RESOURCE_FLAGS_NONE,
                                FfxResourceUsage(FFX_RESOURCE_USAGE_RENDERTARGET |
                                                 FFX_RESOURCE_USAGE_UAV)};
    desc.initialState = FFX_RESOURCE_STATE_COMPUTE_READ;
    desc.name = L"test";
    desc.initData = init;
    return desc;
}

// Reads `level` of `texture` (RG16F) back as floats.
std::vector<float> ReadRg16f(Metal::Device& device, void* texture, uint32_t level, uint32_t width,
                             uint32_t height) {
    Metal::Encoder encoder(device);
    id<MTLBuffer> readback = SharedBuffer(device, size_t(width) * height * 4);
    encoder.CopyTextureToBuffer(texture, level, 0, {}, {width, height, 1}, Ref(readback),
                                width * 4, width * height * 4);
    device.Wait(encoder.Submit());
    std::vector<float> out(size_t(width) * height * 2);
    const auto* halves = static_cast<const _Float16*>(readback.contents);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = float(halves[i]);
    }
    return out;
}

void TestCreateClearCopy(Metal::Device& device) {
    constexpr uint32_t kW = 64, kH = 32, kMips = 3;
    const uint64_t start = FfxMetal::LiveTextureBytes();
    std::vector<float> ramp(kW * kH * 2);
    std::vector<_Float16> ramp_half(ramp.size());
    for (uint32_t i = 0; i < kW * kH; ++i) {
        ramp[i * 2] = float(i) / 2048.0f;
        ramp[i * 2 + 1] = float(2047 - i) / 2048.0f;
        ramp_half[i * 2] = _Float16(ramp[i * 2]);
        ramp_half[i * 2 + 1] = _Float16(ramp[i * 2 + 1]);
    }
    {
        Backend backend(device);
        Expect(backend.ok, "backend context created");
        FfxInterface& iface = backend.iface;

        FfxResourceInternal a{}, b{};
        const FfxCreateResourceDescription desc_a = Describe(
            FFX_SURFACE_FORMAT_R16G16_FLOAT, kW, kH, kMips,
            FfxResourceInitData::FfxResourceInitBuffer(ramp_half.size() * 2, ramp_half.data()));
        const FfxCreateResourceDescription desc_b =
            Describe(FFX_SURFACE_FORMAT_R16G16_FLOAT, kW, kH, kMips,
                     {FFX_RESOURCE_INIT_DATA_TYPE_UNINITIALIZED});
        Expect(iface.fpCreateResource(&iface, &desc_a, backend.context, &a) == FFX_OK, "create A");
        Expect(iface.fpCreateResource(&iface, &desc_b, backend.context, &b) == FFX_OK, "create B");
        Expect(FfxMetal::LiveTextureBytes() > start, "textures count towards LiveTextureBytes");

        const FfxResourceDescription got = iface.fpGetResourceDescription(&iface, a);
        Expect(got.width == kW && got.height == kH && got.mipCount == kMips &&
                   got.format == FFX_SURFACE_FORMAT_R16G16_FLOAT,
               "description of A");
        const FfxResource handle = iface.fpGetResource(&iface, a);
        Expect(handle.resource != nullptr, "A has a texture");
        id<MTLTexture> texture_a = (__bridge id<MTLTexture>)handle.resource;
        Expect(texture_a.mipmapLevelCount == kMips && texture_a.width == kW, "A texture shape");

        // The SDK's clear and copy jobs cover the whole resource (no mip selector): copy A into B
        // (all mips), then clear A (all mips), so A's mip 1 shows the clear and B's mip 0 the
        // ramp (A's init data) that the copy took before it.
        FfxGpuJobDescription copy{};
        copy.jobType = FFX_GPU_JOB_COPY;
        copy.copyJobDescriptor.src = a;
        copy.copyJobDescriptor.dst = b;
        FfxGpuJobDescription clear{};
        clear.jobType = FFX_GPU_JOB_CLEAR_FLOAT;
        clear.clearJobDescriptor = {{0.25f, 0.5f, 0.0f, 0.0f}, a};
        FfxGpuJobDescription barrier{};
        barrier.jobType = FFX_GPU_JOB_BARRIER;
        FfxGpuJobDescription discard{};
        discard.jobType = FFX_GPU_JOB_DISCARD;
        discard.discardJobDescriptor.target = b;
        Expect(iface.fpScheduleGpuJob(&iface, &copy) == FFX_OK, "schedule copy");
        Expect(iface.fpScheduleGpuJob(&iface, &barrier) == FFX_OK, "schedule barrier");
        Expect(iface.fpScheduleGpuJob(&iface, &clear) == FFX_OK, "schedule clear");
        Expect(iface.fpScheduleGpuJob(&iface, &discard) == FFX_OK, "schedule discard");
        {
            Metal::Encoder encoder(device);
            Expect(iface.fpExecuteGpuJobs(&iface, FfxMetal::CommandList(encoder),
                                          backend.context) == FFX_OK,
                   "execute jobs");
            device.Wait(encoder.Submit());
        }

        const std::vector<float> copied =
            ReadRg16f(device, iface.fpGetResource(&iface, b).resource, 0, kW, kH);
        bool ramp_ok = true;
        for (size_t i = 0; i < ramp.size(); ++i) {
            ramp_ok &= copied[i] == ramp[i];
        }
        Expect(ramp_ok, "B mip 0 equals the init ramp (upload and copy)");
        for (uint32_t mip = 0; mip < kMips; ++mip) {
            const uint32_t w = kW >> mip, h = kH >> mip;
            const std::vector<float> cleared = ReadRg16f(device, handle.resource, mip, w, h);
            bool clear_ok = true;
            for (size_t i = 0; i < size_t(w) * h; ++i) {
                clear_ok &= cleared[i * 2] == 0.25f && cleared[i * 2 + 1] == 0.5f;
            }
            Expect(clear_ok, mip == 1 ? "A mip 1 equals the clear colour" : "A mips cleared");
        }

        // Per-mip storage views (for UAV bindings).
        void* view = FfxMetal::StorageView(iface, a, 1);
        Expect(view != nullptr, "storage view of mip 1");
        if (view) {
            id<MTLTexture> mip_view = (__bridge id<MTLTexture>)view;
            Expect(mip_view.mipmapLevelCount == 1 && mip_view.width == kW / 2,
                   "storage view is the one mip");
            Expect(FfxMetal::StorageView(iface, a, 1) == view, "storage view is cached");
        }
        Expect(FfxMetal::StorageView(iface, a, kMips) == nullptr, "no storage view past the mips");

        Expect(iface.fpDestroyResource(&iface, a, backend.context) == FFX_OK, "destroy A");
        Expect(iface.fpDestroyResource(&iface, b, backend.context) == FFX_OK, "destroy B");
        Expect(FfxMetal::LiveTextureBytes() == start, "LiveTextureBytes back to its start");

        // A resource left alive goes with its context.
        FfxResourceInternal c{};
        Expect(iface.fpCreateResource(&iface, &desc_b, backend.context, &c) == FFX_OK, "create C");
        Expect(FfxMetal::LiveTextureBytes() > start, "C is live");
        Expect(iface.fpDestroyBackendContext(&iface, backend.context) == FFX_OK,
               "destroy the backend context");
        Expect(FfxMetal::LiveTextureBytes() == start, "destroying the context frees C");
    }
    device.Finish();
    if (failures == 0) {
        std::printf("PASS CreateClearCopy\n");
    }
}

void TestRegisterNull(Metal::Device& device) {
    Backend backend(device);
    FfxInterface& iface = backend.iface;
    FfxResourceInternal null_resource{-1};
    const FfxResource none = FfxMetal::Resource(nullptr, FFX_RESOURCE_USAGE_READ_ONLY, L"none");
    Expect(none.resource == nullptr, "Resource(nullptr) is a null FfxResource");
    Expect(iface.fpRegisterResource(&iface, &none, backend.context, &null_resource) == FFX_OK,
           "registering a null resource is OK");
    Expect(iface.fpGetResourceDescription(&iface, null_resource).width == 0,
           "a null resource has width 0");
    Expect(iface.fpGetResource(&iface, null_resource).resource == nullptr, "and no texture");

    // A real texture registered per dispatch and dropped by fpUnregisterResources.
    Metal::TextureDesc desc;
    desc.pixel_format = uint32_t(MTLPixelFormatRG16Float);
    desc.width = 16;
    desc.height = 8;
    desc.levels = 2;
    desc.render_target = desc.shader_write = true;
    Metal::Object texture = Metal::CreateTexture(device, desc, "registered", nullptr);
    const FfxResource described =
        FfxMetal::Resource(texture.Get(), FfxResourceUsage(FFX_RESOURCE_USAGE_UAV), L"registered");
    FfxResourceInternal registered{};
    Expect(iface.fpRegisterResource(&iface, &described, backend.context, &registered) == FFX_OK &&
               registered.internalIndex != 0,
           "register a texture");
    const FfxResourceDescription got = iface.fpGetResourceDescription(&iface, registered);
    Expect(got.width == 16 && got.height == 8 && got.mipCount == 2 &&
               got.format == FFX_SURFACE_FORMAT_R16G16_FLOAT,
           "registered description comes from the texture");
    Expect(iface.fpGetResource(&iface, registered).resource == texture.Get(),
           "registered texture is the caller's");
    // The dispatch pattern: jobs using the registered texture's views are encoded, the resources
    // are unregistered (freeing the views) and only then the command buffer is submitted.
    {
        FfxResourceInternal target{};
        const FfxCreateResourceDescription desc_t = Describe(
            FFX_SURFACE_FORMAT_R16G16_FLOAT, 16, 8, 2, {FFX_RESOURCE_INIT_DATA_TYPE_UNINITIALIZED});
        Expect(iface.fpCreateResource(&iface, &desc_t, backend.context, &target) == FFX_OK,
               "create copy target");
        Metal::Encoder encoder(device);
        Expect(FfxMetal::StorageView(iface, registered, 1) != nullptr, "view of the registered mip");
        FfxGpuJobDescription clear{};
        clear.jobType = FFX_GPU_JOB_CLEAR_FLOAT;
        clear.clearJobDescriptor = {{0.25f, 0.5f, 0.0f, 0.0f}, registered};
        FfxGpuJobDescription copy{};
        copy.jobType = FFX_GPU_JOB_COPY;
        copy.copyJobDescriptor.src = registered;
        copy.copyJobDescriptor.dst = target;
        iface.fpScheduleGpuJob(&iface, &clear);
        iface.fpScheduleGpuJob(&iface, &copy);
        Expect(iface.fpExecuteGpuJobs(&iface, FfxMetal::CommandList(encoder), backend.context) ==
                   FFX_OK,
               "execute on registered");
        Expect(iface.fpUnregisterResources(&iface, FfxMetal::CommandList(encoder),
                                           backend.context) == FFX_OK,
               "unregister before submit");
        device.Wait(encoder.Submit());
        const std::vector<float> out =
            ReadRg16f(device, iface.fpGetResource(&iface, target).resource, 1, 8, 4);
        Expect(out[0] == 0.25f && out[1] == 0.5f, "copy of the registered texture survived");
        iface.fpDestroyResource(&iface, target, backend.context);
        // re-register for the check below
        Expect(iface.fpRegisterResource(&iface, &described, backend.context, &registered) == FFX_OK,
               "register again");
    }
    {
        Metal::Encoder encoder(device);
        Expect(iface.fpUnregisterResources(&iface, FfxMetal::CommandList(encoder),
                                           backend.context) == FFX_OK,
               "unregister");
        device.Wait(encoder.Submit());
    }
    Expect(iface.fpGetResourceDescription(&iface, registered).width == 0,
           "unregistered resource is gone");
    Expect(iface.fpDestroyBackendContext(&iface, backend.context) == FFX_OK, "destroy context");
    if (failures == 0) {
        std::printf("PASS RegisterNull\n");
    }
}

// --- Task 3: compute jobs; FSR 3 against the Vulkan path's golden outputs ---

// fpCreatePipeline maps the blob's reflection tables to MSL slots; a binding the compiled shader
// lacks fails the pipeline (here: a real accumulate blob whose first SRV claims binding 99).
FfxGetPermutationBlobByIndexFunc real_blob_accessor = nullptr;
uint32_t doctored_srv_bindings[FFX_MAX_NUM_SRVS];

FfxErrorCode DoctoredBlob(FfxEffect effect, FfxPass pass, FfxBindStage stage, uint32_t options,
                          FfxShaderBlob* out) {
    const FfxErrorCode code = real_blob_accessor(effect, pass, stage, options, out);
    if (code == FFX_OK && out->srvTextureCount > 0) {
        std::memcpy(doctored_srv_bindings, out->boundSRVTextures,
                    out->srvTextureCount * sizeof(uint32_t));
        doctored_srv_bindings[0] = 99;
        const uint32_t* bindings = doctored_srv_bindings;
        // FfxShaderBlob's members are const: patch the one pointer in place.
        std::memcpy(reinterpret_cast<uint8_t*>(out) + offsetof(FfxShaderBlob, boundSRVTextures),
                    &bindings, sizeof(bindings));
    }
    return code;
}

void TestCreatePipelineBindings(Metal::Device& device) {
    const int before = failures;
    Backend backend(device);
    FfxInterface& iface = backend.iface;
    const FfxSamplerDescription samplers[2] = {
        {FFX_FILTER_TYPE_MINMAGMIP_POINT, FFX_ADDRESS_MODE_CLAMP, FFX_ADDRESS_MODE_CLAMP,
         FFX_ADDRESS_MODE_CLAMP, FFX_BIND_COMPUTE_SHADER_STAGE},
        {FFX_FILTER_TYPE_MINMAGMIP_LINEAR, FFX_ADDRESS_MODE_CLAMP, FFX_ADDRESS_MODE_CLAMP,
         FFX_ADDRESS_MODE_CLAMP, FFX_BIND_COMPUTE_SHADER_STAGE}};
    FfxPipelineDescription desc{};
    desc.stage = FFX_BIND_COMPUTE_SHADER_STAGE;
    desc.samplers = samplers;
    desc.samplerCount = 2;
    std::wcsncpy(desc.name, L"FSR3-ACCUMULATE", 63);
    const uint32_t options = FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16;

    FfxPipelineState good{};
    Expect(iface.fpCreatePipeline(&iface, FFX_EFFECT_FSR3UPSCALER, FFX_FSR3UPSCALER_PASS_ACCUMULATE,
                                  options, &desc, backend.context, &good) == FFX_OK,
           "accumulate pipeline");
    Expect(good.pipeline != nullptr && good.srvTextureCount > 0 && good.uavTextureCount > 0 &&
               good.constCount == 1,
           "accumulate pipeline state has its bindings");
    Expect(good.srvTextureBindings[0].name[0] != 0, "binding names for patchResourceBindings");
    Expect(iface.fpDestroyPipeline(&iface, &good, backend.context) == FFX_OK &&
               good.pipeline == nullptr,
           "destroy pipeline");

    real_blob_accessor = iface.fpGetPermutationBlobByIndex;
    iface.fpGetPermutationBlobByIndex = DoctoredBlob;
    FfxPipelineState bad{};
    Expect(iface.fpCreatePipeline(&iface, FFX_EFFECT_FSR3UPSCALER, FFX_FSR3UPSCALER_PASS_ACCUMULATE,
                                  options, &desc, backend.context, &bad) != FFX_OK,
           "a binding missing from the compiled shader fails the pipeline");
    iface.fpGetPermutationBlobByIndex = real_blob_accessor;

    desc.indirectWorkload = 1;
    Expect(iface.fpCreatePipeline(&iface, FFX_EFFECT_FSR3UPSCALER, FFX_FSR3UPSCALER_PASS_ACCUMULATE,
                                  options, &desc, backend.context, &bad) != FFX_OK,
           "indirect workloads are refused");
    Expect(iface.fpDestroyBackendContext(&iface, backend.context) == FFX_OK, "destroy context");
    if (failures == before) {
        std::printf("PASS CreatePipelineBindings\n");
    }
}

// An integer target takes the float clear colour's bits (ffx_vk.cpp's VkClearColorValue union):
// FSR clears its R32_UINT reconstructed depth with 1.0f, read back as asuint(1.0) = 0x3f800000.
void TestIntegerClear(Metal::Device& device) {
    const int before = failures;
    Backend backend(device);
    FfxInterface& iface = backend.iface;
    FfxResourceInternal target{};
    const FfxCreateResourceDescription desc = Describe(
        FFX_SURFACE_FORMAT_R32_UINT, 8, 4, 1, {FFX_RESOURCE_INIT_DATA_TYPE_UNINITIALIZED});
    Expect(iface.fpCreateResource(&iface, &desc, backend.context, &target) == FFX_OK,
           "create R32_UINT");
    FfxGpuJobDescription clear{};
    clear.jobType = FFX_GPU_JOB_CLEAR_FLOAT;
    clear.clearJobDescriptor = {{1.0f, 1.0f, 1.0f, 1.0f}, target};
    iface.fpScheduleGpuJob(&iface, &clear);
    id<MTLBuffer> readback = SharedBuffer(device, 8 * 4 * 4);
    {
        Metal::Encoder encoder(device);
        Expect(iface.fpExecuteGpuJobs(&iface, FfxMetal::CommandList(encoder), backend.context) ==
                   FFX_OK,
               "execute integer clear");
        encoder.CopyTextureToBuffer(iface.fpGetResource(&iface, target).resource, 0, 0, {},
                                    {8, 4, 1}, Ref(readback), 8 * 4, 8 * 4 * 4);
        device.Wait(encoder.Submit());
    }
    const auto* values = static_cast<const uint32_t*>(readback.contents);
    bool ok = true;
    for (uint32_t i = 0; i < 8 * 4; ++i) {
        ok &= values[i] == 0x3f800000u;
    }
    Expect(ok, "R32_UINT cleared with 1.0f holds 0x3f800000");
    Expect(iface.fpDestroyBackendContext(&iface, backend.context) == FFX_OK, "destroy context");
    if (failures == before) {
        std::printf("PASS IntegerClear\n");
    }
}

Metal::Object Texture2D(Metal::Device& device, MTLPixelFormat format, uint32_t width,
                        uint32_t height, bool writable, bool atomic, const char* label) {
    Metal::TextureDesc desc;
    desc.pixel_format = uint32_t(format);
    desc.width = width;
    desc.height = height;
    desc.render_target = desc.shader_write = writable;
    desc.shader_atomic = atomic;
    return Metal::CreateTexture(device, desc, label, nullptr);
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::vector<uint8_t> out;
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
        std::fseek(file, 0, SEEK_END);
        out.resize(size_t(std::ftell(file)));
        std::fseek(file, 0, SEEK_SET);
        if (std::fread(out.data(), 1, out.size(), file) != out.size()) {
            out.clear();
        }
        std::fclose(file);
    }
    return out;
}

// Mean and max absolute error per channel of `got` against `want` (RGBA16F), over the texels
// where both are finite; counts the non-finite values of `got`.
struct Errors {
    float mean[4]{}, max[4]{};
    size_t nonfinite{};
};

Errors Compare(const uint16_t* got, const uint16_t* want, size_t texels) {
    Errors errors;
    double sum[4]{};
    size_t count[4]{};
    for (size_t i = 0; i < texels; ++i) {
        for (int c = 0; c < 4; ++c) {
            const float a = Fsr3Synthetic::FromHalf(got[i * 4 + c]);
            const float b = Fsr3Synthetic::FromHalf(want[i * 4 + c]);
            if (!std::isfinite(a)) {
                ++errors.nonfinite;
                continue;
            }
            if (!std::isfinite(b)) {
                continue;
            }
            const float e = std::fabs(a - b);
            sum[c] += e;
            ++count[c];
            errors.max[c] = std::max(errors.max[c], e);
        }
    }
    for (int c = 0; c < 4; ++c) {
        errors.mean[c] = count[c] ? float(sum[c] / double(count[c])) : 0.0f;
    }
    return errors;
}

// Runs one golden case (fsr3_synthetic.h) through AMD's FSR 3 host code on the Metal back end,
// as the Vulkan tool ran it through ffx-vulkan's portable wrapper: same flags, same frames, the
// three shared resources allocated by the caller, null exposure/reactive/composition masks. The
// renderer's wrapper (Fsr3Metal, Task 4) makes the context and the shared resources: its
// flags (HDR and auto exposure, or none) and constants are the cases' (Fsr3Synthetic).
void RunFsr3Case(Metal::Device& device, const Fsr3Synthetic::Fsr3Case& c) {
    using namespace Fsr3Synthetic;
    const uint32_t w = c.render_width, h = c.render_height;
    const uint32_t ow = c.output_width, oh = c.output_height;
    const bool hdr = c.flags == (kFlagHdr | kFlagAutoExposure);
    Expect(hdr || c.flags == 0, "a golden case has the wrapper's flags");
    FfxMetal::Fsr3Metal fsr3;
    std::string error;
    if (!fsr3.Create(device, {w, h, ow, oh, hdr}, &error)) {
        std::printf("FAIL: %s: Fsr3Metal::Create: %s\n", c.name, error.c_str());
        ++failures;
        return;
    }

    Metal::Object color = Texture2D(device, MTLPixelFormatRGBA16Float, w, h, false, false, "color");
    Metal::Object depth = Texture2D(device, MTLPixelFormatR32Float, w, h, false, false, "depth");
    Metal::Object motion = Texture2D(device, MTLPixelFormatRG16Float, w, h, false, false, "motion");
    Metal::Object output = Texture2D(device, MTLPixelFormatRGBA16Float, ow, oh, true, false, "output");
    id<MTLBuffer> color_staging = SharedBuffer(device, size_t(w) * h * 8);
    id<MTLBuffer> depth_staging = SharedBuffer(device, size_t(w) * h * 4);
    id<MTLBuffer> motion_staging = SharedBuffer(device, size_t(w) * h * 4);
    const size_t output_bytes = size_t(ow) * oh * 8;
    id<MTLBuffer> readback = SharedBuffer(device, output_bytes);

    for (uint32_t index = 0; index < kFrames; ++index) {
        const Fsr3Frame frame = MakeFrame(index, w, h);
        std::memcpy(color_staging.contents, frame.color.data(), frame.color.size() * 2);
        std::memcpy(depth_staging.contents, frame.depth.data(), frame.depth.size() * 4);
        std::memcpy(motion_staging.contents, frame.motion.data(), frame.motion.size() * 2);

        Metal::Encoder encoder(device);
        encoder.CopyBufferToTexture(Ref(color_staging), w * 8, w * h * 8, color.Get(), 0, 0, {},
                                    {w, h, 1});
        encoder.CopyBufferToTexture(Ref(depth_staging), w * 4, w * h * 4, depth.Get(), 0, 0, {},
                                    {w, h, 1});
        encoder.CopyBufferToTexture(Ref(motion_staging), w * 4, w * h * 4, motion.Get(), 0, 0, {},
                                    {w, h, 1});

        FfxMetal::Fsr3Metal::DispatchInfo dispatch{};
        dispatch.color = color.Get();
        dispatch.depth = depth.Get();
        dispatch.motion = motion.Get();
        dispatch.output = output.Get();
        dispatch.render_w = w;
        dispatch.render_h = h;
        dispatch.output_w = ow;
        dispatch.output_h = oh;
        dispatch.jitter_x = frame.jitter_x;
        dispatch.jitter_y = frame.jitter_y;
        dispatch.frame_time_ms = kFrameTimeMs;
        dispatch.camera_near = kCameraNear;
        dispatch.camera_far = kCameraFar;
        dispatch.vertical_fov = kCameraFovY;
        dispatch.sharpness = c.sharpness;
        dispatch.sharpen = c.sharpen;
        dispatch.reset = frame.reset;
        if (!fsr3.Dispatch(encoder, dispatch)) {
            std::printf("FAIL: %s f%u: Fsr3Metal::Dispatch\n", c.name, index);
            ++failures;
            device.Wait(encoder.Submit());
            break;
        }
        bool golden = false;
        for (const uint32_t g : kGoldenFrames) {
            golden |= g == index;
        }
        if (golden) {
            encoder.CopyTextureToBuffer(output.Get(), 0, 0, {}, {ow, oh, 1}, Ref(readback),
                                        ow * 8, uint32_t(output_bytes));
        }
        device.Wait(encoder.Submit());
        if (!golden) {
            continue;
        }

        const std::string path =
            std::string(BB_FSR3_GOLDEN_DIR) + "/" + c.name + "_f" + std::to_string(index) + ".bin";
        const std::vector<uint8_t> want = ReadFile(path);
        if (want.size() != output_bytes) {
            std::printf("FAIL: %s: golden file missing or of the wrong size\n", path.c_str());
            ++failures;
            continue;
        }
        const Errors e = Compare(static_cast<const uint16_t*>(readback.contents),
                                 reinterpret_cast<const uint16_t*>(want.data()), size_t(ow) * oh);
        const float mean = std::max({e.mean[0], e.mean[1], e.mean[2], e.mean[3]});
        const float max = std::max({e.max[0], e.max[1], e.max[2], e.max[3]});
        std::printf("  %s f%u mean %.4f max %.4f (per channel mean %.4f %.4f %.4f %.4f, "
                    "max %.4f %.4f %.4f %.4f)\n",
                    c.name, index, mean, max, e.mean[0], e.mean[1], e.mean[2], e.mean[3],
                    e.max[0], e.max[1], e.max[2], e.max[3]);
        if (e.nonfinite != 0) {
            std::printf("FAIL: %s f%u: %zu NaN/Inf values\n", c.name, index, e.nonfinite);
            ++failures;
        }
        if (mean > 0.002f || max > 0.03f) {
            std::printf("FAIL: %s f%u differs from the Vulkan path (mean <= 0.002, max <= 0.03)\n",
                        c.name, index);
            ++failures;
        }
    }
    fsr3.Destroy(); // every frame was waited for
    device.Finish();
}

void TestFsr3MatchesVulkan(Metal::Device& device) {
    const int before = failures;
    const uint64_t start = FfxMetal::LiveTextureBytes();
    for (const Fsr3Synthetic::Fsr3Case& c : Fsr3Synthetic::kCases) {
        RunFsr3Case(device, c);
    }
    Expect(FfxMetal::LiveTextureBytes() == start, "FSR 3 contexts free their textures");
    if (failures == before) {
        std::printf("PASS Fsr3MatchesVulkan\n");
    }
}

// --- Task 4: null bindings and the Fsr3Metal wrapper ---

// Reads `texture` (RGBA16F, width x height, level 0) back as half floats.
std::vector<uint16_t> ReadRgba16f(Metal::Device& device, void* texture, uint32_t width,
                                  uint32_t height) {
    Metal::Encoder encoder(device);
    id<MTLBuffer> readback = SharedBuffer(device, size_t(width) * height * 8);
    encoder.CopyTextureToBuffer(texture, 0, 0, {}, {width, height, 1}, Ref(readback), width * 8,
                                width * height * 8);
    device.Wait(encoder.Submit());
    const auto* data = static_cast<const uint16_t*>(readback.contents);
    return {data, data + size_t(width) * height * 4};
}

// A null SRV unbinds its slot: Encoder slots persist across dispatches, so skipping it would leave
// the previous job's texture bound. Two RCAS jobs over a flat 1.0 input with exposure 1.0; the
// second names a null RCAS input and must not reproduce the first one's output.
void TestNullBindingUnbinds(Metal::Device& device) {
    const int before = failures;
    Backend backend(device);
    FfxInterface& iface = backend.iface;
    const FfxSamplerDescription samplers[2] = {
        {FFX_FILTER_TYPE_MINMAGMIP_POINT, FFX_ADDRESS_MODE_CLAMP, FFX_ADDRESS_MODE_CLAMP,
         FFX_ADDRESS_MODE_CLAMP, FFX_BIND_COMPUTE_SHADER_STAGE},
        {FFX_FILTER_TYPE_MINMAGMIP_LINEAR, FFX_ADDRESS_MODE_CLAMP, FFX_ADDRESS_MODE_CLAMP,
         FFX_ADDRESS_MODE_CLAMP, FFX_BIND_COMPUTE_SHADER_STAGE}};
    FfxPipelineDescription desc{};
    desc.stage = FFX_BIND_COMPUTE_SHADER_STAGE;
    desc.samplers = samplers;
    desc.samplerCount = 2;
    std::wcsncpy(desc.name, L"FSR3-RCAS", 63);
    FfxPipelineState rcas{};
    Expect(iface.fpCreatePipeline(&iface, FFX_EFFECT_FSR3UPSCALER, FFX_FSR3UPSCALER_PASS_RCAS,
                                  FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16, &desc,
                                  backend.context, &rcas) == FFX_OK,
           "RCAS pipeline");

    constexpr uint32_t kSize = 16; // one RCAS threadgroup (16 x 16 pixels)
    const FfxResourceInitData none{FFX_RESOURCE_INIT_DATA_TYPE_UNINITIALIZED};
    FfxResourceInternal input{}, exposure{}, outputs[2]{};
    const FfxCreateResourceDescription color_desc =
        Describe(FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, kSize, kSize, 1, none);
    const FfxCreateResourceDescription exposure_desc =
        Describe(FFX_SURFACE_FORMAT_R32G32_FLOAT, 1, 1, 1, none);
    bool created =
        iface.fpCreateResource(&iface, &color_desc, backend.context, &input) == FFX_OK &&
        iface.fpCreateResource(&iface, &exposure_desc, backend.context, &exposure) == FFX_OK;
    for (FfxResourceInternal& output : outputs) {
        created &= iface.fpCreateResource(&iface, &color_desc, backend.context, &output) == FFX_OK;
    }
    Expect(created, "RCAS resources");
    if (!created || !rcas.pipeline) {
        return;
    }

    FfxGpuJobDescription clear{};
    clear.jobType = FFX_GPU_JOB_CLEAR_FLOAT;
    clear.clearJobDescriptor = {{1.0f, 1.0f, 1.0f, 1.0f}, input};
    iface.fpScheduleGpuJob(&iface, &clear);
    clear.clearJobDescriptor.target = exposure;
    iface.fpScheduleGpuJob(&iface, &clear);

    // Constant buffers of zeros (RCAS config 0: some sharpening; the output depends on the input
    // either way), sized as FSR stages them.
    static uint32_t constants[sizeof(Fsr3UpscalerConstants) / 4]{};
    static uint32_t rcas_constants[4]{}; // Fsr3UpscalerRcasConstants (ffx_fsr3upscaler.cpp)
    for (uint32_t pass = 0; pass < 2; ++pass) {
        FfxGpuJobDescription job{};
        job.jobType = FFX_GPU_JOB_COMPUTE;
        FfxComputeJobDescription& compute = job.computeJobDescriptor;
        compute.pipeline = rcas;
        compute.dimensions[0] = compute.dimensions[1] = compute.dimensions[2] = 1;
        for (uint32_t i = 0; i < rcas.srvTextureCount; ++i) {
            const std::wstring name = rcas.srvTextureBindings[i].name;
            if (name == L"r_input_exposure") {
                compute.srvTextures[i].resource = exposure;
            } else if (name == L"r_rcas_input" && pass == 0) {
                compute.srvTextures[i].resource = input;
            } // else the null resource (internalIndex 0)
        }
        for (uint32_t i = 0; i < rcas.uavTextureCount; ++i) {
            compute.uavTextures[i].resource = outputs[pass];
        }
        for (uint32_t i = 0; i < rcas.constCount; ++i) {
            const bool rcas_cb = std::wstring(rcas.constantBufferBindings[i].name) == L"cbRCAS";
            compute.cbs[i].data = rcas_cb ? rcas_constants : constants;
            compute.cbs[i].num32BitEntries =
                rcas_cb ? uint32_t(std::size(rcas_constants)) : uint32_t(std::size(constants));
        }
        iface.fpScheduleGpuJob(&iface, &job);
    }
    {
        Metal::Encoder encoder(device);
        Expect(iface.fpExecuteGpuJobs(&iface, FfxMetal::CommandList(encoder), backend.context) ==
                   FFX_OK,
               "execute RCAS jobs");
        device.Wait(encoder.Submit());
    }
    const auto bound = ReadRgba16f(device, iface.fpGetResource(&iface, outputs[0]).resource, kSize,
                                   kSize);
    const auto unbound = ReadRgba16f(device, iface.fpGetResource(&iface, outputs[1]).resource,
                                     kSize, kSize);
    const float first = Fsr3Synthetic::FromHalf(bound[0]);
    Expect(std::fabs(first - 1.0f) < 0.01f, "RCAS of a flat 1.0 input is 1.0");
    Expect(bound != unbound, "a null SRV does not read the previous job's texture");
    iface.fpDestroyPipeline(&iface, &rcas, backend.context);
    Expect(iface.fpDestroyBackendContext(&iface, backend.context) == FFX_OK, "destroy context");
    if (failures == before) {
        std::printf("PASS NullBindingUnbinds\n");
    }
}

// The synthetic scene's inputs at one render size, and an output at one output size.
struct WrapperFrame {
    Metal::Object color, depth, motion, reactive, output;
    id<MTLBuffer> color_staging, depth_staging, motion_staging, reactive_staging;
    uint32_t w, h, ow, oh;

    WrapperFrame(Metal::Device& device, uint32_t w_, uint32_t h_, uint32_t ow_, uint32_t oh_)
        : w(w_), h(h_), ow(ow_), oh(oh_) {
        color = Texture2D(device, MTLPixelFormatRGBA16Float, w, h, false, false, "color");
        depth = Texture2D(device, MTLPixelFormatR32Float, w, h, false, false, "depth");
        motion = Texture2D(device, MTLPixelFormatRG16Float, w, h, false, false, "motion");
        reactive = Texture2D(device, MTLPixelFormatR8Unorm, w, h, false, false, "reactive");
        output = Texture2D(device, MTLPixelFormatRGBA16Float, ow, oh, true, false, "output");
        color_staging = SharedBuffer(device, size_t(w) * h * 8);
        depth_staging = SharedBuffer(device, size_t(w) * h * 4);
        motion_staging = SharedBuffer(device, size_t(w) * h * 4);
        reactive_staging = SharedBuffer(device, size_t(w) * h);
        std::memset(reactive_staging.contents, 0x80, size_t(w) * h); // reactive 0.5
    }

    void Upload(Metal::Encoder& encoder, uint32_t index) {
        const Fsr3Synthetic::Fsr3Frame frame = Fsr3Synthetic::MakeFrame(index, w, h);
        std::memcpy(color_staging.contents, frame.color.data(), frame.color.size() * 2);
        std::memcpy(depth_staging.contents, frame.depth.data(), frame.depth.size() * 4);
        std::memcpy(motion_staging.contents, frame.motion.data(), frame.motion.size() * 2);
        encoder.CopyBufferToTexture(Ref(color_staging), w * 8, w * h * 8, color.Get(), 0, 0, {},
                                    {w, h, 1});
        encoder.CopyBufferToTexture(Ref(depth_staging), w * 4, w * h * 4, depth.Get(), 0, 0, {},
                                    {w, h, 1});
        encoder.CopyBufferToTexture(Ref(motion_staging), w * 4, w * h * 4, motion.Get(), 0, 0, {},
                                    {w, h, 1});
        encoder.CopyBufferToTexture(Ref(reactive_staging), w, w * h, reactive.Get(), 0, 0, {},
                                    {w, h, 1});
    }
};

// A context creation that fails halfway (AMD's create makes the back-end context and its textures
// before the pipelines, and returns without cleanup) leaves nothing behind, on every retry.
FfxErrorCode FailingCreatePipeline(FfxInterface*, FfxEffect, FfxPass, uint32_t,
                                   const FfxPipelineDescription*, FfxUInt32, FfxPipelineState*) {
    return FFX_ERROR_BACKEND_API_ERROR;
}

void FailPipelines(FfxInterface* iface) {
    iface->fpCreatePipeline = FailingCreatePipeline;
}

void TestWrapperCreateFailure(Metal::Device& device) {
    const int before = failures;
    const uint64_t start = FfxMetal::LiveTextureBytes();
    FfxMetal::Fsr3Metal fsr3;
    const FfxMetal::Fsr3Metal::CreateInfo info{1280, 720, 1920, 1080, true};
    FfxMetal::Fsr3Metal::SetInterfaceHookForTests(FailPipelines);
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string error;
        Expect(!fsr3.Create(device, info, &error) && !error.empty(),
               "Create fails when a pipeline cannot be made");
        Expect(fsr3.MemoryBytes() == 0 && !fsr3.Matches(info), "a failed Create leaves no context");
        Expect(FfxMetal::LiveTextureBytes() == start, "a failed Create leaves no texture");
    }
    FfxMetal::Fsr3Metal::SetInterfaceHookForTests(nullptr);
    std::string error;
    Expect(fsr3.Create(device, info, &error), "Create succeeds after failed attempts");
    fsr3.Destroy();
    Expect(FfxMetal::LiveTextureBytes() == start, "and Destroy frees its textures");
    if (failures == before) {
        std::printf("PASS WrapperCreateFailure\n");
    }
}

// Fsr3Metal at the renderer's sizes: a preset change (render size), an output size change and
// back. Re-creation compiles no pipeline (the per-device cache), and the last Destroy leaves the
// back end's texture total where it started. The first frame of each size has a reactive mask,
// the second none.
void TestWrapperRecreate(Metal::Device& device) {
    const int before = failures;
    const uint64_t start = FfxMetal::LiveTextureBytes();
    struct Size {
        uint32_t w, h, ow, oh;
    };
    constexpr Size kSizes[] = {
        {1280, 720, 1920, 1080}, {960, 540, 1920, 1080}, {960, 540, 2560, 1440},
        {1280, 720, 1920, 1080}};
    FfxMetal::Fsr3Metal fsr3;
    Expect(fsr3.MemoryBytes() == 0 && !fsr3.Matches({1280, 720, 1920, 1080, true}),
           "no context before Create");
    uint32_t compiled = 0;
    for (size_t i = 0; i < std::size(kSizes); ++i) {
        const Size& size = kSizes[i];
        const FfxMetal::Fsr3Metal::CreateInfo info{size.w, size.h, size.ow, size.oh, true};
        if (i > 0) {
            fsr3.Destroy(); // every frame below was waited for
            Expect(fsr3.MemoryBytes() == 0 && !fsr3.Matches(info), "Destroy releases the context");
        }
        std::string error;
        if (!fsr3.Create(device, info, &error)) {
            std::printf("FAIL: Fsr3Metal::Create %ux%u -> %ux%u: %s\n", size.w, size.h, size.ow,
                        size.oh, error.c_str());
            ++failures;
            return;
        }
        if (i == 0) {
            compiled = FfxMetal::PipelineCompileCount();
        } else {
            Expect(FfxMetal::PipelineCompileCount() == compiled,
                   "re-creation at another size compiles no pipeline");
        }
        Expect(fsr3.Matches(info), "Matches the CreateInfo it was made for");
        Expect(!fsr3.Matches({size.w, size.h, size.ow, size.oh, false}) &&
                   !fsr3.Matches({size.w + 1, size.h, size.ow, size.oh, true}) &&
                   !fsr3.Matches({size.w, size.h, size.ow, size.oh + 1, true}),
               "Matches no other CreateInfo");
        Expect(fsr3.MemoryBytes() > 0 && FfxMetal::LiveTextureBytes() > start,
               "a context holds textures");

        WrapperFrame frame(device, size.w, size.h, size.ow, size.oh);
        for (uint32_t index = 0; index < 2; ++index) {
            Metal::Encoder encoder(device);
            frame.Upload(encoder, index);
            FfxMetal::Fsr3Metal::DispatchInfo dispatch{};
            dispatch.color = frame.color.Get();
            dispatch.depth = frame.depth.Get();
            dispatch.motion = frame.motion.Get();
            dispatch.reactive = index == 0 ? frame.reactive.Get() : nullptr;
            dispatch.output = frame.output.Get();
            dispatch.render_w = size.w;
            dispatch.render_h = size.h;
            dispatch.output_w = size.ow;
            dispatch.output_h = size.oh;
            dispatch.jitter_x = 0.25f;
            dispatch.jitter_y = -0.125f;
            dispatch.frame_time_ms = Fsr3Synthetic::kFrameTimeMs;
            dispatch.camera_near = Fsr3Synthetic::kCameraNear;
            dispatch.camera_far = Fsr3Synthetic::kCameraFar;
            dispatch.vertical_fov = Fsr3Synthetic::kCameraFovY;
            dispatch.sharpness = 0.5f;
            dispatch.sharpen = true;
            dispatch.reset = index == 0;
            Expect(fsr3.Dispatch(encoder, dispatch), "Fsr3Metal::Dispatch");
            device.Wait(encoder.Submit());
        }
        const auto out = ReadRgba16f(device, frame.output.Get(), size.ow, size.oh);
        bool finite = true, written = false;
        for (const uint16_t bits : out) {
            const float value = Fsr3Synthetic::FromHalf(bits);
            finite &= std::isfinite(value);
            written |= value != 0.0f;
        }
        Expect(finite && written, "the upscaled frame is written and finite");
    }
    // The game's depth formats (Depth32Float, Depth32Float_Stencil8, read through the depth
    // aspect as FSR's texture2d<float>): one frame each on the last context.
    {
        const Size& size = kSizes[std::size(kSizes) - 1];
        WrapperFrame frame(device, size.w, size.h, size.ow, size.oh);
        for (const MTLPixelFormat format :
             {MTLPixelFormatDepth32Float, MTLPixelFormatDepth32Float_Stencil8}) {
            Metal::Object depth =
                Texture2D(device, format, size.w, size.h, false, false, "game depth");
            Metal::Encoder encoder(device);
            frame.Upload(encoder, 2);
            encoder.CopyBufferToTexture(Ref(frame.depth_staging), size.w * 4,
                                        size.w * size.h * 4, depth.Get(), 0, 0, {},
                                        {size.w, size.h, 1}, Metal::BlitAspect::Depth);
            FfxMetal::Fsr3Metal::DispatchInfo dispatch{};
            dispatch.color = frame.color.Get();
            dispatch.depth = depth.Get();
            dispatch.motion = frame.motion.Get();
            dispatch.output = frame.output.Get();
            dispatch.render_w = size.w;
            dispatch.render_h = size.h;
            dispatch.output_w = size.ow;
            dispatch.output_h = size.oh;
            dispatch.frame_time_ms = Fsr3Synthetic::kFrameTimeMs;
            dispatch.camera_near = Fsr3Synthetic::kCameraNear;
            dispatch.camera_far = Fsr3Synthetic::kCameraFar;
            dispatch.vertical_fov = Fsr3Synthetic::kCameraFovY;
            dispatch.reset = true;
            Expect(fsr3.Dispatch(encoder, dispatch), "Fsr3Metal::Dispatch with a depth format");
            device.Wait(encoder.Submit());
            const auto out = ReadRgba16f(device, frame.output.Get(), size.ow, size.oh);
            bool finite = true, written = false;
            for (const uint16_t bits : out) {
                const float value = Fsr3Synthetic::FromHalf(bits);
                finite &= std::isfinite(value);
                written |= value != 0.0f;
            }
            Expect(finite && written, format == MTLPixelFormatDepth32Float
                                          ? "upscaled with Depth32Float depth"
                                          : "upscaled with Depth32Float_Stencil8 depth");
        }
    }
    fsr3.Destroy();
    Expect(fsr3.MemoryBytes() == 0, "no memory after Destroy");
    Expect(FfxMetal::LiveTextureBytes() == start, "the last Destroy frees every texture");
    if (failures == before) {
        std::printf("PASS WrapperRecreate\n");
    }
}

} // namespace

int main() {
    @autoreleasepool {
        std::string dir_template =
            (std::filesystem::temp_directory_path() / "bb-ffx-metal-test.XXXXXX").string();
        const char* dir = mkdtemp(dir_template.data());
        if (!dir) {
            std::printf("FAIL: no temporary directory\n");
            return 1;
        }
        Metal::SetHostKernelCacheDir(dir);
        Metal::Device device;
        TestAllFsr3BlobsCompile(device);
        TestPixelFormatTable();
        TestCreateClearCopy(device);
        TestRegisterNull(device);
        TestCreatePipelineBindings(device);
        TestIntegerClear(device);
        TestFsr3MatchesVulkan(device);
        TestNullBindingUnbinds(device);
        TestWrapperRecreate(device);
        TestWrapperCreateFailure(device);
        Expect(device.ErrorCount() == 0, "no command buffer errors on the device queue");
        std::filesystem::remove_all(dir);
    }
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    return 0;
}
