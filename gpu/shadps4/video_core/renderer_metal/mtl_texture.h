// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: textures, texture views and sampler states for the texture cache (Metal-native
// renderer). Plain C++: Metal enums cross as their numeric values (see vk_to_mtl.h).

#pragma once

#include <array>
#include <string_view>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {

class Device;

enum class TextureType : u8 { Tex2D, Tex2DArray, Tex2DMultisample, Tex2DMultisampleArray, Tex3D };

struct TextureDesc {
    TextureType type{TextureType::Tex2D};
    u32 pixel_format{}; ///< MTLPixelFormat (from VkToMtl::PixelFormat)
    u32 width{1}, height{1}, depth{1}, levels{1}, layers{1}, samples{1};
    bool render_target{}; ///< MTLTextureUsageRenderTarget
    bool shader_write{};  ///< MTLTextureUsageShaderWrite
    bool shader_atomic{}; ///< MTLTextureUsageShaderAtomic (atomics on r32 integer textures)
    bool format_views{};  ///< MTLTextureUsagePixelFormatView (views with another pixel format)
};

/// Creates a private-storage texture (usage ShaderRead plus the desc's flags). `allocated_size`
/// (optional) receives the texture's allocatedSize. Returns an empty Object on failure.
Object CreateTexture(Device& device, const TextureDesc& desc, std::string_view label,
                     u64* allocated_size);

/// A view of `texture` (id<MTLTexture>, unretained) with another pixel format/type, a level and
/// slice range and a swizzle (four MTLTextureSwizzle values, r g b a). Empty Object on failure.
Object CreateTextureView(void* texture, u32 pixel_format, TextureType type, u32 base_level,
                         u32 levels, u32 base_layer, u32 layers, std::array<u8, 4> swizzle);

struct SamplerDesc {
    u32 min_filter{}, mag_filter{}, mip_filter{}; ///< MTLSamplerMinMagFilter / MTLSamplerMipFilter
    u32 address_u{}, address_v{}, address_w{};    ///< MTLSamplerAddressMode
    u32 border{};                                 ///< MTLSamplerBorderColor
    float min_lod{}, max_lod{};
    float lod_bias{}; ///< MTLSamplerDescriptor.lodBias (macOS 26); ignored (logged once) before
    u32 max_anisotropy{1}; ///< clamped to 1..16
    bool compare{};
    u32 compare_function{}; ///< MTLCompareFunction, used when `compare`
    bool normalized{true};
};

/// An MTLSamplerState (not argument-buffer capable). Empty Object on failure.
Object CreateSampler(Device& device, const SamplerDesc& desc);

/// A texture's pixelFormat and sampleCount (id<MTLTexture>, unretained), for callers that only
/// hold the texture (sample counts may have been clamped to what the device supports).
u32 TexturePixelFormat(void* texture);
u32 TextureSampleCount(void* texture);

/// [MTLDevice supportsTextureSampleCount:].
bool SupportsSampleCount(Device& device, u32 samples);

/// The pixel format of a view that samples the stencil of a texture of `pixel_format`:
/// X32_Stencil8 for Depth32Float_Stencil8, X24_Stencil8 for Depth24Unorm_Stencil8, Stencil8 for
/// Stencil8; 0 for formats without stencil.
u32 StencilViewPixelFormat(u32 pixel_format);

/// The pixel format to view a texture of `texture_format` with when `requested_format` is wanted:
/// `requested_format` when Metal can make that view, else `texture_format` (Metal aborts on an
/// incompatible view). Allowed: the same format; its sRGB/linear twin; for depth textures only the
/// stencil view format (StencilViewPixelFormat) besides those; for block-compressed and depth
/// textures nothing else; otherwise, with `texture_has_format_views` (PixelFormatView usage), an
/// uncompressed non-depth format of the same texel size.
u32 SafeViewPixelFormat(u32 texture_format, u32 requested_format, bool texture_has_format_views);

/// Bytes per texel block of a pixel format (a texel for uncompressed formats) and the block's
/// size in texels; BytesPerBlock is 0 for formats without a table entry (buffer copies of them
/// are not supported). Block width/height are 1 for uncompressed formats.
u32 BytesPerBlock(u32 pixel_format);
u32 BlockWidth(u32 pixel_format);
u32 BlockHeight(u32 pixel_format);

/// True for integer (Uint/Sint) colour formats: Metal cannot multisample-resolve them.
bool IsIntegerPixelFormat(u32 pixel_format);

} // namespace Metal
