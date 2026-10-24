// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the FidelityFX SDK 1.1.4 back end on native Metal (Metal-native M6). AMD's FSR 3.1 host
// code (ffx_fsr3upscaler.cpp) talks to it through the FfxInterface callbacks, the role ffx_vk.cpp
// plays on Vulkan; AMD's SPIR-V blobs become Metal compute pipelines through the renderer's
// ShaderCompiler. Plain C++: Metal objects cross as void*.

#pragma once

#include <bit> // ffx_util.h uses std::popcount from C++20 on without including it
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <FidelityFX/host/ffx_interface.h>

namespace Metal {
class Device;
class Encoder;
} // namespace Metal

namespace FfxMetal {

/// Bytes of scratch memory GetInterface needs for `max_contexts` effect contexts.
size_t ScratchSize(uint32_t max_contexts);

/// Fills `out` with the Metal back end on `device`. `scratch` (ScratchSize bytes, owned by the
/// caller, outliving the interface) holds the back end's state.
FfxErrorCode GetInterface(FfxInterface* out, Metal::Device& device, void* scratch,
                          size_t scratch_size, uint32_t max_contexts);

/// Releases every effect context still live on `iface` (GetInterface's) with its textures: what
/// an effect's failed context creation leaves behind (AMD's create returns without cleanup). The
/// GPU must be idle for those textures, as for fpDestroyBackendContext.
void ReleaseInterface(FfxInterface* iface);

/// The command list FFX dispatches record into: `encoder`, which must outlive the dispatch.
FfxCommandList CommandList(Metal::Encoder& encoder);

/// What the back end reports to FFX: shader model 6.2, wave32 only, fp16 (BB_FFX_FP16=0: off),
/// nothing else.
FfxDeviceCapabilities Capabilities(Metal::Device& device);

/// FSR 3 needs MTLGPUFamilyApple7 (texture atomics on r32uint, quad operations).
bool Supported(Metal::Device& device);

/// The compute pipeline of `blob` (id<MTLComputePipelineState>, unretained, alive as long as
/// `device`), compiled once per device and blob and then cached. Null on failure, with the cause
/// (and `name`, a label for logs and errors) in `error` when given.
void* PipelineFor(Metal::Device& device, const FfxShaderBlob& blob, std::string* error,
                  std::string_view name = "ffx");

/// Pipelines compiled by PipelineFor so far (cache misses), for tests.
uint32_t PipelineCompileCount();

/// The MTLPixelFormat (its numeric value; 0, MTLPixelFormatInvalid, for formats FSR 3 does not
/// use: typeless, 3-channel, ...) of an FFX surface format.
uint32_t PixelFormat(FfxSurfaceFormat format);

/// Describes `texture` (id<MTLTexture>, unretained) for fpRegisterResource: a 2D texture of its
/// format, size and mip count with `usage`. A null texture gives a null FfxResource.
FfxResource Resource(void* texture, FfxResourceUsage usage, const wchar_t* name);

/// Bytes of the textures the back end holds (created and not yet destroyed), for tests.
uint64_t LiveTextureBytes();

} // namespace FfxMetal
