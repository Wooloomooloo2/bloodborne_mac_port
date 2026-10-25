// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: textures, texture views and sampler states for the texture cache (Metal-native
// renderer).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace Metal {

namespace {

MTLTextureType TypeOf(TextureType type) {
    switch (type) {
    case TextureType::Tex2DArray:
        return MTLTextureType2DArray;
    case TextureType::Tex2DMultisample:
        return MTLTextureType2DMultisample;
    case TextureType::Tex2DMultisampleArray:
        return MTLTextureType2DMultisampleArray;
    case TextureType::Tex3D:
        return MTLTextureType3D;
    case TextureType::Tex2D:
    default:
        return MTLTextureType2D;
    }
}

/// Bytes and block size of a pixel format, for buffer <-> texture copies.
struct Block {
    u32 bytes{}, width{1}, height{1};
};
Block BlockOf(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB:
    case MTLPixelFormatR8Snorm: case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint:
    case MTLPixelFormatStencil8:
        return {1};
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint:
    case MTLPixelFormatR16Sint: case MTLPixelFormatR16Float: case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG8Uint:
    case MTLPixelFormatRG8Sint: case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatA1BGR5Unorm:
    case MTLPixelFormatABGR4Unorm: case MTLPixelFormatBGR5A1Unorm: case MTLPixelFormatDepth16Unorm:
        return {2};
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float:
    case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRG16Sint: case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatRGBA8Sint: case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG11B10Float:
    case MTLPixelFormatRGB9E5Float: case MTLPixelFormatBGR10A2Unorm: case MTLPixelFormatDepth32Float:
        return {4};
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA16Float:
        return {8};
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Float:
        return {16};
    case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC1_RGBA_sRGB: case MTLPixelFormatBC4_RUnorm:
    case MTLPixelFormatBC4_RSnorm:
        return {8, 4, 4};
    case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC2_RGBA_sRGB: case MTLPixelFormatBC3_RGBA:
    case MTLPixelFormatBC3_RGBA_sRGB: case MTLPixelFormatBC5_RGUnorm: case MTLPixelFormatBC5_RGSnorm:
    case MTLPixelFormatBC6H_RGBFloat: case MTLPixelFormatBC6H_RGBUfloat: case MTLPixelFormatBC7_RGBAUnorm:
    case MTLPixelFormatBC7_RGBAUnorm_sRGB:
        return {16, 4, 4};
    default:
        return {};
    }
}

/// The sRGB/linear twin of a format, or Invalid.
MTLPixelFormat SrgbTwin(MTLPixelFormat f) {
    switch (f) {
#define TWIN(a, b)                                                                                 \
    case MTLPixelFormat##a:                                                                        \
        return MTLPixelFormat##b;                                                                  \
    case MTLPixelFormat##b:                                                                        \
        return MTLPixelFormat##a;
        TWIN(R8Unorm, R8Unorm_sRGB)
        TWIN(RG8Unorm, RG8Unorm_sRGB)
        TWIN(RGBA8Unorm, RGBA8Unorm_sRGB)
        TWIN(BGRA8Unorm, BGRA8Unorm_sRGB)
        TWIN(BC1_RGBA, BC1_RGBA_sRGB)
        TWIN(BC2_RGBA, BC2_RGBA_sRGB)
        TWIN(BC3_RGBA, BC3_RGBA_sRGB)
        TWIN(BC7_RGBAUnorm, BC7_RGBAUnorm_sRGB)
#undef TWIN
    default:
        return MTLPixelFormatInvalid;
    }
}

bool IsDepthOrStencil(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float:
    case MTLPixelFormatStencil8: case MTLPixelFormatDepth24Unorm_Stencil8:
    case MTLPixelFormatDepth32Float_Stencil8: case MTLPixelFormatX32_Stencil8:
    case MTLPixelFormatX24_Stencil8:
        return true;
    default:
        return false;
    }
}

} // namespace

Object CreateTexture(Device& device, const TextureDesc& desc, std::string_view label,
                     u64* allocated_size) {
    if (allocated_size) {
        *allocated_size = 0;
    }
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = TypeOf(desc.type);
    d.pixelFormat = MTLPixelFormat(desc.pixel_format);
    d.width = std::max(desc.width, 1u);
    d.height = std::max(desc.height, 1u);
    d.depth = std::max(desc.depth, 1u);
    d.mipmapLevelCount = std::max(desc.levels, 1u);
    d.arrayLength = std::max(desc.layers, 1u);
    d.sampleCount = std::max(desc.samples, 1u);
    d.storageMode = MTLStorageModePrivate;
    MTLTextureUsage usage = MTLTextureUsageShaderRead;
    if (desc.render_target) {
        usage |= MTLTextureUsageRenderTarget;
    }
    if (desc.shader_write) {
        usage |= MTLTextureUsageShaderWrite;
    }
    if (desc.shader_atomic) {
        usage |= MTLTextureUsageShaderAtomic;
    }
    if (desc.format_views) {
        usage |= MTLTextureUsagePixelFormatView;
    }
    d.usage = usage;
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLTexture> texture = [mtl_device newTextureWithDescriptor:d];
    if (texture == nil) {
        return {};
    }
    if (!label.empty()) {
        texture.label = [[NSString alloc] initWithBytes:label.data()
                                                 length:label.size()
                                               encoding:NSUTF8StringEncoding];
    }
    if (allocated_size) {
        *allocated_size = texture.allocatedSize;
    }
    return Object::Adopt((__bridge_retained void*)texture);
}

Object CreateTextureView(void* texture, u32 pixel_format, TextureType type, u32 base_level,
                         u32 levels, u32 base_layer, u32 layers, std::array<u8, 4> swizzle) {
    id<MTLTexture> base = (__bridge id<MTLTexture>)texture;
    if (base == nil) {
        return {};
    }
    const MTLTextureSwizzleChannels channels =
        MTLTextureSwizzleChannelsMake(MTLTextureSwizzle(swizzle[0]), MTLTextureSwizzle(swizzle[1]),
                                      MTLTextureSwizzle(swizzle[2]), MTLTextureSwizzle(swizzle[3]));
    id<MTLTexture> view = [base newTextureViewWithPixelFormat:MTLPixelFormat(pixel_format)
                                                  textureType:TypeOf(type)
                                                       levels:NSMakeRange(base_level, levels)
                                                       slices:NSMakeRange(base_layer, layers)
                                                      swizzle:channels];
    if (view == nil) {
        return {};
    }
    return Object::Adopt((__bridge_retained void*)view);
}

Object CreateSampler(Device& device, const SamplerDesc& desc) {
    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
    d.minFilter = MTLSamplerMinMagFilter(desc.min_filter);
    d.magFilter = MTLSamplerMinMagFilter(desc.mag_filter);
    d.mipFilter = MTLSamplerMipFilter(desc.mip_filter);
    d.sAddressMode = MTLSamplerAddressMode(desc.address_u);
    d.tAddressMode = MTLSamplerAddressMode(desc.address_v);
    d.rAddressMode = MTLSamplerAddressMode(desc.address_w);
    d.borderColor = MTLSamplerBorderColor(desc.border);
    d.lodMinClamp = desc.min_lod;
    d.lodMaxClamp = desc.max_lod;
    d.maxAnisotropy = std::clamp<NSUInteger>(desc.max_anisotropy, 1, 16);
    if (desc.compare) {
        d.compareFunction = MTLCompareFunction(desc.compare_function);
    }
    d.normalizedCoordinates = desc.normalized;
    if (desc.lod_bias != 0.0f) {
        // bbport: lodBias is declared by the macOS 26 SDK only (builds with older SDKs skip it).
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (@available(macOS 26.0, *)) {
            d.lodBias = desc.lod_bias;
        } else
#endif
        {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed)) {
                LOG_WARNING(Render, "sampler LOD bias is not applied (Metal samplers have none "
                                    "before macOS 26)");
            }
        }
    }
    d.supportArgumentBuffers = NO;
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLSamplerState> state = [mtl_device newSamplerStateWithDescriptor:d];
    if (state == nil) {
        return {};
    }
    return Object::Adopt((__bridge_retained void*)state);
}

u32 TexturePixelFormat(void* texture) {
    return u32(((__bridge id<MTLTexture>)texture).pixelFormat);
}

u32 TextureSampleCount(void* texture) {
    return u32(((__bridge id<MTLTexture>)texture).sampleCount);
}

bool SupportsSampleCount(Device& device, u32 samples) {
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device.NativeDevice();
    return [mtl_device supportsTextureSampleCount:samples];
}

u32 StencilViewPixelFormat(u32 pixel_format) {
    switch (MTLPixelFormat(pixel_format)) {
    case MTLPixelFormatDepth32Float_Stencil8:
        return MTLPixelFormatX32_Stencil8;
    case MTLPixelFormatDepth24Unorm_Stencil8:
        return MTLPixelFormatX24_Stencil8;
    case MTLPixelFormatStencil8:
        return MTLPixelFormatStencil8;
    default:
        return 0;
    }
}

u32 SafeViewPixelFormat(u32 texture_format, u32 requested_format, bool texture_has_format_views) {
    const auto texture = MTLPixelFormat(texture_format);
    const auto requested = MTLPixelFormat(requested_format);
    if (requested == texture || (requested != MTLPixelFormatInvalid && requested == SrgbTwin(texture))) {
        return requested_format;
    }
    if (requested == MTLPixelFormatInvalid) {
        return texture_format;
    }
    const Block texture_block = BlockOf(texture);
    const Block requested_block = BlockOf(requested);
    if (IsDepthOrStencil(texture)) {
        const u32 stencil = StencilViewPixelFormat(texture_format);
        return stencil != 0 && stencil != texture_format && requested_format == stencil
                   ? requested_format
                   : texture_format;
    }
    const bool compressed = texture_block.width > 1 || requested_block.width > 1;
    if (!texture_has_format_views || compressed || IsDepthOrStencil(requested) ||
        texture_block.bytes == 0 || texture_block.bytes != requested_block.bytes) {
        return texture_format;
    }
    return requested_format;
}

u32 BytesPerBlock(u32 pixel_format) {
    return BlockOf(MTLPixelFormat(pixel_format)).bytes;
}

u32 BlockWidth(u32 pixel_format) {
    return BlockOf(MTLPixelFormat(pixel_format)).width;
}

u32 BlockHeight(u32 pixel_format) {
    return BlockOf(MTLPixelFormat(pixel_format)).height;
}

bool IsIntegerPixelFormat(u32 pixel_format) {
    switch (MTLPixelFormat(pixel_format)) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatR16Uint:
    case MTLPixelFormatR16Sint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatRGBA32Sint:
        return true;
    default:
        return false;
    }
}

} // namespace Metal
