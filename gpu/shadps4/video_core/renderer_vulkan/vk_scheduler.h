// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <mutex>
#include <thread>
#include <queue>
#include <pthread.h>

#include <boost/container/static_vector.hpp>

#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/interval_set.h"
#include "common/logging/log.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/buffer_cache/buffer_copy.h"
#include "video_core/renderer_metal/mtl_depth_stencil.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_draw.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_raster_util.h"
#include "video_core/renderer_metal/mtl_timeline.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// bbport: BB_PASS_LOG=<file>: the render passes recorded into each submission, by the tick it
/// signals (vk_scheduler.cpp).
namespace BbPassLog {
bool Enabled();
void Pass(u64 tick, const std::string& label);
} // namespace BbPassLog

/// One render target of a pass: the image's own texture or a plain view of it.
struct RenderAttachment {
    void* texture{}; ///< id<MTLTexture>
    u32 level{}, slice{}, depth_plane{};
    /// Colors: clear on load. The depth attachment: clear its depth on load.
    bool clear{};
    /// The depth attachment only: clear its stencil on load (independent of the depth clear).
    bool clear_stencil{};
    vk::ClearValue clear_value{};

    bool operator==(const RenderAttachment& other) const noexcept {
        // The clear value is a union of 16 bytes without padding.
        return texture == other.texture && level == other.level && slice == other.slice &&
               depth_plane == other.depth_plane && clear == other.clear &&
               clear_stencil == other.clear_stencil &&
               std::memcmp(&clear_value, &other.clear_value, sizeof(clear_value)) == 0;
    }
};

struct RenderState {
    std::array<RenderAttachment, 8> colors{};
    u32 num_colors{};
    RenderAttachment depth{};
    bool has_depth{}, has_stencil{}, stencil_only{};
    u32 width{}, height{};
    /// Metal::RenderPassDesc::vertex_rw_buffer (passes with object motion pipelines).
    void* vertex_rw_buffer{};

    /// The Metal render pass: colors and depth/stencil stored, loaded unless cleared.
    Metal::RenderPassDesc ToDesc() const;

    /// Memberwise (no memcmp: the struct has padding). Color slots past num_colors are ignored.
    bool operator==(const RenderState& other) const noexcept {
        if (num_colors != other.num_colors || has_depth != other.has_depth ||
            has_stencil != other.has_stencil || stencil_only != other.stencil_only ||
            width != other.width || height != other.height || !(depth == other.depth) ||
            vertex_rw_buffer != other.vertex_rw_buffer) {
            return false;
        }
        for (u32 i = 0; i < num_colors; ++i) {
            if (!(colors[i] == other.colors[i])) {
                return false;
            }
        }
        return true;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;

        bool attachments : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// The pass's depth and stencil attachments (tests of a missing aspect are off).
    bool has_depth_attachment{};
    bool has_stencil_attachment{};

    /// The whole state of the next draw in the draw helper's terms, kept up to date by
    /// CommitWith() (the target size is the pass's, filled by the caller).
    Metal::Draw::DrawState draw_state{};

    /// bbport: folds the dirty fields into `draw_state`, clears the flags it used, and passes
    /// the complete state to `emit` (the draw helper takes the whole state with every draw: a
    /// tessellation run flushed before a draw leaves its own state on the encoder). Flags whose
    /// test is disabled stay dirty. The depth/stencil fields become one MTLDepthStencilState,
    /// looked up here (on the producer thread) only when they changed. The PSO-key state
    /// (primitive restart, color write masks, line width, depth bounds, feedback loop) comes
    /// from the pipeline: its flags are cleared without effect.
    template <typename Emit>
    void CommitWith(Metal::DepthStencilCache& depth_stencil, Emit&& emit) {
        Metal::Draw::DrawState& s = draw_state;
        if (dirty_state.viewports) {
            dirty_state.viewports = false;
            if (!viewports.empty()) {
                // Vulkan conventions: the helper converts with RasterUtil::ToMetalViewport.
                // Metal has one viewport per draw: the first one.
                const vk::Viewport& v = viewports[0];
                s.viewport_x = v.x;
                s.viewport_y = v.y;
                s.viewport_width = v.width;
                s.viewport_height = v.height;
                s.min_depth = v.minDepth;
                s.max_depth = v.maxDepth;
            }
        }
        if (dirty_state.scissors) {
            dirty_state.scissors = false;
            if (!scissors.empty()) {
                // The helper clamps to the target (negative offsets included).
                const vk::Rect2D& r = scissors[0];
                s.scissor_x = r.offset.x;
                s.scissor_y = r.offset.y;
                s.scissor_width = r.extent.width;
                s.scissor_height = r.extent.height;
            }
        }

        // Depth and stencil: one state object for all of the fields below.
        const bool stencil_faces_dirty =
            dirty_state.stencil_front_ops || dirty_state.stencil_back_ops ||
            dirty_state.stencil_front_write_mask || dirty_state.stencil_back_write_mask ||
            dirty_state.stencil_front_compare_mask || dirty_state.stencil_back_compare_mask;
        if (dirty_state.attachments || dirty_state.depth_test_enabled ||
            dirty_state.depth_write_enabled ||
            (depth_test_enabled && dirty_state.depth_compare_op) ||
            dirty_state.stencil_test_enabled || (stencil_test_enabled && stencil_faces_dirty)) {
            Metal::DepthStencilKey key{};
            key.depth_test = depth_test_enabled;
            key.depth_write = depth_write_enabled;
            if (depth_test_enabled) {
                key.depth_compare = static_cast<u8>(depth_compare_op);
                dirty_state.depth_compare_op = false;
            }
            key.stencil_test = stencil_test_enabled;
            if (stencil_test_enabled) {
                const auto face = [](const StencilOps& o, u32 compare_mask, u32 write_mask) {
                    return Metal::StencilFace{
                        .fail = static_cast<u8>(o.fail_op),
                        .pass = static_cast<u8>(o.pass_op),
                        .depth_fail = static_cast<u8>(o.depth_fail_op),
                        .compare = static_cast<u8>(o.compare_op),
                        .read_mask = static_cast<u8>(compare_mask),
                        .write_mask = static_cast<u8>(write_mask),
                    };
                };
                key.front = face(stencil_front_ops, stencil_front_compare_mask,
                                 stencil_front_write_mask);
                key.back = face(stencil_back_ops, stencil_back_compare_mask,
                                stencil_back_write_mask);
                dirty_state.stencil_front_ops = dirty_state.stencil_back_ops = false;
                dirty_state.stencil_front_write_mask = dirty_state.stencil_back_write_mask = false;
                dirty_state.stencil_front_compare_mask = dirty_state.stencil_back_compare_mask =
                    false;
            }
            dirty_state.depth_test_enabled = dirty_state.depth_write_enabled = false;
            dirty_state.stencil_test_enabled = false;
            dirty_state.attachments = false;
            // Ignored fields zeroed, so equal states share one MTLDepthStencilState.
            s.depth_stencil = depth_stencil.Get(Metal::Draw::MaskDepthStencil(
                key, has_depth_attachment, has_stencil_attachment));
        }
        if (stencil_test_enabled &&
            (dirty_state.stencil_front_reference || dirty_state.stencil_back_reference)) {
            dirty_state.stencil_front_reference = dirty_state.stencil_back_reference = false;
            s.stencil_ref_front = stencil_front_reference;
            s.stencil_ref_back = stencil_back_reference;
        }
        if (dirty_state.depth_bias_enabled || (depth_bias_enabled && dirty_state.depth_bias)) {
            dirty_state.depth_bias_enabled = false;
            if (depth_bias_enabled) {
                dirty_state.depth_bias = false;
            }
            const bool on = depth_bias_enabled;
            s.depth_bias_enable = on;
            s.depth_bias_constant = on ? depth_bias_constant : 0.0f;
            s.depth_bias_slope = on ? depth_bias_slope : 0.0f;
            s.depth_bias_clamp = on ? depth_bias_clamp : 0.0f;
        }
        if (dirty_state.cull_mode) {
            dirty_state.cull_mode = false;
            // Metal has no front-and-back culling: the helper skips those draws.
            s.cull_all = cull_mode == vk::CullModeFlagBits::eFrontAndBack;
            s.cull = ToMetalCull(cull_mode);
        }
        if (dirty_state.front_face) {
            dirty_state.front_face = false;
            s.front_face = front_face == vk::FrontFace::eClockwise
                               ? Metal::Winding::Clockwise
                               : Metal::Winding::CounterClockwise;
        }
        if (dirty_state.blend_constants) {
            dirty_state.blend_constants = false;
            s.blend_color = blend_constants;
        }
        if (dirty_state.rasterizer_discard_enable) {
            dirty_state.rasterizer_discard_enable = false;
            s.rasterizer_discard = rasterizer_discard_enable; // the helper skips the draw
        }
        // Metal: these belong to the pipeline key or have no equivalent.
        dirty_state.primitive_restart_enable = false;
        dirty_state.color_write_masks = false;
        dirty_state.line_width = false;
        dirty_state.feedback_loop_enabled = false;
        dirty_state.depth_bounds_test_enabled = false;
        if (depth_bounds_test_enabled) {
            dirty_state.depth_bounds = false;
        }
        emit(static_cast<const Metal::Draw::DrawState&>(s));
    }

    /// Front or back culling (none for front-and-back: DrawState::cull_all skips those draws).
    static Metal::CullMode ToMetalCull(vk::CullModeFlags mode) {
        if (mode == vk::CullModeFlagBits::eFront) {
            return Metal::CullMode::Front;
        }
        if (mode == vk::CullModeFlagBits::eBack) {
            return Metal::CullMode::Back;
        }
        return Metal::CullMode::None;
    }

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        // bbport: named flags only (a memset also set the padding bits).
        dirty_state.viewports = true;
        dirty_state.scissors = true;
        dirty_state.depth_test_enabled = true;
        dirty_state.depth_write_enabled = true;
        dirty_state.depth_compare_op = true;
        dirty_state.depth_bounds_test_enabled = true;
        dirty_state.depth_bounds = true;
        dirty_state.depth_bias_enabled = true;
        dirty_state.depth_bias = true;
        dirty_state.stencil_test_enabled = true;
        dirty_state.stencil_front_ops = true;
        dirty_state.stencil_front_reference = true;
        dirty_state.stencil_front_write_mask = true;
        dirty_state.stencil_front_compare_mask = true;
        dirty_state.stencil_back_ops = true;
        dirty_state.stencil_back_reference = true;
        dirty_state.stencil_back_write_mask = true;
        dirty_state.stencil_back_compare_mask = true;
        dirty_state.primitive_restart_enable = true;
        dirty_state.rasterizer_discard_enable = true;
        dirty_state.cull_mode = true;
        dirty_state.front_face = true;
        dirty_state.blend_constants = true;
        dirty_state.color_write_masks = true;
        dirty_state.line_width = true;
        dirty_state.feedback_loop_enabled = true;
        dirty_state.attachments = true;
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachments(const bool has_depth, const bool has_stencil) {
        if (has_depth_attachment != has_depth || has_stencil_attachment != has_stencil) {
            has_depth_attachment = has_depth;
            has_stencil_attachment = has_stencil;
            dirty_state.attachments = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }
};

/// bbport: a block of deferred encoder commands. Commands are closures placed in
/// fixed storage (no allocation per command) and run in order on the recording thread.
class RecordChunk {
public:
    static constexpr size_t Capacity = 128 * 1024;

    /// Returns false (and leaves `func` untouched) when the chunk has no room. `order_command`:
    /// the renderer's command it encodes (Metal::Encoder::SetOrderCommand).
    template <typename Func>
    bool Push(Func&& func, u64 order_command) {
        using Command = TypedCommand<std::decay_t<Func>>;
        static_assert(sizeof(Command) <= Capacity, "recorded command is too large");
        const size_t offset = (used + alignof(Command) - 1) & ~(alignof(Command) - 1);
        if (offset + sizeof(Command) > Capacity) {
            return false;
        }
        auto* command = new (storage + offset) Command(std::forward<Func>(func));
        command->order_command = order_command;
        if (last) {
            last->next = command;
        } else {
            first = command;
        }
        last = command;
        used = offset + sizeof(Command);
        PrefetchAhead();
        return true;
    }

    /// Raw storage in the chunk for a command's variable-length data; null when full.
    void* Allocate(size_t bytes, size_t align) {
        const size_t offset = (used + align - 1) & ~(align - 1);
        if (offset + bytes > Capacity) {
            return nullptr;
        }
        used = offset + bytes;
        PrefetchAhead();
        return storage + offset;
    }

    void Execute(Metal::Encoder& encoder) {
        for (CommandBase* command = first; command;) {
            CommandBase* const next = command->next;
            encoder.SetOrderCommand(command->order_command);
            command->Execute(encoder);
            command->~CommandBase();
            command = next;
        }
        first = last = nullptr;
        used = 0;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return first == nullptr;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return used;
    }

private:
    /// The recording thread last read these cache lines: the first write to each has to take
    /// it back from that core. Requesting ownership a few lines ahead overlaps those
    /// transfers instead of stalling on each (RecordPrefetch).
    void PrefetchAhead() const {
        if (!BbToggle::Disabled(BbToggle::RecordPrefetch)) {
            __builtin_prefetch(storage + used + 384, 1, 3);
            __builtin_prefetch(storage + used + 448, 1, 3);
        }
    }

    struct CommandBase {
        virtual ~CommandBase() = default;
        virtual void Execute(Metal::Encoder& encoder) = 0;
        CommandBase* next{};
        u64 order_command{};
    };
    template <typename Func>
    struct TypedCommand final : CommandBase {
        explicit TypedCommand(Func&& func_) : func{std::move(func_)} {}
        explicit TypedCommand(const Func& func_) : func{func_} {}
        void Execute(Metal::Encoder& encoder) override {
            func(encoder);
        }
        Func func;
    };

    alignas(64) std::byte storage[Capacity];
    size_t used = 0;
    CommandBase* first{};
    CommandBase* last{};
};

class Scheduler {
public:
    /// `threaded`: commands passed to Record() are encoded by a worker thread.
    explicit Scheduler(Metal::Device& device, bool threaded);
    ~Scheduler();

    /// Commits the current command buffer with the device's next tick.
    void Flush();

    /// Commits the current command buffer and waits for it to complete.
    void Finish();

    /// Waits for the given tick (committing the current command buffer first if it is that tick).
    void Wait(u64 tick);

    /// Runs deferred operations whose tick the GPU has completed.
    void PopPendingOperations();

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state);

    /// bbport: called right after a render pass ends (before the command that ended it is
    /// recorded, and before a submission): the deferred copy-shader copies (vk_shader_hle.h).
    using PassEndHook = void (*)(void* context);
    void SetPassEndHook(PassEndHook hook, void* context) {
        pass_end_hook = hook;
        pass_end_context = context;
    }
    /// bbport: reopens the render pass that ended last on the same attachments, loading them
    /// (its clears already ran).
    void ResumeRendering();

    /// bbport: hoisted copies are recorded now, on the calling thread, into the upload encoder,
    /// whose command buffer is committed (untracked) right before the next submission, so they
    /// run before all of its commands (Runtime::HoistCopy decides when that is safe). Copies
    /// there do not end the current render pass: on a tile-based GPU every reopened pass reloads
    /// its attachments.
    /// `ordered`: the copy may read or write memory that copies hoisted before it (in this call or
    /// earlier) wrote: each copy starts a new blit encoder (Metal does not document the order of
    /// copies within one).
    /// The upload encoder's encoders are chained; the arena accesses after a barrier on the
    /// uploads wait for them (Runtime::HoistCopy marks their ranges).
    void RecordHoistedCopy(Metal::BufferRef src, Metal::BufferRef dst,
                           std::span<const VideoCore::BufferCopy> copies, bool ordered = false);

    /// Ends current rendering scope. bbport: the caller is counted with BB_PASS_STATS=1.
    void EndRendering(std::source_location where = std::source_location::current(),
                      const char* detail = "");

    /// Sets a function to be called on every scheduler submission.
    void SetSubmitCallback(std::function<void()> callback) {
        on_submit = std::move(callback);
    }

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the encoder for encoding on the calling thread. With threaded recording this
    /// first waits until all commands passed to Record() are encoded, then encodes directly
    /// (Record() included, so callers may mix both) until the next KickRecording() or
    /// submission.
    [[gnu::noinline]] Metal::Encoder& DirectEncoder() {
        if (draw_run_open) {
            EndDrawRun(); // the caller may encode anything
        }
        if (recorder_running && !direct_mode) {
            SyncRecording();
            direct_mode = true;
            direct_recordings.fetch_add(1, std::memory_order_relaxed);
            TraceDirectRecording(__builtin_return_address(0));
        }
        encoder.SetOrderCommand(order_command);
        return encoder;
    }

    /// BB_RECORDER_TRACE=1: prints the most frequent DirectEncoder() callers every 2000 calls.
    static void TraceDirectRecording(void* caller);

    /// Records `func(Metal::Encoder&)` in order with other commands. The closure must own
    /// everything it uses (capture by value): it may run later on the recording thread.
    /// Patch draws a tessellated draw left waiting (RecordDraw) are drawn first, so no waiting
    /// patch draw crosses a copy, clear, debug group, pass end or submission recorded here.
    template <typename Func>
    void Record(Func&& func) {
        if (draw_run_open) [[unlikely]] {
            EndDrawRun();
        }
        RecordCommand(std::forward<Func>(func));
    }

    /// Records a draw or dispatch that goes through GetDrawRuns() (which ends a waiting
    /// tessellation run itself before anything else). `leaves_run`: the command may leave patch
    /// draws waiting in the runs (a tessellated draw): the next Record() draws them first.
    template <typename Func>
    void RecordDraw(Func&& func, bool leaves_run) {
        RecordCommand(std::forward<Func>(func));
        draw_run_open = leaves_run;
    }

    /// The draw helper of this scheduler's encoder: used by recorded commands only (on the
    /// recording thread), and by the scheduler's pass ends and submissions, which go through
    /// its EndRenderPass / Submit wrappers.
    [[nodiscard]] Metal::Draw::Runs& GetDrawRuns() noexcept {
        return draw_runs;
    }

    /// bbport: debug groups (Xcode GPU captures, Metal System Trace). A pop without its push in
    /// the same command buffer (a submission in between closed it) is dropped.
    void PushDebugGroup(std::string label);
    void PopDebugGroup();
    /// An empty debug group with this label (Metal has no labels without a scope).
    void InsertDebugLabel(std::string label);

private:
    template <typename Func>
    void RecordCommand(Func&& func) {
        if (!recorder_running || direct_mode) {
            encoder.SetOrderCommand(order_command);
            func(encoder);
            return;
        }
        if (BbToggle::Disabled(BbToggle::ThreadedRecording)) {
            SyncRecording();
            direct_mode = true;
            encoder.SetOrderCommand(order_command);
            func(encoder);
            return;
        }
        const ChunkGuard guard{*this, __builtin_return_address(0)};
        if (!record_chunk->Push(std::forward<Func>(func), order_command)) {
            full_chunks.push_back(std::move(record_chunk));
            record_chunk = AcquireChunk();
            const bool pushed = record_chunk->Push(std::forward<Func>(func), order_command);
            ASSERT(pushed);
        }
    }

public:
    /// bbport: the renderer's command (Metal::BufferBarriers::BeginCommand) that the commands
    /// recorded from now on encode: the Encoder learns it with each (SetOrderCommand), so it
    /// knows which Metal encoders hold which command (Metal::OrderTracker).
    void SetOrderCommand(u64 command) noexcept {
        order_command = command;
    }

private:
    u64 order_command{};

    /// Records the end of the waiting tessellation run (Runs::Flush).
    void EndDrawRun() {
        draw_run_open = false;
        RecordCommand([runs = &draw_runs](Metal::Encoder& e) { runs->Flush(e); });
    }

public:
    /// True when Record() defers commands (and RecordData() copies into chunks).
    [[nodiscard]] bool IsRecordingDeferred() const noexcept {
        return recorder_running && !direct_mode &&
               !BbToggle::Disabled(BbToggle::ThreadedRecording);
    }

    /// Makes room for `bytes` of RecordData() plus the command that uses them in the current
    /// chunk: data and command must share a chunk, which is recycled once executed.
    void ReserveRecordData(size_t bytes) {
        if (!IsRecordingDeferred()) {
            return;
        }
        ASSERT(bytes + 1024 <= RecordChunk::Capacity);
        const ChunkGuard guard{*this, __builtin_return_address(0)};
        if (RecordChunk::Capacity - record_chunk->Size() < bytes + 1024) {
            full_chunks.push_back(std::move(record_chunk));
            record_chunk = AcquireChunk();
        }
    }

    /// Copies `data` into recording storage that lives until the command that uses it has
    /// been recorded (the same chunk). With threaded recording off, returns `data` itself.
    template <typename T>
    std::span<const T> RecordData(std::span<const T> data) {
        if (!IsRecordingDeferred() || data.empty()) {
            return data;
        }
        // Room for the data and the command that follows it, so both stay in one chunk.
        const size_t bytes = data.size_bytes();
        ReserveRecordData(bytes + alignof(T));
        auto* dst = static_cast<T*>(record_chunk->Allocate(bytes, alignof(T)));
        std::memcpy(dst, data.data(), bytes);
        return {dst, data.size()};
    }

    /// Hands recorded commands to the recording thread. Called at points where no caller holds
    /// the raw command buffer (end of draws and dispatches); small batches are kept.
    void KickRecording(bool force = false);

    /// Waits until every recorded command is in the command buffer.
    void SyncRecording();

    /// bbport: guest memory copies on the recording thread (small ones, RecordHostCopy) or the
    /// copy threads (BbCopy::Async) must be done before the guest learns the GPU is past them
    /// (it may then rewrite the memory, e.g. UI vertices: flickering) and before the
    /// submission that reads them. Waits for all.
    void WaitHostCopies();

    /// Runs `copy` on the recording thread in order with the commands (the thread spins for
    /// work anyway, so small copies cost no wakeup); WaitHostCopies() covers it.
    template <typename Func>
    void RecordHostCopy(Func&& copy) {
        // Not an encoder command: a waiting tessellation run may continue past it.
        RecordCommand([copy = std::forward<Func>(copy), this,
                seq = ++host_copies_issued](Metal::Encoder&) {
            copy();
            host_copies_done.store(seq, std::memory_order_release);
        });
    }

    /// bbport: runs `signal` once the guest memory copies issued so far are done, without
    /// waiting here: the recording thread reaches it after the copies queued before it and hands
    /// it to the copy threads' completion (BbCopy::AfterCopies).
    void SignalAfterHostCopies(std::function<void()> signal);

    /// bbport: before a guest-visible write that is not deferred (WriteData, end-of-shader
    /// fences, flip): waits until every signal handed to SignalAfterHostCopies ran, so that
    /// the write cannot overtake an earlier fence. Otherwise the guest, seeing the later value,
    /// may free memory the earlier fence then writes into (corrupted heap, guest fault).
    void WaitDeferredSignals();

    /// Whether a render pass is open.
    [[nodiscard]] bool IsRendering() const {
        return is_rendering;
    }

    /// Whether a render pass with exactly this state is open.
    [[nodiscard]] bool IsRenderingWith(const RenderState& state) const {
        return is_rendering && render_state == state;
    }

    /// Whether BeginRendering(state) would start a new render pass.
    [[nodiscard]] bool WillBeginRendering(const RenderState& state) const {
        return !(is_rendering && render_state == state);
    }

    /// DirectEncoder() calls that waited for a recording thread (BB_FRAME_STATS).
    static inline std::atomic<u64> direct_recordings{0};

    /// The tick this scheduler's next submission gets (its own timeline: other schedulers on
    /// the device do not advance it).
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return timeline.CurrentTick();
    }

    /// Returns true when the GPU completed this scheduler's submissions up to `tick`.
    [[nodiscard]] bool IsFree(u64 tick) const {
        return timeline.IsFree(tick);
    }

    /// The highest of this scheduler's ticks known complete.
    [[nodiscard]] u64 KnownGpuTick() const {
        return timeline.KnownGpuTick();
    }

    /// This scheduler's ticks, for tick users that are not the scheduler (staging, rings).
    [[nodiscard]] Metal::Timeline& GetTimeline() noexcept {
        return timeline;
    }

    [[nodiscard]] Metal::Device& GetDevice() noexcept {
        return device;
    }

    /// MTLDepthStencilStates by key (DynamicState::CommitWith).
    [[nodiscard]] Metal::DepthStencilCache& GetDepthStencilCache() noexcept {
        return depth_stencil;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick());
        num_pending_ops.fetch_add(1, std::memory_order_release);
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

    /// One committer at a time across schedulers (Device::Commit hands out ticks in commit order).
    static std::mutex submit_mutex;

private:
    std::unique_ptr<RecordChunk> AcquireChunk();

    void RecorderThread(std::stop_token stoken);

    /// Returns the tick (of this scheduler's timeline) the submission got.
    u64 SubmitExecution();

    void PriorityPendingOpsThread(std::stop_token stoken);

private:
    Metal::Device& device;
    Metal::Timeline timeline{device};
    Metal::Encoder encoder{device};
    /// bbport: this submission's hoisted copies, recorded on the calling thread while the
    /// recorder thread encodes into `encoder`.
    Metal::Encoder upload_encoder{device, Metal::Ordering::Ahead};
    std::mutex upload_mutex; ///< upload_encoder and upload_open
    bool upload_open{};
    Metal::DepthStencilCache depth_stencil{device};
    /// The draw helper's stand-ins and runs for `encoder`.
    Metal::Draw::NullResources draw_nulls{device};
    Metal::Draw::Runs draw_runs{device, draw_nulls.Get()};
    /// A recorded tessellated draw may have left patch draws waiting in draw_runs (producer
    /// side; see Record / RecordDraw).
    bool draw_run_open = false;
    /// Debug groups pushed in the command buffer being recorded (recording thread; reset by
    /// SubmitExecution after the recording is synced).
    u32 debug_groups = 0;
    DynamicState dynamic_state;
    std::function<void()> on_submit{};
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    std::atomic<u32> num_pending_ops{0}; ///< bbport: pending_ops.size(), checked without the lock
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
    PassEndHook pass_end_hook = nullptr;
    void* pass_end_context = nullptr;
    bool in_pass_end_hook = false;
    void PassEnded();
    // bbport: threaded recording
    /// The chunk being filled belongs to one thread at a time (the GPU command thread, or the
    /// draw recording thread while it runs). ChunkGuard reports overlapping users, e.g. a
    /// missing DrawPipe drain, which otherwise corrupt the chunk or crash on a null one.
    /// Each thread has one ChunkUser (its guard's caller and the thread itself), so the report
    /// names both sides: the thread that entered second, and the one it found inside, with
    /// that thread's stack at report time (macOS: the thread is suspended and its frame chain
    /// walked). The report reads the other thread's ChunkUser (a thread-local) and pthread_t:
    /// fine for the long-lived GPU threads that record, undefined once that thread has exited.
    /// The guard costs two stores and one exchange per Record.
    struct ChunkUser {
        void* caller;
        void* thread; ///< pthread_t
    };
    std::atomic<ChunkUser*> chunk_user{nullptr};
    [[nodiscard]] static ChunkUser& ThisChunkUser() noexcept {
        static thread_local ChunkUser user{};
        return user;
    }
    [[nodiscard]] static void* CurrentThreadHandle() noexcept {
        return (void*)(uintptr_t)pthread_self(); // a pointer (macOS) or an integer (Linux)
    }
    /// `previous_caller`: the reporting thread's caller before this guard, which is the caller
    /// found inside when the thread re-entered its own use (other == self).
    static void ReportChunkRace(const ChunkUser* other, const ChunkUser* self,
                                void* previous_caller);
    struct ChunkGuard {
        Scheduler& s;
        ChunkGuard(Scheduler& s_, void* caller) : s{s_} {
            ChunkUser& user = ThisChunkUser();
            void* const previous_caller = user.caller; // kept: a same-thread re-entry reports it
            user.caller = caller;
            user.thread = CurrentThreadHandle();
            if (ChunkUser* other = s.chunk_user.exchange(&user, std::memory_order_acquire)) {
                ReportChunkRace(other, &user, previous_caller);
            }
        }
        ~ChunkGuard() {
            s.chunk_user.store(nullptr, std::memory_order_release);
        }
    };
    std::unique_ptr<RecordChunk> record_chunk;
    std::vector<std::unique_ptr<RecordChunk>> full_chunks;
    std::mutex recorder_mutex;
    std::condition_variable_any recorder_cv;
    std::condition_variable_any recorder_idle_cv;
    std::deque<std::unique_ptr<RecordChunk>> recorder_queue;
    std::vector<std::unique_ptr<RecordChunk>> free_chunks;
    bool recorder_busy = false;
    std::atomic<size_t> queued_chunks{0}; ///< recorder_queue.size() for lock-free polling
    bool recorder_sleeping = false; ///< waiting on recorder_cv (guarded by recorder_mutex)
    bool direct_mode = false; ///< the command buffer is recorded on the caller's thread
    u64 host_copies_issued = 0;
    std::atomic<u64> host_copies_done{0};
    std::atomic<u64> deferred_signals_issued{0}; ///< by the thread recording (A or B)
    std::shared_ptr<std::atomic<u64>> deferred_signals_done =
        std::make_shared<std::atomic<u64>>(0);
    std::jthread recorder_thread;
    bool recorder_running = false;
};

} // namespace Vulkan
