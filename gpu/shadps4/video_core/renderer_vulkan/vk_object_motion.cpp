// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_object_motion.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_metal/mtl_buffer.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

ObjectMotion::ObjectMotion(Metal::Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {
    // Pipeline selection limits the extra attachment and vertex stores to likely
    // animated draws. A setting or environment override can still disable the path.
    if (!BbSettings::Get().object_motion ||
        BbSettings::Get().upscaler == BbSettings::UpscalerOff) {
        std::puts("Object motion: off (enable in menu or BB_OBJECT_MOTION=1)");
        return;
    }
    u8* mapped = nullptr;
    u8* unused = nullptr;
    u64 params_address = 0, positions_address = 0;
    params_buffer = Metal::CreateBuffer(device, u64(1 + FrameSlots * ParamsPerFrame) * 32,
                                        VideoCore::MemoryType::HostUncached, "ObjectMotionParams",
                                        &params_address, &mapped);
    positions_buffer = Metal::CreateBuffer(device, u64(1 + 2 * PositionsPerFrame) * 16,
                                           VideoCore::MemoryType::DeviceLocal,
                                           "ObjectMotionPositions", &positions_address, &unused);
    if (!params_buffer || !positions_buffer || !mapped || !params_address || !positions_address) {
        params_buffer = {};
        positions_buffer = {};
        std::printf("Object motion: buffer creation failed, off\n");
        return;
    }
    params_mapped = reinterpret_cast<u32*>(mapped);
    std::memset(params_mapped, 0, 32);
    // Shaders reach both buffers only through the addresses embedded in the motion variants
    // (Shader::MotionVectors), never through a binding, so Metal neither makes them resident
    // nor orders the passes that use them on its own:
    //  - residency: the queue's residency set (MakeResident); parameters are also declared
    //    (useResource, read) on every encoder the scheduler's Encoder opens (DeclareIndirect);
    //  - order: positions are written by this frame's vertex shaders (current half, each draw
    //    its own range) and read by the next frame's (previous half), and the half read this
    //    frame is written two frames on. Only the passes of motion pipelines use them
    //    (Attach: RenderState::vertex_rw_buffer). The positions buffer is device-local, so
    //    untracked (mtl_buffer.mm): those passes' draws are HazardClass::MotionHistory accesses
    //    that do not wait for each other within a frame, and OnFrameStart's OrderBarrier orders
    //    one frame's after the last frame's, as the Vulkan vertex-to-vertex barrier at frame
    //    start did. With BB_METAL_HAZARD_TRACKING=1 it is tracked and declared read/write at
    //    the vertex stage on those passes, so hazard tracking orders them (all of them).
    //    Other passes and dispatches do not wait on it either way.
    //    Parameters are written by the CPU before the command buffer that reads them is
    //    committed and only read on the GPU (FrameSlots ring, params_ticks).
    device.MakeResident(params_buffer.Get());
    device.MakeResident(positions_buffer.Get());
    scheduler.Record([params = params_buffer.Get()](Metal::Encoder& e) {
        e.DeclareIndirect(params);
    });
    Shader::MotionVectors::params_address = params_address;
    Shader::MotionVectors::positions_address = positions_address;
    enabled = true;
    std::printf("Object motion: on (%u vertices per frame)\n", PositionsPerFrame);
}

ObjectMotion::~ObjectMotion() {
    if (params_buffer) {
        scheduler.Record([params = params_buffer.Get()](Metal::Encoder& e) {
            e.RemoveIndirect(params);
        });
    }
    scheduler.Finish();
    if (params_buffer) {
        device.Evict(params_buffer.Get());
        device.Evict(positions_buffer.Get());
    }
}

void ObjectMotion::OnFrameStart() {
    if (!enabled) {
        return;
    }
    if (BbStats::enabled && (frame % 600) == 0 && history.stats.draws) {
        const auto& s = history.stats;
        std::printf("Object motion: %llu draws, %llu stored, %llu with history, %llu unmatched, "
                    "%llu capacity skips, %llu invalid, %llu still (600 frames); last frame %u/%u vertices; "
                    "index ranges %llu reused, %llu scanned (%llu found changed), %zu cached\n",
                    (unsigned long long)s.draws, (unsigned long long)s.stored,
                    (unsigned long long)s.loaded, (unsigned long long)s.unmatched,
                    (unsigned long long)s.exhausted, (unsigned long long)s.invalid,
                    (unsigned long long)s.still,
                    history.Used(), PositionsPerFrame, (unsigned long long)index_ranges.stats.hits,
                    (unsigned long long)index_ranges.stats.scans,
                    (unsigned long long)index_ranges.stats.stale, index_ranges.Size());
        index_ranges.stats = {};
        history.stats = {};
    }
    history.NextFrame();
    ++frame;
    if (frame % Motion::IndexRangeCache::Unused == 0) {
        index_ranges.Trim(frame);
    }
    const u32 slot = u32(frame % FrameSlots);
    // Do not overwrite host parameters still referenced by queued GPU work.
    if (params_ticks[slot]) {
        scheduler.Wait(params_ticks[slot]);
        params_ticks[slot] = 0;
    }
    params_used = 0;
    written = false;
    // Last frame's position stores before this frame's loads, and this frame's stores after the
    // loads two frames back (constructor): a phase boundary in the motion history's ordering.
    scheduler.Record([](Metal::Encoder& e) { e.OrderBarrier(Metal::HazardClass::MotionHistory); });
}

u32 ObjectMotion::PrepareDraw(const DrawInfo& draw) {
    if (!enabled || BbToggle::Disabled(1u << 29) || params_used >= ParamsPerFrame) {
        return 0;
    }
    const auto allocation = history.Prepare(draw);
    u32 flags = (allocation.store ? Shader::MotionVectors::FlagStore : 0) |
                (allocation.load ? Shader::MotionVectors::FlagLoad : 0);
    if (!flags) {
        return 0;
    }
    const u32 slot = u32(frame % FrameSlots);
    const u32 index = 1 + slot * ParamsPerFrame + params_used++;
    u32* param = params_mapped + index * 8;
    param[0] = allocation.store;
    param[1] = allocation.load;
    param[2] = allocation.vertices;
    param[3] = flags;
    param[4] = allocation.first_vertex;
    param[5] = allocation.first_instance;
    param[6] = allocation.instances;
    param[7] = 0;
    params_ticks[slot] = scheduler.CurrentTick();
    return index;
}

void ObjectMotion::EnsureImage(u32 width, u32 height) {
    if (image && width == image_width && height == image_height) {
        return;
    }
    if (image) {
        // Recorded passes and the compose pass may still use the old texture.
        scheduler.Finish();
    }
    image = Metal::CreateTexture(
        device,
        Metal::TextureDesc{
            .pixel_format = Metal::VkToMtl::PixelFormat(vk::Format::eR32G32B32A32Sfloat),
            .width = width,
            .height = height,
            .render_target = true,
        },
        "ObjectMotion", nullptr);
    ASSERT_MSG(image, "Object motion: {}x{} motion texture creation failed", width, height);
    image_width = width;
    image_height = height;
    written = false;
}

void ObjectMotion::Attach(RenderState& state, u32 width, u32 height) {
    constexpr u32 slot = Shader::MotionVectors::Output;
    EnsureImage(width, height);
    auto& attachment = state.colors[slot];
    attachment = {};
    attachment.texture = image.Get();
    // Clear (to zero: no motion) once per frame; later passes load, so a read in between does
    // not erase the vectors already written.
    attachment.clear = !written;
    written = true;
    state.num_colors = std::max(state.num_colors, slot + 1);
    // The motion pipelines' position history (see the constructor).
    state.vertex_rw_buffer = positions_buffer.Get();
}

void* ObjectMotion::PrepareRead(u32 width, u32 height, bool& valid) {
    EnsureImage(std::max(image_width, 1u), std::max(image_height, 1u));
    valid = written && width == image_width && height == image_height;
    return image.Get();
}

} // namespace Vulkan
