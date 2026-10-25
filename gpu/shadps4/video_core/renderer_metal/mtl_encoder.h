// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Metal::Encoder records the renderer's commands into Metal command buffers (Metal-native
// renderer, spec component 2). It is the state machine of mtl_frame.mm (metal-backend) with
// native arguments: a render pass opens its Metal encoder at the first draw (a pass with no draws,
// clears or resolves costs nothing), blits and dispatches inside a pass end the render encoder
// and the pass reopens with Load actions, and state reaches the Metal encoder only when it changed.
// One thread uses an Encoder at a time.
//
// Untracked buffers (device-local ones, mtl_buffer.mm) are ordered by the Encoder with fences,
// per Metal encoder and per hazard class (mtl_order.h has the rule): each Metal encoder it opens
// declares what it does to untracked buffers (None, Read or Write) from the command that opens
// it, and a later command that needs more (a write in an encoder opened to read) ends it and
// opens another. Writes are blits into an untracked buffer and draws or dispatches whose
// pipeline was set with Access::Write; every draw and dispatch reads (shaders reach guest memory
// by address). Inside one Metal encoder nothing is ordered, as with hazard tracking.

#pragma once

#include <array>
#include <memory>
#include <string_view>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_metal/mtl_order.h"

namespace Metal {

class Device;

enum class Stage : u8 { Vertex, Fragment, Compute };
enum class Primitive : u8 { Point, Line, LineStrip, Triangle, TriangleStrip };
enum class IndexType : u8 { U16, U32 };
enum class CullMode : u8 { None, Front, Back };
enum class BlitAspect : u8 { All, Depth, Stencil };
enum class Winding : u8 { Clockwise, CounterClockwise };
enum class FillMode : u8 { Fill, Lines };
enum class DepthClip : u8 { Clip, Clamp };
/// Which stream of the queue an Encoder's command buffers belong to (mtl_order.h).
enum class Ordering : u8 {
    InOrder, ///< committed in the order recorded (the scheduler's encoder; one per device)
    Ahead,   ///< committed before the InOrder encoder's current command buffer (hoisted uploads)
};

/// How render encoders use a buffer reached by GPU address (Encoder::DeclareIndirect).
enum class IndirectUse : u8 {
    Read,        ///< read by vertex and fragment shaders (guest memory, page table)
    VertexWrite, ///< read and written by vertex shaders (object motion history)
};

struct Origin3 {
    u32 x{}, y{}, z{};
};
struct Size3 {
    u32 width{1}, height{1}, depth{1};
};

struct ColorAttachment {
    void* texture{}; ///< id<MTLTexture>; null: slot unused
    u32 level{}, slice{};
    bool clear{};
    std::array<double, 4> clear_color{};
    u32 depth_plane{};         ///< 3D targets
    void* resolve_texture{};   ///< MSAA resolve target (StoreAndMultisampleResolve)
    u32 resolve_level{}, resolve_slice{};
};

struct DepthAttachment {
    void* texture{}; ///< id<MTLTexture>; null: no depth
    u32 level{}, slice{};
    bool has_stencil{}; ///< the format has a stencil aspect (bound as the stencil attachment too)
    bool clear_depth{}, clear_stencil{};
    double depth{1.0};
    u32 stencil{};
    bool stencil_only{}; ///< Stencil8-type formats: bound as the stencil attachment only
};

struct RenderPassDesc {
    static constexpr u32 MaxColors = 8;
    std::array<ColorAttachment, MaxColors> colors{};
    DepthAttachment depth{};
    u32 width{}, height{}; ///< render area, used when there are no attachments
    /// A buffer this pass's vertex shaders read and write by GPU address (object motion
    /// history): declared on the pass's encoders like DeclareIndirect(VertexWrite), so hazard
    /// tracking orders only the passes that use it. Untracked (the default), the pass's draws
    /// are HazardClass::MotionHistory reads instead, ordered per frame (OrderBarrier).
    void* vertex_rw_buffer{};
};

struct Viewport {
    float x{}, y{}, width{}, height{}, min_depth{}, max_depth{1.0f};
};
struct Scissor {
    u32 x{}, y{}, width{}, height{};
};

/// Where Encoder samples GPU timestamps (the GPU profiler, BB_GPU_PROFILE). Called on the
/// encoding thread whenever the Encoder opens a Metal encoder.
class TimestampSink {
public:
    /// Room for `count` samples in `sample_buffer` (id<MTLCounterSampleBuffer>) from `index`;
    /// false: this encoder is not sampled. Render encoders take 3 (start of the vertex stage,
    /// start and end of the fragment stage), compute and blit encoders 2 (start, end).
    virtual bool Reserve(void*& sample_buffer, u32& index, u32 count) = 0;

protected:
    ~TimestampSink() = default;
};

/// bbport: BB_ENCODER_STATS frame boundary (Presenter, at each guest flip).
void EncoderStatsFrame();

class Encoder {
public:
    explicit Encoder(Device& device, Ordering ordering = Ordering::InOrder);
    ~Encoder();

    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Render passes. A pass must be ended before Submit.
    void BeginRenderPass(const RenderPassDesc& desc);
    void EndRenderPass();
    [[nodiscard]] bool InRenderPass() const noexcept;

    // Render state: kept across passes, applied at the next draw.
    /// id<MTLRenderPipelineState>; `buffers`: what its shaders do to untracked buffers (Write:
    /// they store to guest buffers; Read: they only load, incl. by address; None: they use no
    /// buffer, e.g. texture copies). Fault-buffer bits are not writes (mtl_order.h).
    void SetRenderPipeline(void* pipeline, Access buffers = Access::Write);
    void SetDepthStencil(void* state);      ///< id<MTLDepthStencilState>; null: no depth/stencil test
    void SetViewport(const Viewport& viewport);
    void SetScissor(const Scissor& scissor);
    void SetCullMode(CullMode mode);
    void SetFrontFace(Winding winding);
    void SetDepthBias(float constant, float slope, float clamp);
    void SetStencilReference(u32 front, u32 back);
    void SetBlendColor(float r, float g, float b, float a);
    void SetFillMode(FillMode mode);
    void SetDepthClip(DepthClip mode);
    /// Tessellation factors for DrawPatches (setTessellationFactorBuffer:offset:instanceStride:).
    void SetTessellationFactorBuffer(BufferRef factors, u32 instance_stride);

    // Resources by stage and slot: render stages apply at the next draw, compute at the next
    // dispatch. Slots: buffers 0-30, textures 0-127, samplers 0-15.
    // Bindings are unretained pointers that stay bound until replaced, into new Metal encoders
    // (which get every bound slot again) but not across Submit: a new command buffer starts
    // unbound, as in Vulkan. Callers must keep resources alive through GPU completion via
    // deferred deletion or KeepAlive; submitting alone does not make them safe to free.
    void SetBuffer(Stage stage, u32 slot, BufferRef buffer);
    /// Copies `size` bytes now. Up to 4 KiB they go inline (setBytes); more go through a
    /// temporary shared buffer kept until the command buffer completes.
    void SetBytes(Stage stage, u32 slot, const void* data, u32 size);
    /// A vertex buffer with an attribute stride (the pipeline's vertex layout uses
    /// MTLBufferLayoutStrideDynamic): setVertexBuffer:offset:attributeStride:. Occupies the
    /// vertex-stage buffer `slot`, like SetBuffer(Stage::Vertex, ...), and is replaced by it.
    /// A stride of 0 reads element 0 for every vertex (Vulkan's stride 0).
    void SetVertexBuffer(u32 slot, BufferRef buffer, u32 stride);
    void SetTexture(Stage stage, u32 slot, void* texture); ///< id<MTLTexture>
    void SetSampler(Stage stage, u32 slot, void* sampler); ///< id<MTLSamplerState>

    /// Per-Encoder lifetime counters, collected only with BB_ENCODER_STATS=1. `emitted`
    /// counts resource-binding Metal calls; `skipped` counts identical setter requests.
    /// BB_METAL_BINDING_CACHE=0 keeps the original setters for performance comparisons.
    struct BindingStats { u64 emitted{}, skipped{}; };
    [[nodiscard]] BindingStats GetBindingStats() const noexcept;

    void Draw(Primitive primitive, u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0,
              u32 first_instance = 0);
    void DrawIndexed(Primitive primitive, u32 index_count, IndexType type, BufferRef indices,
                     u32 instance_count = 1, s32 base_vertex = 0, u32 first_instance = 0);
    /// Draws with GPU-side arguments: MTLDrawPrimitivesIndirectArguments at `args`.
    void DrawIndirect(Primitive primitive, BufferRef args);
    /// MTLDrawIndexedPrimitivesIndirectArguments at `args`.
    void DrawIndexedIndirect(Primitive primitive, IndexType type, BufferRef indices, BufferRef args);
    /// Multi-draw indirect with a GPU count: clamps a copy of the `max_count` argument records at `args`
    /// (stride `stride` bytes) so records at index >= *count draw nothing (vertex/index count 0),
    /// with a compute pass before the render pass reopens, then issues max_count indirect draws.
    /// The clamp works on a scratch copy of the records (`args` is only read); the caller's
    /// compute state is untouched.
    void DrawIndirectCount(Primitive primitive, BufferRef args, u32 stride, BufferRef count,
                           u32 max_count, bool indexed, IndexType type = IndexType::U16,
                           BufferRef indices = {});
    /// Patch draw (drawPatches:...): `patch_index_buffer` may be empty. Needs a tessellation
    /// render pipeline and SetTessellationFactorBuffer.
    void DrawPatches(u32 control_points, u32 patch_start, u32 patch_count,
                     BufferRef patch_index_buffer, u32 instance_count, u32 base_instance);

    // Compute.
    /// id<MTLComputePipelineState>; `buffers` as for SetRenderPipeline. `lane`: Lane::Copy for
    /// copy kernels (the copy shader: their barriers do not wait for dispatches), else Compute.
    void SetComputePipeline(void* pipeline, Access buffers = Access::Write,
                            Lane lane = Lane::Compute);
    void Dispatch(Size3 groups, Size3 threads_per_group);
    /// Threadgroup counts (3 u32) are read from `args` on the GPU.
    void DispatchIndirect(BufferRef args, Size3 threads_per_group);
    /// The open compute encoder (id<MTLComputeCommandEncoder>, opened if needed) for kernels
    /// encoded outside the Encoder (tessellation and geometry emulation). The caller's pipeline
    /// and slots are re-applied by its next Dispatch. `buffers`: what the caller's kernels do to
    /// untracked buffers (as for SetComputePipeline).
    void* ComputeEncoder(Access buffers = Access::Write);

    // Blits. Inside a render pass they end the render encoder; the pass continues afterwards.
    void CopyBuffer(BufferRef src, BufferRef dst, u64 size);
    /// Fills `size` bytes with the 32-bit `pattern`: a blit when all four bytes are equal, else a
    /// compute kernel over whole words (an unaligned size is rounded down, an unaligned offset
    /// skips the fill; both log once). Size 0 does nothing (also for CopyBuffer).
    void FillBuffer(BufferRef dst, u64 size, u32 pattern);
    void CopyBufferToTexture(BufferRef src, u32 bytes_per_row, u32 bytes_per_image, void* texture,
                             u32 level, u32 slice, Origin3 origin, Size3 size,
                             BlitAspect aspect = BlitAspect::All);
    void CopyTextureToBuffer(void* texture, u32 level, u32 slice, Origin3 origin, Size3 size,
                             BufferRef dst, u32 bytes_per_row, u32 bytes_per_image,
                             BlitAspect aspect = BlitAspect::All);
    void CopyTexture(void* src, u32 src_level, u32 src_slice, Origin3 src_origin, void* dst,
                     u32 dst_level, u32 dst_slice, Origin3 dst_origin, Size3 size);

    /// Buffers shaders reach by GPU address, declared on every Metal encoder this Encoder opens
    /// from now on (useResource) so they are resident and, for tracked buffers, Metal's hazard
    /// tracking orders the encoders that use them (untracked buffers: the fences do).
    /// Compute encoders: read/write. Render encoders: read (vertex and fragment stages), or
    /// with VertexWrite read/write at the vertex stage. Declaring a buffer again changes its use.
    void DeclareIndirect(void* buffer, IndirectUse use = IndirectUse::Read);
    /// Stops declaring `buffer` on encoders opened from now on (before it is released; the
    /// open encoder keeps it until it ends).
    void RemoveIndirect(void* buffer);
    /// Samples GPU timestamps at the start and end of every Metal encoder opened from now on
    /// into `sink` (null: stops). Sampling may keep the GPU from overlapping adjacent encoders.
    void SetTimestampSink(TimestampSink* sink);
    /// Every command touching untracked memory of class `c` from now on waits for what `scopes`
    /// names among the commands before (OrderTracker::Barrier): staging round trips within one
    /// command and object motion's frame start. Ends the open Metal encoder when it touches `c`.
    /// No-op with hazard tracking and on Ahead encoders.
    void OrderBarrier(HazardClass c, BarrierScopes scopes = ScopeAll);
    /// The renderer's command whose work is encoded from now on (BufferBarriers::BeginCommand;
    /// Scheduler::SetOrderCommand): the Metal encoders that hold a command are known by it.
    /// Commands only grow.
    void SetOrderCommand(u64 command);
    [[nodiscard]] u64 OrderCommand() const noexcept;
    /// Draws of the current command are deferred (patch draws, Draw::Runs): the encoders that
    /// encode work until ReleaseOrderCommand also hold the oldest command held.
    void HoldOrderCommand();
    void ReleaseOrderCommand();
    /// The current command waits for the earlier commands `request` names (the renderer's
    /// conflicts on the guest arena, mtl_buffer_barriers.h): every Metal encoder it opens waits
    /// for the encoders holding them; the open one ends unless it already does
    /// (OrderTracker::Request). No-op with hazard tracking and on Ahead encoders.
    void OrderWait(const OrderRequest& request);
    /// True while any slot, pipeline or render state is set (Submit clears all of it).
    [[nodiscard]] bool HasBindings() const noexcept;

    /// Makes the GPU wait, before the commands recorded after this call, until the device has
    /// signalled `device_tick` (Device::Commit; Timeline::DeviceTickOf for another submitter's
    /// logical tick). Ends the open Metal encoder (a pass in progress reopens with Load).
    /// 0: no-op (already complete). `device_tick` must already be committed (asserted): the queue
    /// starts command buffers in commit order, so waiting for a later commit never ends.
    void WaitForDeviceTick(u64 device_tick);
    /// Debug groups: on the command buffer when no encoder is open, else on the open encoder
    /// (closed with the encoder if still open then). Pops must match pushes.
    void PushDebugGroup(std::string_view label);
    void PopDebugGroup();
    /// Keeps `object` alive until the current command buffer completes.
    void KeepAlive(Object object);
    /// Ends the open render, compute or blit encoder (a pass in progress reopens with Load). A
    /// pass that has not opened its encoder yet runs its clears first.
    void EndEncoding();
    /// The command buffer being recorded (id<MTLCommandBuffer>, created if needed), for work
    /// encoded outside the Encoder (MetalFX). Call EndEncoding() first.
    void* CommandBuffer();
    /// Ends encoding, commits with the device's next tick and returns it. Starts a new command
    /// buffer on the next command. Asserts that no render pass is in progress.
    u64 Submit();
    /// Ends encoding and commits with no tick (Device::CommitUntracked); keep-alive objects are
    /// released when it completes. For the scheduler's hoisted-upload command buffer.
    void SubmitUntracked();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Metal
