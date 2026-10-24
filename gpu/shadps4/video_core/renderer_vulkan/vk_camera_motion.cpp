// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_camera_motion.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_toggles.h"
#include "common/assert.h"

#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/camera_motion_comp.h"
#include "video_core/host_shaders/camera_motion_debug_comp.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

using PushConstants = CameraMotion::Params;
static_assert(sizeof(PushConstants) == 112 && offsetof(PushConstants, mode) == 104);

Metal::Size3 Groups(const PushConstants& push, const Metal::HostKernel& kernel) {
    const u32 width = u32(push.size[0]), height = u32(push.size[1]);
    return {(width + kernel.local_size[0] - 1) / kernel.local_size[0],
            (height + kernel.local_size[1] - 1) / kernel.local_size[1], 1};
}

Metal::Size3 Threads(const Metal::HostKernel& kernel) {
    return {kernel.local_size[0], kernel.local_size[1], kernel.local_size[2]};
}

/// a * b for 3x4 affine matrices (rows [R | t]).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

} // namespace

void CameraMotion::Record(Metal::Encoder& encoder, const Metal::HostKernel& kernel, void* depth,
                          void* motion, void* object, const Params& params) {
    encoder.SetComputePipeline(kernel.pipeline.Get(), Metal::Access::None); // textures only
    encoder.SetTexture(Metal::Stage::Compute, kernel.texture_slot[0], depth);
    encoder.SetTexture(Metal::Stage::Compute, kernel.texture_slot[1], motion);
    encoder.SetTexture(Metal::Stage::Compute, kernel.texture_slot[2], object);
    encoder.SetBytes(Metal::Stage::Compute, Metal::kPushConstantBuffer, &params, sizeof(params));
    encoder.Dispatch(Groups(params, kernel), Threads(kernel));
}

CameraMotion::CameraMotion(Metal::Device& device_, Scheduler& scheduler_,
                           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : device{device_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_} {
    const char* env = std::getenv("BB_DEBUG_MOTION");
    debug_overlay = env && env[0] == '1';
    const char* upscaler = std::getenv("BB_UPSCALER");
    // The upscaler can be switched on from the menu at any time: the camera is always tracked
    // unless BB_UPSCALER=none.
    for_upscaler = !(upscaler && std::strcmp(upscaler, "none") == 0);
    // Built here, not at first use on the recording thread.
    if (for_upscaler) {
        motion_kernel = Metal::BuildHostKernel(device, CAMERA_MOTION_COMP, {}, "camera_motion");
    }
    if (!debug_overlay) {
        return;
    }
    overlay_kernel =
        Metal::BuildHostKernel(device, CAMERA_MOTION_DEBUG_COMP, {}, "camera_motion_debug");
    for (auto& frame : frames) {
        frame = std::make_unique<VideoCore::Buffer>(device, VideoCore::MemoryType::DeviceLocal, 0,
                                                    3840ull * 2160 * 4, "CameraMotionDebugFrame");
    }
    std::printf("GPU: camera motion debug overlay on\n");
}

CameraMotion::~CameraMotion() = default;

float CameraMotion::VerticalFov() const noexcept {
    return 2.0f * std::atan(1.0f / current.proj[1]);
}

float CameraMotion::Near() const noexcept {
    // depth = zs + zo / z is 0 at the near plane.
    return -current.proj[3] / current.proj[2];
}

bool CameraMotion::Continuous() const noexcept {
    if (!Ready() || current.proj != previous.proj) return false;
    float distance = 0, forward = 0, up = 0;
    for (const u32 i : {3u, 7u, 11u}) {
        const float delta = current.inv_view[i] - previous.inv_view[i];
        distance += delta * delta;
    }
    for (const u32 i : {8u, 9u, 10u}) forward += current.view[i] * previous.view[i];
    for (const u32 i : {4u, 5u, 6u}) up += current.view[i] * previous.view[i];
    return std::isfinite(distance) && std::isfinite(forward) && std::isfinite(up) &&
           distance <= 100 && forward >= .5f && up >= .5f;
}

std::array<std::array<float, 4>, 3> CameraMotion::TaaDepthParameters() const noexcept {
    const auto transform = Multiply(previous.view, current.inv_view);
    return {current.proj, previous.proj,
            {transform[8], transform[9], transform[10], transform[11]}};
}

void* CameraMotion::ObjectMotionView() const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->View() : nullptr;
}

void* CameraMotion::ObjectMotionImage(u32 width, u32 height) const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->Image(width, height)
                                                      : nullptr;
}

CameraMotion::Params CameraMotion::MakeParams(u32 width, u32 height, u32 mode) const noexcept {
    return {
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(width), float(height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = mode,
    };
}

void CameraMotion::RecordMotion(void* depth_texture, void* motion_texture, u32 width,
                                u32 height) {
    ASSERT_MSG(motion_kernel.pipeline, "Camera motion: RecordMotion with BB_UPSCALER=none");
    bool object_valid = false;
    void* const object_texture = object_motion && object_motion->Enabled()
        ? object_motion->PrepareRead(width, height, object_valid) : depth_texture;
    const Params params = MakeParams(width, height, object_valid ? 1u : 0u);
    scheduler.EndRendering();
    scheduler.Record([kernel = &motion_kernel, depth_texture, motion_texture, object_texture,
                      params](Metal::Encoder& e) {
        Record(e, *kernel, depth_texture, motion_texture, object_texture, params);
    });
}

void CameraMotion::OnConstants(const float* data) {
    // Scene constants: far plane 3000, 1/far, and the render size.
    if (data[0] != 3000.0f || data[4] < 64.0f || data[5] < 64.0f ||
        std::abs(data[1] * data[0] - 1.0f) > 1e-3f) {
        return;
    }
    if (frame_has_camera) {
        return; // the first one of a frame is the main camera
    }
    previous = current;
    std::memcpy(current.view.data(), data + 8, 12 * sizeof(float));
    std::memcpy(current.inv_view.data(), data + 180, 12 * sizeof(float));
    current.proj = {data[52], data[57], data[62], data[63]};
    current.valid = current.proj[0] != 0.0f && current.proj[1] != 0.0f;
    const std::array<u32, 2> size{u32(data[4]), u32(data[5])};
    if (size != render_size) {
        std::printf("Camera motion: scene render size %ux%u\n", size[0], size[1]);
        render_size = size;
    }
    frame_has_camera = true;
}

void CameraMotion::OnGBufferPass(VideoCore::ImageId depth) {
    depth_id = depth;
}

void CameraMotion::OnDisplayPass(VideoCore::ImageId frame) {
    if (debug_overlay && frame && depth_id && current.valid && previous.valid) {
        Overlay(frame);
    }
    if (!frame_has_camera) InvalidateHistory();
    frame_has_camera = false;
    depth_id = {};
}

void CameraMotion::Overlay(VideoCore::ImageId frame) {
    auto& depth = texture_cache.GetImage(depth_id);
    auto& color = texture_cache.GetImage(frame);
    const auto depth_format = depth.info.pixel_format;
    if ((depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD32SfloatS8Uint) ||
        color.info.pixel_format != vk::Format::eR8G8B8A8Unorm || !color.texture_desc.shader_write ||
        color.info.size.width != depth.info.size.width ||
        color.info.size.height != depth.info.size.height) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("Camera motion: overlay skipped (depth %s, frame %s %ux%u%s)\n",
                        vk::to_string(depth_format).c_str(),
                        vk::to_string(color.info.pixel_format).c_str(), color.info.size.width,
                        color.info.size.height,
                        color.texture_desc.shader_write ? "" : ", not shader-writable");
        }
        return;
    }

    scheduler.EndRendering();
    runtime.AccessImage(&depth, false);
    runtime.AccessImage(&color, true);

    const Params push = MakeParams(color.info.size.width, color.info.size.height,
                                   BbToggle::Disabled(1u << 20)   ? 1u
                                   : BbToggle::Disabled(1u << 21) ? 2u
                                   : BbToggle::Disabled(1u << 22) ? 3u
                                   : BbToggle::Disabled(1u << 23) ? 4u
                                                                  : 0u);
    static u32 log_counter = 0;
    if (++log_counter % 200 == 0) {
        const auto& m = push.reproject;
        std::printf("Camera motion: proj %g %g %g %g prev %g %g %g %g\n"
                    "  view  %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f\n"
                    "  reproj %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f\n"
                    "  depth %s %ux%u, frame %ux%u\n",
                    push.proj[0], push.proj[1], push.proj[2], push.proj[3], push.prev_proj[0],
                    push.prev_proj[1], push.prev_proj[2], push.prev_proj[3], current.view[0],
                    current.view[1], current.view[2], current.view[3], current.view[4],
                    current.view[5], current.view[6], current.view[7], current.view[8],
                    current.view[9], current.view[10], current.view[11], m[0], m[1], m[2], m[3],
                    m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                    vk::to_string(depth_format).c_str(), depth.info.size.width,
                    depth.info.size.height, color.info.size.width, color.info.size.height);
    }
    // The previous frame buffer was written by the last overlay dispatch: both are bound
    // buffers, so Metal's hazard tracking orders the two dispatches.
    const auto prev_frame = frames[frame_index ^ 1]->Ref();
    const auto next_frame = frames[frame_index]->Ref();
    frame_index ^= 1;
    scheduler.Record([kernel = &overlay_kernel, depth_texture = depth.GetImage(),
                      color_texture = color.GetImage(), prev_frame, next_frame,
                      push](Metal::Encoder& e) {
        e.SetComputePipeline(kernel->pipeline.Get());
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[0], depth_texture);
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[1], color_texture);
        e.SetBuffer(Metal::Stage::Compute, kernel->buffer_slot[2], prev_frame);
        e.SetBuffer(Metal::Stage::Compute, kernel->buffer_slot[3], next_frame);
        e.SetBytes(Metal::Stage::Compute, Metal::kPushConstantBuffer, &push, sizeof(push));
        e.Dispatch(Groups(push, *kernel), Threads(*kernel));
    });
}

} // namespace Vulkan
