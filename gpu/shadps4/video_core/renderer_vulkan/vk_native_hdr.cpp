// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_native_hdr.h"
#include "bbport_settings.h"
#include "common/assert.h"
#include "shader_recompiler/native_hdr.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/ui_composition.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>
namespace Vulkan {
NativeHdr::NativeHdr(Metal::Device& d, Scheduler& s) : device{d}, scheduler{s} {
    if (BbSettings::Get().startup_hdr)
        passes = std::make_unique<Metal::HdrPasses>(device);
}
NativeHdr::~NativeHdr() {
    if (passes)
        scheduler.Finish();
}
Metal::Object NativeHdr::Make(u32 format, u32 w, u32 h, const char* label) {
    auto image = Metal::CreateTexture(
        device,
        {.pixel_format = format,
         .width = w,
         .height = h,
         .render_target = true,
         .shader_write = format == Metal::VkToMtl::PixelFormat(vk::Format::eR16G16B16A16Sfloat),
         .format_views = true},
        label, nullptr);
    ASSERT_MSG(image, "Native HDR texture allocation failed");
    return image;
}
void NativeHdr::OnDraw(u64 vs, u64 ps) {
    if (!passes)
        return;
    if (Shader::NativeHdrToneShader(ps))
        ui_phase = false;
    // The last SDR glare composite is additive over the same target as Scaleform.
    // Capture it too, retaining the game's authored glare before the UI layers.
    if (scene_valid && (UiComposition::MovieShader(vs) || ps == 0x8c08444c))
        ui_phase = true;
}
void NativeHdr::Constants(u64 hash, const void* data, u64 size) {
    if (!passes || !Shader::NativeHdrToneShader(hash) || size < 852 * sizeof(float))
        return;
    const auto* f = static_cast<const float*>(data);
    if (!std::isfinite(f[808]) || f[808] < 0.1f || f[808] > 4.0f) {
        invalid = true;
        return;
    }
    params.gamma = f[808];
    if (hash == 0x5c02eaf8) {
        for (u32 i = 0; i < 12; ++i) {
            if (!std::isfinite(f[840 + i])) {
                invalid = true;
                return;
            }
            params.matrix[i] = f[840 + i];
        }
    }
}
void NativeHdr::GradingLut(void* texture) {
    if (passes && scene_valid && !ui_phase)
        lut = Metal::Object::Retain(texture);
}
void NativeHdr::GradingInput(bool rgb) {
    if (rgb)
        params.flags &= ~2u;
    else
        params.flags |= 2u;
}
void NativeHdr::Attach(RenderState& state) {
    if (!passes || !state.width || !state.height)
        return;
    if (width != state.width || height != state.height || !scene) {
        width = state.width;
        height = state.height;
        const auto format = Metal::VkToMtl::PixelFormat(vk::Format::eR16G16B16A16Sfloat);
        scene = Make(format, width, height, "HDR exposed lighting");
        bloom = Make(format, width, height, "HDR unclipped bloom");
    }
    state.num_colors = 3;
    state.colors[1] = {.texture = scene.Get(), .clear = true};
    state.colors[2] = {.texture = bloom.Get(), .clear = true};
    scene_valid = true;
    scheduler.Record([a = Metal::Object::Retain(scene.Get()),
                      b = Metal::Object::Retain(bloom.Get())](Metal::Encoder& e) mutable {
        e.KeepAlive(std::move(a));
        e.KeepAlive(std::move(b));
    });
}
bool NativeHdr::UiState(RenderState& state) {
    if (!InUi() || invalid || !state.num_colors)
        return false;
    if (state.num_colors != 1 || state.vertex_rw_buffer || !state.colors[0].texture) {
        invalid = true;
        return false;
    }
    if (ui_source && ui_source != state.colors[0].texture) {
        invalid = true;
        return false;
    }
    ui_source = state.colors[0].texture;
    if (Metal::TexturePixelFormat(ui_source) !=
        Metal::VkToMtl::PixelFormat(vk::Format::eR8G8B8A8Unorm)) {
        invalid = true;
        return false;
    }
    params.flags |= 1u; // Guest BGR exports, swapped by the original display copy.
    if (!ui || ui_width != state.width || ui_height != state.height) {
        if (ui_written) {
            invalid = true;
            return false;
        }
        ui_width = state.width;
        ui_height = state.height;
        const auto format = Metal::VkToMtl::PixelFormat(vk::Format::eR16G16B16A16Sfloat);
        ui = Make(format, ui_width, ui_height, "HDR UI black backdrop");
        ui_white = Make(format, ui_width, ui_height, "HDR UI white backdrop");
    }
    // One draw, three colour outputs, one shared depth/stencil attachment. No UI
    // replay, stencil copies or per-draw render-target switches are necessary.
    state.num_colors = 3;
    state.colors[1] = {.texture = ui.Get(), .clear = !ui_written};
    state.colors[2] = {.texture = ui_white.Get(), .clear = !ui_written};
    state.colors[2].clear_value.color.float32 = std::array<float, 4>{1, 1, 1, 0};
    if (!ui_written) {
        scheduler.Record([a = Metal::Object::Retain(ui.Get()),
                          b = Metal::Object::Retain(ui_white.Get())](Metal::Encoder& e) mutable {
            e.KeepAlive(std::move(a));
            e.KeepAlive(std::move(b));
        });
    }
    ui_written = true;
    return true;
}
void NativeHdr::OnDisplay(VAddr address) {
    if (!passes)
        return;
    // Resolve BEFORE reusing the lighting/UI textures for the next frame. Associate the
    // immutable result with the actual display buffer, not just whichever scene was last seen.
    auto& frame = frames[next_frame++ % frames.size()];
    if (frame.tick)
        scheduler.Wait(frame.tick);
    frame.valid = false;
    frame.address = address;
    if (scene_valid && ui_phase && !invalid && BbSettings::NativeHdrSupported()) {
        scheduler.EndRendering();
        if (!frame.texture || frame.w != width || frame.h != height) {
            frame.w = width;
            frame.h = height;
            frame.texture = Make(Metal::VkToMtl::PixelFormat(vk::Format::eR16G16B16A16Sfloat),
                                 width, height, "HDR resolved display frame");
        }
        auto p = params;
        p.headroom = BbSettings::Get().hdr_headroom;
        p.paper_white = BbSettings::Get().hdr_paper_white;
        p.peak = BbSettings::Get().hdr_peak;
        p.has_lut = bool(lut);
        p.has_ui = ui_written;
        scheduler.Record(
            [pass = passes.get(), a = Metal::Object::Retain(scene.Get()),
             b = Metal::Object::Retain(bloom.Get()), l = Metal::Object::Retain(lut.Get()),
             u = ui_written ? Metal::Object::Retain(ui.Get()) : Metal::Object{},
             white = ui_written ? Metal::Object::Retain(ui_white.Get()) : Metal::Object{},
             out = Metal::Object::Retain(frame.texture.Get()), p](Metal::Encoder& e) mutable {
                pass->ConvertNative(e, a.Get(), b.Get(), l.Get(), u.Get(), out.Get(), p,
                                    white.Get());
            });
        frame.tick = scheduler.CurrentTick();
        frame.valid = true;
        // Opt-in raw readbacks establish that the game supplies unclipped lighting,
        // rather than relying on an SDR diagnostic PNG to prove HDR.
        const char* trigger = std::getenv("BB_HDR_DUMP_TRIGGER");
        const char* dump_dir = std::getenv("BB_HDR_DUMP_DIR");
        if (trigger && dump_dir && std::remove(trigger) == 0) {
            std::filesystem::create_directories(dump_dir);
            const auto metadata = std::filesystem::path(dump_dir) / "parameters.json";
            if (auto* f = std::fopen(metadata.c_str(), "w")) {
                std::fprintf(f,
                             "{\"headroom\":%.6g,\"paper_white\":%.6g,\"peak\":%.6g,"
                             "\"gamma\":%.6g,\"has_lut\":%u,\"has_ui\":%u,\"flags\":%u}\n",
                             p.headroom, p.paper_white, p.peak, p.gamma, p.has_lut, p.has_ui,
                             p.flags);
                std::fclose(f);
            }
            const auto dump = [&](void* texture, const char* name) {
                const u32 row = width * 8;
                const u64 size = u64(row) * height;
                auto buffer = std::make_shared<VideoCore::Buffer>(
                    device, VideoCore::MemoryType::HostCached, 0, size, "Native HDR raw readback");
                scheduler.Record([src = Metal::Object::Retain(texture), buffer, row, size,
                                  w = width, h = height](Metal::Encoder& e) mutable {
                    e.CopyTextureToBuffer(src.Get(), 0, 0, {}, {w, h, 1}, buffer->Ref(), row,
                                          u32(size));
                    e.KeepAlive(std::move(src));
                });
                auto path = std::filesystem::path(dump_dir) /
                            (std::string(name) + "_" + std::to_string(width) + "x" +
                             std::to_string(height) + ".rgba16f.raw");
                scheduler.DeferPriorityOperation([buffer, path, size] {
                    if (auto* f = std::fopen(path.c_str(), "wb")) {
                        std::fwrite(buffer->mapped_data.data(), 1, size, f);
                        std::fclose(f);
                    }
                });
            };
            dump(scene.Get(), "lighting");
            dump(bloom.Get(), "bloom");
            dump(frame.texture.Get(), "hdr_output");
            std::printf("HDR: raw lighting/bloom/output capture %ux%u -> %s\n", width, height,
                        dump_dir);
        }
        BbSettings::Get().hdr_scene_active = true;
        static bool logged = false;
        if (!logged) {
            std::puts("HDR: native lighting, bloom, grading and separate UI resolved");
            logged = true;
        }
    } else {
        BbSettings::Get().hdr_scene_active = false;
    }
    scene_valid = ui_phase = ui_written = invalid = false;
    ui_source = nullptr;
    params = {};
    lut = {};
    scheduler.GetDynamicState().Invalidate();
}
Metal::Object NativeHdr::TakeFrame(VAddr address) {
    for (u32 n = 0; n < frames.size(); ++n) {
        auto& frame = frames[(next_frame + frames.size() - 1 - n) % frames.size()];
        if (frame.address == address) {
            const bool valid = std::exchange(frame.valid, false);
            if (valid) {
                static bool logged = false;
                if (!logged) {
                    std::puts("HDR: presenting matched native display frame");
                    logged = true;
                }
            }
            return valid ? Metal::Object::Retain(frame.texture.Get()) : Metal::Object{};
        }
    }
    return {};
}
} // namespace Vulkan
