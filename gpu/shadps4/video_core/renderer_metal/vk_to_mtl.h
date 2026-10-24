// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

/// Vulkan enums are the shared vocabulary of the frontend, the caches and the pipeline keys
/// (docs/METAL_BACKEND_PLAN.md D2); the Metal backend maps them with these functions. They return
/// the numeric values of the Metal enums (MTLPixelFormat, MTLBlendFactor, ...), so this header
/// stays plain C++; vk_to_mtl.mm static_asserts the values against the real enums.
///
/// A value of 0 is "Invalid" only for the pixel and vertex formats. Enumerations whose 0 is a
/// real value (blend factor Zero, compare Never, cull None, ...) map unknown inputs to a safe
/// default documented at the function.
namespace Metal::VkToMtl {

/// MTLTextureSwizzle values (what MTLTextureSwizzleChannels takes).
inline constexpr u8 SwizzleZero = 0;
inline constexpr u8 SwizzleOne = 1;
inline constexpr u8 SwizzleRed = 2;
inline constexpr u8 SwizzleGreen = 3;
inline constexpr u8 SwizzleBlue = 4;
inline constexpr u8 SwizzleAlpha = 5;

/// Four MTLTextureSwizzle values: the texture channel each of r, g, b, a reads.
struct TextureSwizzle {
    u8 r, g, b, a;

    bool operator==(const TextureSwizzle&) const = default;
};

inline constexpr TextureSwizzle IdentitySwizzle{SwizzleRed, SwizzleGreen, SwizzleBlue,
                                                SwizzleAlpha};

// ---- Formats ----------------------------------------------------------------------------------

/// MTLPixelFormat for a texture or render target of the format, 0 (MTLPixelFormatInvalid) when
/// Metal has none: 3-channel formats (RGB8, RGB16, RGB32), 64-bit and scaled formats, A2B10G10R10
/// Snorm/Sint, A2R10G10B10 except Unorm, R4G4. Where Metal has only the swizzled twin (BGRA8
/// Snorm/Uint/Sint, B5G6R5, B5G5R5A1, B4G4R4A4, A4R4G4B4, A4B4G4R4) the twin is returned and
/// FormatSwizzle gives the swizzle; D24S8, D16S8 and X8D24 map to 32-bit depth (see
/// IsExactPixelFormat).
u32 PixelFormat(vk::Format format);

/// False when PixelFormat is a different-but-compatible format with another texel size or value
/// range (D24UnormS8 and D16UnormS8 -> Depth32Float_Stencil8, X8D24 -> Depth32Float), so that
/// memory copies between a guest layout and the texture need a conversion. True for every other
/// mapped format (a swizzle may still be needed).
bool IsExactPixelFormat(vk::Format format);

/// The swizzle Metal needs on top of the view's component mapping so that sampling the texture
/// of PixelFormat(format) returns the Vulkan format's (r, g, b, a). Identity for most formats;
/// BGRA-like twins swap r and b, BC1 without alpha forces alpha to one. Render targets cannot
/// apply it (the caller must treat those formats as sample-only or convert).
TextureSwizzle FormatSwizzle(vk::Format format);

/// The swizzle to put into an MTLTextureSwizzleChannels for a view with `view` over a texture of
/// `format`: the view's mapping composed with FormatSwizzle (eIdentity is the channel itself,
/// eZero/eOne stay constants).
TextureSwizzle ComposeSwizzle(const vk::ComponentMapping& view, vk::Format format);

/// MTLVertexFormat for a vertex attribute of the format, 0 (MTLVertexFormatInvalid) when Metal has
/// none (sRGB, 4/5/6-bit packed, B10G11R11, E5B9G9R9, A2B10G10R10 Uint/Sint/scaled, 64-bit, BGRA8
/// except Unorm). The Vulkan backend takes vertex attribute formats from the guest V# through
/// LiverpoolToVK::SurfaceFormat, so the buffer-capable subset of that table is what matters.
u32 VertexFormat(vk::Format format);

/// True for the depth formats (with or without stencil) and false for color and stencil-only.
bool IsDepthFormat(vk::Format format);
/// True when the format has a stencil aspect (S8 and the depth-stencil formats).
bool HasStencil(vk::Format format);
/// The same for a Metal pixel format value (MTLPixelFormatDepth16Unorm, Depth32Float,
/// Stencil8, Depth32Float_Stencil8, Depth24Unorm_Stencil8, X32_Stencil8, X24_Stencil8).
bool IsDepthPixelFormat(u32 mtl_pixel_format);
bool HasStencilPixelFormat(u32 mtl_pixel_format);

// ---- Blend, depth, stencil --------------------------------------------------------------------

/// MTLBlendFactor, including the dual-source factors (Source1Color, ...) and Constant{Color,Alpha}
/// -> BlendColor/BlendAlpha. Unknown values give Zero (0).
u32 BlendFactor(vk::BlendFactor factor);

/// MTLBlendOperation (Add, Subtract, ReverseSubtract, Min, Max). Unknown (advanced EXT blend
/// ops) give Add (0).
u32 BlendOp(vk::BlendOp op);

/// MTLColorWriteMask. The bit order differs (Metal: alpha 1, blue 2, green 4, red 8).
u32 ColorWriteMask(vk::ColorComponentFlags flags);

/// MTLCompareFunction. Used for the depth test and for sampler comparison. Unknown gives Always.
u32 CompareFunction(vk::CompareOp op);

/// MTLStencilOperation. Unknown gives Keep.
u32 StencilOp(vk::StencilOp op);

// ---- Primitives and rasterizer state ----------------------------------------------------------

struct PrimitiveInfo {
    u32 type;             ///< MTLPrimitiveType of the draw that is encoded
    bool needs_emulation; ///< Metal has no such topology: the draw is expanded by the backend
};

/// MTLPrimitiveType. needs_emulation (type is then the type of the expanded draw): triangle fans
/// (indices are rewritten to a triangle list), every *WithAdjacency list/strip (adjacent vertices
/// dropped; line list/strip, triangle list/strip) and patch lists, which is how the Vulkan
/// backend expresses RectList and QuadList (expanded to a triangle list).
PrimitiveInfo PrimitiveType(vk::PrimitiveTopology topology);

struct CullInfo {
    u32 mode;         ///< MTLCullMode
    bool cull_all;    ///< FrontAndBack: Metal has no such mode; nothing is rasterized
};

/// MTLCullMode. FrontAndBack gives {None, true}: the caller discards the primitives instead.
CullInfo CullMode(vk::CullModeFlags flags);

/// MTLWinding, straight from the Vulkan convention. The caller flips it when the vertex stage
/// inverts clip-space Y (MoltenVK does the same flip in mvkMTLWindingFromVkFrontFace).
u32 Winding(vk::FrontFace face);

struct FillInfo {
    u32 mode;             ///< MTLTriangleFillMode
    bool needs_emulation; ///< point polygon mode: Metal has none, draws filled
};

/// MTLTriangleFillMode: Fill -> Fill, Line -> Lines, Point -> {Fill, true}.
FillInfo PolygonMode(vk::PolygonMode mode);

struct IndexInfo {
    u32 type;              ///< MTLIndexType
    bool needs_conversion; ///< 8-bit indices: widened to UInt16 by the backend
};

/// MTLIndexType. Uint8 gives {UInt16, true}; NoneKHR gives {UInt16, false}.
IndexInfo IndexType(vk::IndexType type);

// ---- Samplers ---------------------------------------------------------------------------------

/// MTLSamplerMinMagFilter (Nearest, Linear).
u32 SamplerFilter(vk::Filter filter);

/// MTLSamplerMipFilter: Nearest, Linear. A texture without mips (or LOD clamped to its base)
/// uses SamplerMipNotMipmapped instead; the Vulkan side expresses that with the LOD range.
u32 MipFilter(vk::SamplerMipmapMode mode);
inline constexpr u32 SamplerMipNotMipmapped = 0; ///< MTLSamplerMipFilterNotMipmapped

/// MTLSamplerAddressMode, including MirrorClampToEdge and ClampToBorderColor.
u32 AddressMode(vk::SamplerAddressMode mode);

struct BorderInfo {
    u32 color;      ///< MTLSamplerBorderColor
    bool supported; ///< false for custom colors (fallback is TransparentBlack)
};

/// MTLSamplerBorderColor. Float and Int variants map to the same Metal color; the custom
/// variants give {TransparentBlack, false}.
BorderInfo BorderColor(vk::BorderColor color);

/// MTLCompareFunction for a comparison sampler (same as CompareFunction).
u32 SamplerCompare(vk::CompareOp op);

} // namespace Metal::VkToMtl
