// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

#import <Metal/Metal.h>

#include "video_core/renderer_metal/vk_to_mtl.h"

namespace Metal::VkToMtl {

static_assert(SwizzleZero == MTLTextureSwizzleZero && SwizzleOne == MTLTextureSwizzleOne &&
              SwizzleRed == MTLTextureSwizzleRed && SwizzleGreen == MTLTextureSwizzleGreen &&
              SwizzleBlue == MTLTextureSwizzleBlue && SwizzleAlpha == MTLTextureSwizzleAlpha);
static_assert(SamplerMipNotMipmapped == MTLSamplerMipFilterNotMipmapped);
static_assert(MTLPixelFormatInvalid == 0 && MTLVertexFormatInvalid == 0);

#define PF(name) static_cast<u32>(MTLPixelFormat##name)
#define VF(name) static_cast<u32>(MTLVertexFormat##name)

u32 PixelFormat(vk::Format format) {
    using F = vk::Format;
    switch (format) {
    // 8 bit
    case F::eR8Unorm:
        return PF(R8Unorm);
    case F::eR8Snorm:
        return PF(R8Snorm);
    case F::eR8Uint:
        return PF(R8Uint);
    case F::eR8Sint:
        return PF(R8Sint);
    case F::eR8Srgb:
        return PF(R8Unorm_sRGB);
    case F::eR8G8Unorm:
        return PF(RG8Unorm);
    case F::eR8G8Snorm:
        return PF(RG8Snorm);
    case F::eR8G8Uint:
        return PF(RG8Uint);
    case F::eR8G8Sint:
        return PF(RG8Sint);
    case F::eR8G8Srgb:
        return PF(RG8Unorm_sRGB);
    case F::eR8G8B8A8Unorm:
    case F::eA8B8G8R8UnormPack32:
        return PF(RGBA8Unorm);
    case F::eR8G8B8A8Snorm:
    case F::eA8B8G8R8SnormPack32:
        return PF(RGBA8Snorm);
    case F::eR8G8B8A8Uint:
    case F::eA8B8G8R8UintPack32:
        return PF(RGBA8Uint);
    case F::eR8G8B8A8Sint:
    case F::eA8B8G8R8SintPack32:
        return PF(RGBA8Sint);
    case F::eR8G8B8A8Srgb:
    case F::eA8B8G8R8SrgbPack32:
        return PF(RGBA8Unorm_sRGB);
    case F::eB8G8R8A8Unorm:
        return PF(BGRA8Unorm);
    case F::eB8G8R8A8Srgb:
        return PF(BGRA8Unorm_sRGB);
    // BGRA8 without a Metal twin: the RGBA8 format and a swizzle (FormatSwizzle).
    case F::eB8G8R8A8Snorm:
        return PF(RGBA8Snorm);
    case F::eB8G8R8A8Uint:
        return PF(RGBA8Uint);
    case F::eB8G8R8A8Sint:
        return PF(RGBA8Sint);
    // 16 bit
    case F::eR16Unorm:
        return PF(R16Unorm);
    case F::eR16Snorm:
        return PF(R16Snorm);
    case F::eR16Uint:
        return PF(R16Uint);
    case F::eR16Sint:
        return PF(R16Sint);
    case F::eR16Sfloat:
        return PF(R16Float);
    case F::eR16G16Unorm:
        return PF(RG16Unorm);
    case F::eR16G16Snorm:
        return PF(RG16Snorm);
    case F::eR16G16Uint:
        return PF(RG16Uint);
    case F::eR16G16Sint:
        return PF(RG16Sint);
    case F::eR16G16Sfloat:
        return PF(RG16Float);
    case F::eR16G16B16A16Unorm:
        return PF(RGBA16Unorm);
    case F::eR16G16B16A16Snorm:
        return PF(RGBA16Snorm);
    case F::eR16G16B16A16Uint:
        return PF(RGBA16Uint);
    case F::eR16G16B16A16Sint:
        return PF(RGBA16Sint);
    case F::eR16G16B16A16Sfloat:
        return PF(RGBA16Float);
    // 32 bit (no 3-channel texture format in Metal)
    case F::eR32Uint:
        return PF(R32Uint);
    case F::eR32Sint:
        return PF(R32Sint);
    case F::eR32Sfloat:
        return PF(R32Float);
    case F::eR32G32Uint:
        return PF(RG32Uint);
    case F::eR32G32Sint:
        return PF(RG32Sint);
    case F::eR32G32Sfloat:
        return PF(RG32Float);
    case F::eR32G32B32A32Uint:
        return PF(RGBA32Uint);
    case F::eR32G32B32A32Sint:
        return PF(RGBA32Sint);
    case F::eR32G32B32A32Sfloat:
        return PF(RGBA32Float);
    // Packed
    case F::eA2B10G10R10UnormPack32:
        return PF(RGB10A2Unorm);
    case F::eA2B10G10R10UintPack32:
        return PF(RGB10A2Uint);
    case F::eA2R10G10B10UnormPack32:
        return PF(BGR10A2Unorm);
    case F::eB10G11R11UfloatPack32:
        return PF(RG11B10Float);
    case F::eE5B9G9R9UfloatPack32:
        return PF(RGB9E5Float);
    // 16 bit packed. Metal names list the components from the least significant bit, Vulkan from
    // the most significant: R5G6B5 = B5G6R5, R5G5B5A1 = A1BGR5, A1R5G5B5 = BGR5A1,
    // R4G4B4A4 = ABGR4 are exact. The reversed ones use the same Metal format with a swizzle.
    case F::eR5G6B5UnormPack16:
    case F::eB5G6R5UnormPack16:
        return PF(B5G6R5Unorm);
    case F::eR5G5B5A1UnormPack16:
    case F::eB5G5R5A1UnormPack16:
        return PF(A1BGR5Unorm);
    case F::eA1R5G5B5UnormPack16:
        return PF(BGR5A1Unorm);
    case F::eR4G4B4A4UnormPack16:
    case F::eB4G4R4A4UnormPack16:
    case F::eA4R4G4B4UnormPack16:
    case F::eA4B4G4R4UnormPack16:
        return PF(ABGR4Unorm);
    // Block compressed
    case F::eBc1RgbUnormBlock:
    case F::eBc1RgbaUnormBlock:
        return PF(BC1_RGBA);
    case F::eBc1RgbSrgbBlock:
    case F::eBc1RgbaSrgbBlock:
        return PF(BC1_RGBA_sRGB);
    case F::eBc2UnormBlock:
        return PF(BC2_RGBA);
    case F::eBc2SrgbBlock:
        return PF(BC2_RGBA_sRGB);
    case F::eBc3UnormBlock:
        return PF(BC3_RGBA);
    case F::eBc3SrgbBlock:
        return PF(BC3_RGBA_sRGB);
    case F::eBc4UnormBlock:
        return PF(BC4_RUnorm);
    case F::eBc4SnormBlock:
        return PF(BC4_RSnorm);
    case F::eBc5UnormBlock:
        return PF(BC5_RGUnorm);
    case F::eBc5SnormBlock:
        return PF(BC5_RGSnorm);
    case F::eBc6HUfloatBlock:
        return PF(BC6H_RGBUfloat);
    case F::eBc6HSfloatBlock:
        return PF(BC6H_RGBFloat);
    case F::eBc7UnormBlock:
        return PF(BC7_RGBAUnorm);
    case F::eBc7SrgbBlock:
        return PF(BC7_RGBAUnorm_sRGB);
    // Depth and stencil. Apple GPUs have no 24-bit depth and no D16S8.
    case F::eD16Unorm:
        return PF(Depth16Unorm);
    case F::eD32Sfloat:
    case F::eX8D24UnormPack32:
        return PF(Depth32Float);
    case F::eS8Uint:
        return PF(Stencil8);
    case F::eD16UnormS8Uint:
    case F::eD24UnormS8Uint:
    case F::eD32SfloatS8Uint:
        return PF(Depth32Float_Stencil8);
    default:
        return PF(Invalid);
    }
}

bool IsExactPixelFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eX8D24UnormPack32:
        return false;
    default:
        return true;
    }
}

TextureSwizzle FormatSwizzle(vk::Format format) {
    constexpr TextureSwizzle bgra{SwizzleBlue, SwizzleGreen, SwizzleRed, SwizzleAlpha};
    switch (format) {
    case vk::Format::eB8G8R8A8Snorm:
    case vk::Format::eB8G8R8A8Uint:
    case vk::Format::eB8G8R8A8Sint:
    case vk::Format::eB5G6R5UnormPack16:
    case vk::Format::eB5G5R5A1UnormPack16:
    case vk::Format::eB4G4R4A4UnormPack16:
        return bgra;
    case vk::Format::eA4R4G4B4UnormPack16:
        // ABGR4 reads (A, R, G, B) as (r, g, b, a).
        return {SwizzleGreen, SwizzleBlue, SwizzleAlpha, SwizzleRed};
    case vk::Format::eA4B4G4R4UnormPack16:
        // ABGR4 reads (A, B, G, R) as (r, g, b, a).
        return {SwizzleAlpha, SwizzleBlue, SwizzleGreen, SwizzleRed};
    case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc1RgbSrgbBlock:
        // BC1_RGBA has punch-through alpha; the RGB variants read alpha as one.
        return {SwizzleRed, SwizzleGreen, SwizzleBlue, SwizzleOne};
    default:
        return IdentitySwizzle;
    }
}

TextureSwizzle ComposeSwizzle(const vk::ComponentMapping& view, vk::Format format) {
    const TextureSwizzle fmt = FormatSwizzle(format);
    const auto channel = [&](vk::ComponentSwizzle swizzle, u8 self) -> u8 {
        switch (swizzle) {
        case vk::ComponentSwizzle::eZero:
            return SwizzleZero;
        case vk::ComponentSwizzle::eOne:
            return SwizzleOne;
        case vk::ComponentSwizzle::eR:
            return fmt.r;
        case vk::ComponentSwizzle::eG:
            return fmt.g;
        case vk::ComponentSwizzle::eB:
            return fmt.b;
        case vk::ComponentSwizzle::eA:
            return fmt.a;
        case vk::ComponentSwizzle::eIdentity:
        default:
            // The channel itself: read through the format's swizzle.
            return self == SwizzleRed     ? fmt.r
                   : self == SwizzleGreen ? fmt.g
                   : self == SwizzleBlue  ? fmt.b
                                          : fmt.a;
        }
    };
    return {channel(view.r, SwizzleRed), channel(view.g, SwizzleGreen),
            channel(view.b, SwizzleBlue), channel(view.a, SwizzleAlpha)};
}

u32 VertexFormat(vk::Format format) {
    using F = vk::Format;
    switch (format) {
    // 8 bit. Scaled formats (integer value converted to float without normalization) and Uint
    // are the same Metal format: the shader's attribute type decides float or integer.
    case F::eR8Unorm:
        return VF(UCharNormalized);
    case F::eR8Snorm:
        return VF(CharNormalized);
    case F::eR8Uint:
    case F::eR8Uscaled:
        return VF(UChar);
    case F::eR8Sint:
    case F::eR8Sscaled:
        return VF(Char);
    case F::eR8G8Unorm:
        return VF(UChar2Normalized);
    case F::eR8G8Snorm:
        return VF(Char2Normalized);
    case F::eR8G8Uint:
    case F::eR8G8Uscaled:
        return VF(UChar2);
    case F::eR8G8Sint:
    case F::eR8G8Sscaled:
        return VF(Char2);
    case F::eR8G8B8Unorm:
        return VF(UChar3Normalized);
    case F::eR8G8B8Snorm:
        return VF(Char3Normalized);
    case F::eR8G8B8Uint:
    case F::eR8G8B8Uscaled:
        return VF(UChar3);
    case F::eR8G8B8Sint:
    case F::eR8G8B8Sscaled:
        return VF(Char3);
    case F::eR8G8B8A8Unorm:
    case F::eA8B8G8R8UnormPack32:
        return VF(UChar4Normalized);
    case F::eR8G8B8A8Snorm:
    case F::eA8B8G8R8SnormPack32:
        return VF(Char4Normalized);
    case F::eR8G8B8A8Uint:
    case F::eR8G8B8A8Uscaled:
    case F::eA8B8G8R8UintPack32:
    case F::eA8B8G8R8UscaledPack32:
        return VF(UChar4);
    case F::eR8G8B8A8Sint:
    case F::eR8G8B8A8Sscaled:
    case F::eA8B8G8R8SintPack32:
    case F::eA8B8G8R8SscaledPack32:
        return VF(Char4);
    case F::eB8G8R8A8Unorm:
        return VF(UChar4Normalized_BGRA);
    // 16 bit
    case F::eR16Unorm:
        return VF(UShortNormalized);
    case F::eR16Snorm:
        return VF(ShortNormalized);
    case F::eR16Uint:
    case F::eR16Uscaled:
        return VF(UShort);
    case F::eR16Sint:
    case F::eR16Sscaled:
        return VF(Short);
    case F::eR16Sfloat:
        return VF(Half);
    case F::eR16G16Unorm:
        return VF(UShort2Normalized);
    case F::eR16G16Snorm:
        return VF(Short2Normalized);
    case F::eR16G16Uint:
    case F::eR16G16Uscaled:
        return VF(UShort2);
    case F::eR16G16Sint:
    case F::eR16G16Sscaled:
        return VF(Short2);
    case F::eR16G16Sfloat:
        return VF(Half2);
    case F::eR16G16B16Unorm:
        return VF(UShort3Normalized);
    case F::eR16G16B16Snorm:
        return VF(Short3Normalized);
    case F::eR16G16B16Uint:
    case F::eR16G16B16Uscaled:
        return VF(UShort3);
    case F::eR16G16B16Sint:
    case F::eR16G16B16Sscaled:
        return VF(Short3);
    case F::eR16G16B16Sfloat:
        return VF(Half3);
    case F::eR16G16B16A16Unorm:
        return VF(UShort4Normalized);
    case F::eR16G16B16A16Snorm:
        return VF(Short4Normalized);
    case F::eR16G16B16A16Uint:
    case F::eR16G16B16A16Uscaled:
        return VF(UShort4);
    case F::eR16G16B16A16Sint:
    case F::eR16G16B16A16Sscaled:
        return VF(Short4);
    case F::eR16G16B16A16Sfloat:
        return VF(Half4);
    // 32 bit
    case F::eR32Uint:
        return VF(UInt);
    case F::eR32Sint:
        return VF(Int);
    case F::eR32Sfloat:
        return VF(Float);
    case F::eR32G32Uint:
        return VF(UInt2);
    case F::eR32G32Sint:
        return VF(Int2);
    case F::eR32G32Sfloat:
        return VF(Float2);
    case F::eR32G32B32Uint:
        return VF(UInt3);
    case F::eR32G32B32Sint:
        return VF(Int3);
    case F::eR32G32B32Sfloat:
        return VF(Float3);
    case F::eR32G32B32A32Uint:
        return VF(UInt4);
    case F::eR32G32B32A32Sint:
        return VF(Int4);
    case F::eR32G32B32A32Sfloat:
        return VF(Float4);
    // Packed: only the normalized 10-10-10-2 with red in the low bits has a Metal twin.
    case F::eA2B10G10R10UnormPack32:
        return VF(UInt1010102Normalized);
    case F::eA2B10G10R10SnormPack32:
        return VF(Int1010102Normalized);
    default:
        return VF(Invalid);
    }
}

bool IsDepthFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
    case vk::Format::eX8D24UnormPack32:
        return true;
    default:
        return false;
    }
}

bool HasStencil(vk::Format format) {
    switch (format) {
    case vk::Format::eS8Uint:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return true;
    default:
        return false;
    }
}

bool IsDepthPixelFormat(u32 format) {
    switch (static_cast<MTLPixelFormat>(format)) {
    case MTLPixelFormatDepth16Unorm:
    case MTLPixelFormatDepth32Float:
    case MTLPixelFormatDepth24Unorm_Stencil8:
    case MTLPixelFormatDepth32Float_Stencil8:
        return true;
    default:
        return false;
    }
}

bool HasStencilPixelFormat(u32 format) {
    switch (static_cast<MTLPixelFormat>(format)) {
    case MTLPixelFormatStencil8:
    case MTLPixelFormatDepth24Unorm_Stencil8:
    case MTLPixelFormatDepth32Float_Stencil8:
    case MTLPixelFormatX32_Stencil8:
    case MTLPixelFormatX24_Stencil8:
        return true;
    default:
        return false;
    }
}

u32 BlendFactor(vk::BlendFactor factor) {
    using B = vk::BlendFactor;
    switch (factor) {
    case B::eZero:
        return MTLBlendFactorZero;
    case B::eOne:
        return MTLBlendFactorOne;
    case B::eSrcColor:
        return MTLBlendFactorSourceColor;
    case B::eOneMinusSrcColor:
        return MTLBlendFactorOneMinusSourceColor;
    case B::eDstColor:
        return MTLBlendFactorDestinationColor;
    case B::eOneMinusDstColor:
        return MTLBlendFactorOneMinusDestinationColor;
    case B::eSrcAlpha:
        return MTLBlendFactorSourceAlpha;
    case B::eOneMinusSrcAlpha:
        return MTLBlendFactorOneMinusSourceAlpha;
    case B::eDstAlpha:
        return MTLBlendFactorDestinationAlpha;
    case B::eOneMinusDstAlpha:
        return MTLBlendFactorOneMinusDestinationAlpha;
    case B::eConstantColor:
        return MTLBlendFactorBlendColor;
    case B::eOneMinusConstantColor:
        return MTLBlendFactorOneMinusBlendColor;
    case B::eConstantAlpha:
        return MTLBlendFactorBlendAlpha;
    case B::eOneMinusConstantAlpha:
        return MTLBlendFactorOneMinusBlendAlpha;
    case B::eSrcAlphaSaturate:
        return MTLBlendFactorSourceAlphaSaturated;
    case B::eSrc1Color:
        return MTLBlendFactorSource1Color;
    case B::eOneMinusSrc1Color:
        return MTLBlendFactorOneMinusSource1Color;
    case B::eSrc1Alpha:
        return MTLBlendFactorSource1Alpha;
    case B::eOneMinusSrc1Alpha:
        return MTLBlendFactorOneMinusSource1Alpha;
    default:
        return MTLBlendFactorZero;
    }
}

u32 BlendOp(vk::BlendOp op) {
    switch (op) {
    case vk::BlendOp::eAdd:
        return MTLBlendOperationAdd;
    case vk::BlendOp::eSubtract:
        return MTLBlendOperationSubtract;
    case vk::BlendOp::eReverseSubtract:
        return MTLBlendOperationReverseSubtract;
    case vk::BlendOp::eMin:
        return MTLBlendOperationMin;
    case vk::BlendOp::eMax:
        return MTLBlendOperationMax;
    default:
        return MTLBlendOperationAdd;
    }
}

u32 ColorWriteMask(vk::ColorComponentFlags flags) {
    u32 mask = MTLColorWriteMaskNone;
    if (flags & vk::ColorComponentFlagBits::eR) {
        mask |= MTLColorWriteMaskRed;
    }
    if (flags & vk::ColorComponentFlagBits::eG) {
        mask |= MTLColorWriteMaskGreen;
    }
    if (flags & vk::ColorComponentFlagBits::eB) {
        mask |= MTLColorWriteMaskBlue;
    }
    if (flags & vk::ColorComponentFlagBits::eA) {
        mask |= MTLColorWriteMaskAlpha;
    }
    return mask;
}

u32 CompareFunction(vk::CompareOp op) {
    switch (op) {
    case vk::CompareOp::eNever:
        return MTLCompareFunctionNever;
    case vk::CompareOp::eLess:
        return MTLCompareFunctionLess;
    case vk::CompareOp::eEqual:
        return MTLCompareFunctionEqual;
    case vk::CompareOp::eLessOrEqual:
        return MTLCompareFunctionLessEqual;
    case vk::CompareOp::eGreater:
        return MTLCompareFunctionGreater;
    case vk::CompareOp::eNotEqual:
        return MTLCompareFunctionNotEqual;
    case vk::CompareOp::eGreaterOrEqual:
        return MTLCompareFunctionGreaterEqual;
    case vk::CompareOp::eAlways:
    default:
        return MTLCompareFunctionAlways;
    }
}

u32 StencilOp(vk::StencilOp op) {
    switch (op) {
    case vk::StencilOp::eKeep:
        return MTLStencilOperationKeep;
    case vk::StencilOp::eZero:
        return MTLStencilOperationZero;
    case vk::StencilOp::eReplace:
        return MTLStencilOperationReplace;
    case vk::StencilOp::eIncrementAndClamp:
        return MTLStencilOperationIncrementClamp;
    case vk::StencilOp::eDecrementAndClamp:
        return MTLStencilOperationDecrementClamp;
    case vk::StencilOp::eInvert:
        return MTLStencilOperationInvert;
    case vk::StencilOp::eIncrementAndWrap:
        return MTLStencilOperationIncrementWrap;
    case vk::StencilOp::eDecrementAndWrap:
        return MTLStencilOperationDecrementWrap;
    default:
        return MTLStencilOperationKeep;
    }
}

PrimitiveInfo PrimitiveType(vk::PrimitiveTopology topology) {
    using T = vk::PrimitiveTopology;
    switch (topology) {
    case T::ePointList:
        return {MTLPrimitiveTypePoint, false};
    case T::eLineList:
        return {MTLPrimitiveTypeLine, false};
    case T::eLineStrip:
        return {MTLPrimitiveTypeLineStrip, false};
    case T::eTriangleList:
        return {MTLPrimitiveTypeTriangle, false};
    case T::eTriangleStrip:
        return {MTLPrimitiveTypeTriangleStrip, false};
    case T::eTriangleFan:
        return {MTLPrimitiveTypeTriangle, true};
    case T::eLineListWithAdjacency:
        return {MTLPrimitiveTypeLine, true};
    case T::eLineStripWithAdjacency:
        return {MTLPrimitiveTypeLineStrip, true};
    case T::eTriangleListWithAdjacency:
        return {MTLPrimitiveTypeTriangle, true};
    case T::eTriangleStripWithAdjacency:
        return {MTLPrimitiveTypeTriangleStrip, true};
    case T::ePatchList:
    default:
        return {MTLPrimitiveTypeTriangle, true};
    }
}

CullInfo CullMode(vk::CullModeFlags flags) {
    const bool front = bool(flags & vk::CullModeFlagBits::eFront);
    const bool back = bool(flags & vk::CullModeFlagBits::eBack);
    if (front && back) {
        return {MTLCullModeNone, true};
    }
    if (front) {
        return {MTLCullModeFront, false};
    }
    if (back) {
        return {MTLCullModeBack, false};
    }
    return {MTLCullModeNone, false};
}

u32 Winding(vk::FrontFace face) {
    return face == vk::FrontFace::eClockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
}

FillInfo PolygonMode(vk::PolygonMode mode) {
    switch (mode) {
    case vk::PolygonMode::eLine:
        return {MTLTriangleFillModeLines, false};
    case vk::PolygonMode::ePoint:
        return {MTLTriangleFillModeFill, true};
    case vk::PolygonMode::eFill:
    default:
        return {MTLTriangleFillModeFill, false};
    }
}

IndexInfo IndexType(vk::IndexType type) {
    switch (type) {
    case vk::IndexType::eUint32:
        return {MTLIndexTypeUInt32, false};
    case vk::IndexType::eUint8:
        return {MTLIndexTypeUInt16, true};
    case vk::IndexType::eUint16:
    default:
        return {MTLIndexTypeUInt16, false};
    }
}

u32 SamplerFilter(vk::Filter filter) {
    return filter == vk::Filter::eNearest ? MTLSamplerMinMagFilterNearest
                                          : MTLSamplerMinMagFilterLinear;
}

u32 MipFilter(vk::SamplerMipmapMode mode) {
    return mode == vk::SamplerMipmapMode::eNearest ? MTLSamplerMipFilterNearest
                                                   : MTLSamplerMipFilterLinear;
}

u32 AddressMode(vk::SamplerAddressMode mode) {
    switch (mode) {
    case vk::SamplerAddressMode::eRepeat:
        return MTLSamplerAddressModeRepeat;
    case vk::SamplerAddressMode::eMirroredRepeat:
        return MTLSamplerAddressModeMirrorRepeat;
    case vk::SamplerAddressMode::eClampToBorder:
        return MTLSamplerAddressModeClampToBorderColor;
    case vk::SamplerAddressMode::eMirrorClampToEdge:
        return MTLSamplerAddressModeMirrorClampToEdge;
    case vk::SamplerAddressMode::eClampToEdge:
    default:
        return MTLSamplerAddressModeClampToEdge;
    }
}

BorderInfo BorderColor(vk::BorderColor color) {
    switch (color) {
    case vk::BorderColor::eFloatOpaqueBlack:
    case vk::BorderColor::eIntOpaqueBlack:
        return {MTLSamplerBorderColorOpaqueBlack, true};
    case vk::BorderColor::eFloatOpaqueWhite:
    case vk::BorderColor::eIntOpaqueWhite:
        return {MTLSamplerBorderColorOpaqueWhite, true};
    case vk::BorderColor::eFloatTransparentBlack:
    case vk::BorderColor::eIntTransparentBlack:
        return {MTLSamplerBorderColorTransparentBlack, true};
    default:
        return {MTLSamplerBorderColorTransparentBlack, false};
    }
}

u32 SamplerCompare(vk::CompareOp op) {
    return CompareFunction(op);
}

#undef PF
#undef VF

} // namespace Metal::VkToMtl
