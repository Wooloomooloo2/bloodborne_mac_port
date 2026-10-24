// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include "bbport_toggles.h"
#include "common/assert.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/image.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

SceneTargets::SceneTargets(Metal::Device& d, Scheduler& s, Runtime& r,
                           VideoCore::TextureCache& t)
    : SceneTargets(d, s, r, [&t](VideoCore::ImageId id, u64 uid) {
        return t.TryGetImage(id, uid);
    }) {}
SceneTargets::SceneTargets(Metal::Device& d, Scheduler& s, Runtime& r, Lookup get)
    : device{d}, scheduler{s}, runtime{r}, lookup{std::move(get)}, scale{d} {
    runtime.scene_targets = this;
    const char* force = std::getenv("BB_SCENE_STENCIL_BITS");
    force_stencil_bits = force && force[0] == '1';
}
SceneTargets::~SceneTargets() {
    scheduler.Finish();
    runtime.scene_targets = nullptr;
}
bool SceneTargets::Blittable(const VideoCore::Image& image) const {
    // Every non-block colour texture is a render target (Image's texture description), so a
    // nearest-texel draw can scale it; same-size copies are plain texture copies.
    const auto& desc = image.backing->desc;
    return !image.info.props.is_depth && !image.info.props.is_block && desc.render_target &&
           desc.pixel_format != 0;
}
bool SceneTargets::ShaderResampled(const VideoCore::Image& image) const {
    // Depth formats (Stencil8 has no depth to read). Apple GPUs export stencil from fragment
    // shaders; BB_SCENE_STENCIL_BITS=1 tests the per-bit resampler instead.
    const auto& desc = image.backing->desc;
    return image.info.props.is_depth && desc.render_target &&
           Metal::VkToMtl::IsDepthPixelFormat(desc.pixel_format);
}
// 1 for the scene size, 2 for half resolution (the game's post-processing chain), else 0.
// Half resolution needs a scene size that halves exactly: the proxy keeps the scene's factor,
// so the shaders' FragCoord conversion (1920 / scene width) stays correct for it.
static u32 SizeDivisor(const VideoCore::ImageInfo& i, SceneResolution::Size scene) {
    if (i.size.width == 1920 && i.size.height == 1080) return 1;
    if (i.size.width == 960 && i.size.height == 540 && scene.width % 2 == 0 &&
        scene.height % 2 == 0 && !BbToggle::Disabled(BbToggle::SceneHalfRes)) {
        return 2;
    }
    return 0;
}
bool SceneTargets::Eligible(const VideoCore::Image& image) const {
    const auto& i = image.info;
    const u32 div = SizeDivisor(i, size);
    if (!div || i.size.depth != 1 || i.resources.layers != 1 || i.num_samples != 1 ||
        i.props.is_block || !image.backing || image.backing->num_samples != 1) {
        return false;
    }
    if (i.resources.levels != 1) {
        // Mip chains (the bloom pyramid at half resolution): one blitted proxy per level.
        return div == 2 && i.resources.levels <= 16 && !i.props.is_depth &&
               Blittable(image);
    }
    return Blittable(image) || ShaderResampled(image);
}
bool SceneTargets::EligibleScene(const VideoCore::Image& image) const {
    return image.info.size.width == 1920 && image.info.size.height == 1080 && Eligible(image);
}
SceneResolution::Size SceneTargets::ProxySize(const VideoCore::Image& image, u32 level) const {
    const u32 div = std::max(1u, SizeDivisor(image.info, size));
    return {std::max(1u, size.width / div >> level), std::max(1u, size.height / div >> level)};
}
bool SceneTargets::SetSize(SceneResolution::Size next) {
    if (next == size) return false;
    ResolveAll();
    scheduler.Finish();
    entries.clear(); // no command buffer can still reference these images/views
    tracked.clear();
    ++generation;
    recent = {};
    size = next;
    std::printf("Scene resolution: raster %ux%u, half-resolution targets %ux%u, UI 1920x1080 "
                "(live)\n", size.width, size.height, size.width / 2, size.height / 2);
    return true;
}
void SceneTargets::ResolveAll() {
    for (auto& [key, entry] : entries) {
        if (entry->state.dirty) {
            if (auto* image = lookup(entry->source, entry->uid)) Copy(*entry, *image, true);
        }
    }
}
void SceneTargets::NativeAccess(VideoCore::Image& image, bool write) {
    if (copying || !image.scene_proxy) return; // most images never had a proxy
    const u32 levels = std::min(16u, image.info.resources.levels);
    for (u32 level = 0; level < levels; ++level) {
        const auto it = entries.find(Key(image.image_uid, level));
        if (it == entries.end()) continue;
        auto& entry = *it->second;
        if (entry.state.dirty) Copy(entry, image, true);
        if (write) entry.state.NativeWrite();
    }
}
void SceneTargets::Copy(Entry& e, VideoCore::Image& original, bool to_native) {
    copying = true;
    scheduler.EndRendering();
    const Runtime::TransferMark mark{runtime, to_native ? "scene proxy resolve" : "scene proxy fill",
                                     original};
    if (debug) {
        std::printf("Scene %s: %s %ux%u level %u\n", to_native ? "resolve" : "fill",
                    vk::to_string(original.info.pixel_format).c_str(), original.info.size.width,
                    original.info.size.height, e.level);
    }
    // Joins the texture binding helper; NativeAccess skips the image (copying).
    runtime.AccessImage(&original, to_native);
    // bbport: the depth resample reads the stencil plane through a stencil view of its source,
    // which needs PixelFormatView: the native image (lazy usages) and the proxy get it first.
    if (ShaderResampled(original) &&
        Metal::VkToMtl::HasStencilPixelFormat(original.backing->desc.pixel_format)) {
        if (!original.backing->desc.format_views && original.allowed_format_views) {
            runtime.UpgradeUsage(&original, VideoCore::Image::UsageFormatViews);
        }
        if (!e.desc.format_views) {
            UpgradeProxy(e);
        }
    }
    void* const proxy = e.image.Get();
    void* const native = original.GetImage();
    void* const src = to_native ? proxy : native;
    void* const dst = to_native ? native : proxy;
    const u32 src_level = to_native ? 0u : e.level;
    const u32 dst_level = to_native ? e.level : 0u;
    const u32 native_width = std::max(1u, original.info.size.width >> e.level);
    const u32 native_height = std::max(1u, original.info.size.height >> e.level);
    const u32 dst_width = to_native ? native_width : e.width;
    const u32 dst_height = to_native ? native_height : e.height;
    if (native_width == e.width && native_height == e.height) {
        // Same size (all aspects of depth/stencil formats included).
        scheduler.Record([src, dst, src_level, dst_level, dst_width, dst_height](Metal::Encoder& enc) {
            enc.CopyTexture(src, src_level, 0, {}, dst, dst_level, 0, {},
                            {dst_width, dst_height, 1});
        });
    } else if (ShaderResampled(original)) {
        // (Depth proxies are single-level: Eligible.) Apple GPUs export stencil from fragment
        // shaders; BB_SCENE_STENCIL_BITS=1 tests the per-bit resampler instead.
        const bool has_stencil =
            Metal::VkToMtl::HasStencilPixelFormat(Metal::TexturePixelFormat(dst));
        const auto stencil = !has_stencil         ? Metal::StencilCopy::None
                             : force_stencil_bits ? Metal::StencilCopy::PerBit
                                                  : Metal::StencilCopy::Export;
        scheduler.Record([this, src, dst, stencil](Metal::Encoder& enc) {
            scale.ResampleDepth(enc, src, dst, stencil);
        });
        // The guest pipelines' dynamic state must be emitted again after this pipeline.
        scheduler.GetDynamicState().Invalidate();
    } else {
        // Nearest texel: preserves integer G-buffer formats; FSR reconstructs colour at the HDR
        // scene boundary.
        scheduler.Record([this, src, src_level, dst, dst_level](Metal::Encoder& enc) {
            scale.ScaleColor(enc, src, dst, Metal::ScaleFilter::Nearest, {}, src_level,
                             dst_level);
        });
        scheduler.GetDynamicState().Invalidate();
    }
    if (to_native) e.state.Resolved(); else e.state.CopiedToProxy();
    copying = false;
}
SceneTargets::Entry& SceneTargets::Get(VideoCore::ImageId id, u32 level) {
    auto& original = *lookup(id, 0);
    const u64 key = Key(original.image_uid, level);
    // A draw has up to six targets, mostly the same as the previous draw's.
    for (const auto& [recent_key, recent_entry] : recent) {
        if (recent_entry && recent_key == key) {
            if (!recent_entry->state.valid) Copy(*recent_entry, original, false);
            return *recent_entry;
        }
    }
    auto& entry = entries[key];
    original.scene_proxy = true;
    if (!entry) {
        entry = std::make_unique<Entry>();
        entry->source = id;
        entry->uid = original.image_uid;
        entry->level = level;
        const auto proxy = ProxySize(original, level);
        entry->width = proxy.width;
        entry->height = proxy.height;
        auto desc = original.backing->desc;
        desc.type = Metal::TextureType::Tex2D;
        desc.width = proxy.width;
        desc.height = proxy.height;
        desc.depth = 1;
        desc.levels = 1;
        desc.layers = 1;
        desc.samples = 1;
        desc.render_target = true;
        // bbport: proxies are only rendered to, copied and read (storage bindings use the native
        // image), so no ShaderWrite and Metal compresses them; PixelFormatView as the original's
        // backing has it, added by View when a view in another pixel format needs it.
        if (VideoCore::LazyTextureUsage()) {
            desc.shader_write = false;
        }
        entry->desc = desc;
        const auto label = fmt::format("scene proxy {:#x} level {}", original.info.guest_address,
                                       level);
        entry->image = Metal::CreateTexture(device, desc, label, nullptr);
        ASSERT_MSG(entry->image, "Scene proxy {}x{} of {} not created", proxy.width,
                   proxy.height, vk::to_string(original.info.pixel_format));
        tracked.insert(original.image_uid);
    }
    recent[recent_next++ % recent.size()] = {key, entry.get()};
    if (!entry->state.valid) Copy(*entry, original, false);
    return *entry;
}
void* SceneTargets::View(Entry& e, const VideoCore::Image& original,
                         const VideoCore::ImageViewInfo& info) {
    for (const auto& [key, view] : e.views) if (key == info) return view.Get();
    // Depth proxies are viewed whole (depth and stencil; samplers read the depth). Colour views
    // follow ImageView: the view's format where Metal can make it, with its swizzle.
    const u32 texture_format = original.backing->desc.pixel_format;
    u32 pixel_format = texture_format;
    vk::Format format = original.info.pixel_format;
    if (!original.info.props.is_depth) {
        const u32 requested = Metal::VkToMtl::PixelFormat(info.format);
        if (requested && !e.desc.format_views && original.allowed_format_views &&
            Metal::SafeViewPixelFormat(texture_format, requested, false) != requested &&
            Metal::SafeViewPixelFormat(texture_format, requested, true) == requested) {
            UpgradeProxy(e);
        }
        const u32 safe = requested ? Metal::SafeViewPixelFormat(texture_format, requested,
                                                                e.desc.format_views)
                                   : texture_format;
        if (safe == requested) {
            pixel_format = requested;
            format = info.format;
        }
    }
    const auto swizzle = original.info.props.is_depth
                             ? Metal::VkToMtl::IdentitySwizzle
                             : Metal::VkToMtl::ComposeSwizzle(info.mapping, format);
    Metal::Object view;
    if (pixel_format == texture_format && swizzle == Metal::VkToMtl::IdentitySwizzle) {
        view = Metal::Object::Retain(e.image.Get());
    } else {
        view = Metal::CreateTextureView(e.image.Get(), pixel_format, Metal::TextureType::Tex2D, 0,
                                        1, 0, 1, {swizzle.r, swizzle.g, swizzle.b, swizzle.a});
        ASSERT_MSG(view, "Scene proxy view of {} as {} not created",
                   vk::to_string(original.info.pixel_format), vk::to_string(info.format));
    }
    void* const result = view.Get();
    e.views.emplace_back(info, std::move(view));
    return result;
}
SceneTargets::Target SceneTargets::Attachment(VideoCore::ImageId id,
                                               const VideoCore::ImageViewInfo& info) {
    auto& original = *lookup(id, 0);
    auto& e = Get(id, info.range.base.level);
    e.state.ProxyWrite();
    return {View(e, original, info)};
}
bool SceneTargets::ProxyCurrent(const VideoCore::Image& image, u32 level, u32 levels) const {
    if (copying || !image.scene_proxy || image.info.props.is_depth || levels != 1) {
        return false;
    }
    const auto it = entries.find(Key(image.image_uid, level));
    return it != entries.end() && it->second->state.valid;
}
void SceneTargets::PrepareSample(const VideoCore::Image&, u32) {}
void SceneTargets::UpgradeProxy(Entry& e) {
    // The old texture and views live until the copy's command buffer completes.
    scheduler.EndRendering();
    e.desc.format_views = true;
    Metal::Object fresh = Metal::CreateTexture(device, e.desc, "scene proxy", nullptr);
    ASSERT_MSG(fresh, "Scene proxy {}x{} not created", e.width, e.height);
    std::vector<Metal::Object> retired;
    retired.push_back(std::move(e.image));
    for (auto& [key, view] : e.views) {
        retired.push_back(std::move(view));
    }
    e.views.clear();
    scheduler.Record([src = retired.front().Get(), dst = fresh.Get(), w = e.width, h = e.height,
                      retired = std::move(retired)](Metal::Encoder& enc) mutable {
        enc.CopyTexture(src, 0, 0, {}, dst, 0, 0, {}, {w, h, 1});
        for (auto& object : retired) {
            enc.KeepAlive(std::move(object));
        }
    });
    e.image = std::move(fresh);
    BbStats::texture_upgrades.fetch_add(1, std::memory_order_relaxed);
}
std::optional<SceneTargets::Target> SceneTargets::SampleProxy(
    const VideoCore::Image& image, const VideoCore::ImageViewInfo& info) {
    const u32 level = info.range.base.level;
    if (!ProxyCurrent(image, level, info.range.extent.levels)) {
        return std::nullopt;
    }
    auto& e = *entries.find(Key(image.image_uid, level))->second;
    return Target{View(e, image, info)};
}
SceneTargets::Target SceneTargets::Read(VideoCore::ImageId id,
                                       const VideoCore::ImageViewInfo& info) {
    auto& original = *lookup(id, 0);
    auto& e = Get(id);
    return {View(e, original, info)};
}
} // namespace Vulkan
