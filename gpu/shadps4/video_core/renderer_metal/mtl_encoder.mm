// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Metal::Encoder (Metal-native renderer, spec component 2).

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cxxabi.h>
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_buffer.h"
#include "video_core/renderer_metal/mtl_device_impl.h"
#include "video_core/renderer_metal/mtl_encoder.h"

namespace Metal {

namespace {

constexpr u32 MaxBuffers = 31, MaxTextures = 128, MaxSamplers = 16;
constexpr u32 MaxInlineBytes = 4096;

/// The calling thread's stack, symbolised, one frame per line (the caller's frame first).
std::string CallerStack() {
    void* frames[32];
    const int count = backtrace(frames, 32);
    char** symbols = backtrace_symbols(frames, count);
    std::string out;
    for (int i = 1; i < count; ++i) {
        out += "\n    ";
        out += symbols ? symbols[i] : "?";
    }
    std::free(symbols);
    return out;
}

/// bbport: the encoder's pass state and the scheduler's have come apart: a pass the scheduler
/// did not count is open at a submission or at the next BeginRenderPass, or none is open at
/// EndRenderPass. Seen once in co-op (a summoned player loading in). A likely symptom of the
/// recording-chunk race (docs/MACOS_BACKLOG.md): the scheduler ends its pass before every
/// submission, so a pass can only be open there when another thread recorded its
/// BeginRenderPass at the same time; that race is fixed in Liverpool::ProcessCommands. The
/// mismatch is survived (see each caller) and the first eight are logged with the reporting
/// thread's stack, which names the thread that submitted or recorded.
void WarnPassMismatch(const char* what, const char* detail) {
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) >= 8) {
        return;
    }
    LOG_WARNING(Render,
                "{} ({}): likely the recording-chunk race (docs/MACOS_BACKLOG.md); stack:{}",
                what, detail, CallerStack());
}

/// A bit per texture slot.
struct TextureMask {
    std::array<u32, MaxTextures / 32> words{};

    void Set(u32 slot) { words[slot / 32] |= 1u << (slot % 32); }
    void Clear() { words = {}; }
    [[nodiscard]] bool Any() const {
        for (u32 w : words) {
            if (w) {
                return true;
            }
        }
        return false;
    }
    /// Calls `f(slot)` for every set slot.
    template <typename F>
    void ForEach(F&& f) const {
        for (u32 i = 0; i < words.size(); ++i) {
            u32 mask = words[i];
            while (mask) {
                f(i * 32 + u32(std::countr_zero(mask)));
                mask &= mask - 1;
            }
        }
    }
};

/// One stage's resources: what the caller set, and which slots changed since the Metal encoder
/// last saw them (bit per slot). A new Metal encoder starts empty, so it gets every used slot.
struct StageState {
    struct Slot {
        void* buffer{};
        u64 offset{};
        u32 stride{};          ///< the attribute stride (0 repeats element 0)
        bool attribute_stride{}; ///< bound with an attribute stride (vertex buffers)
        std::vector<u8> bytes; ///< non-empty: inline bytes instead of the buffer
    };
    std::array<Slot, MaxBuffers> buffers{};
    std::array<void*, MaxTextures> textures{};
    std::array<void*, MaxSamplers> samplers{};
    u32 buffers_dirty{}, buffers_used{};
    TextureMask textures_dirty, textures_used;
    u32 samplers_dirty{}, samplers_used{};

    void Reapply() {
        buffers_dirty = buffers_used;
        textures_dirty = textures_used;
        samplers_dirty = samplers_used;
    }
};

/// Calls `f(slot)` for every set bit of `mask`.
template <typename F>
void ForEachBit(u32 mask, F&& f) {
    while (mask) {
        const u32 slot = u32(std::countr_zero(mask));
        f(slot);
        mask &= mask - 1;
    }
}

MTLPrimitiveType ToMtl(Primitive primitive) {
    switch (primitive) {
    case Primitive::Point:
        return MTLPrimitiveTypePoint;
    case Primitive::Line:
        return MTLPrimitiveTypeLine;
    case Primitive::LineStrip:
        return MTLPrimitiveTypeLineStrip;
    case Primitive::Triangle:
        return MTLPrimitiveTypeTriangle;
    case Primitive::TriangleStrip:
        return MTLPrimitiveTypeTriangleStrip;
    }
    UNREACHABLE();
}

MTLCullMode ToMtl(CullMode mode) {
    switch (mode) {
    case CullMode::None:
        return MTLCullModeNone;
    case CullMode::Front:
        return MTLCullModeFront;
    case CullMode::Back:
        return MTLCullModeBack;
    }
    UNREACHABLE();
}

/// Copies the argument records to a scratch buffer, zeroing the draw-count word (the first) of
/// the records whose index is at or above the GPU-side count: indirect draws with a count. The
/// source records (guest data) are never written.
constexpr const char* kClampCountSource = R"(
#include <metal_stdlib>
using namespace metal;
kernel void clamp_indirect_count(device const uint* records [[buffer(0)]],
                                 device const uint* count [[buffer(1)]],
                                 constant uint2& params [[buffer(2)]], // stride in words, max count
                                 device uint* scratch [[buffer(3)]],
                                 uint i [[thread_position_in_grid]]) {
    if (i >= params.y) return;
    for (uint w = 0; w < params.x; ++w) scratch[i * params.x + w] = records[i * params.x + w];
    if (i >= *count) scratch[i * params.x] = 0;
}
)";

/// A buffer whose hazards the Encoder orders (mtl_order.h).
bool Untracked(void* buffer) {
    return buffer && [(__bridge id<MTLBuffer>)buffer hazardTrackingMode] ==
                         MTLHazardTrackingModeUntracked;
}

/// What a blit does to untracked memory: reads `src`, writes `dst` (either may be null). A write
/// into a buffer the caller orders (Device::OrderByCaller: the arena) is a plain access; a
/// write into any other untracked buffer is isolated (mtl_order.h).
Access BlitAccess(const Device& device, void* src, void* dst) {
    if (Untracked(dst)) {
        return device.OrderedByCaller(dst) ? Access::Read : Access::Write;
    }
    return Untracked(src) ? Access::Read : Access::None;
}

/// True when every class of `open` allows `need`.
bool Covers(const OrderTracker::Accesses& open, const OrderTracker::Accesses& need) {
    for (u32 i = 0; i < NumHazardClasses; ++i) {
        if (u8(open[i]) < u8(need[i])) {
            return false;
        }
    }
    return true;
}

void WaitFences(id<MTLRenderCommandEncoder> e, const OrderPlan& plan, const id<MTLFence>* fences) {
    plan.ForEachWait(
        [&](u32 f) { [e waitForFence:fences[f] beforeStages:MTLRenderStageVertex]; });
}
/// Render encoders: a slot's vertex fence after the vertex stage, its end fence after the
/// fragment stage; motion history after the vertex stage (only vertex shaders use it, so the
/// next pass's vertices may overlap these fragments).
void UpdateFences(id<MTLRenderCommandEncoder> e, const OrderPlan& plan, const id<MTLFence>* fences) {
    for (u32 i = 0; i < plan.num_updates; ++i) {
        const u32 f = plan.updates[i];
        [e updateFence:fences[f]
            afterStages:OrderTracker::IsVertexFence(f) ||
                                 OrderTracker::ClassOf(f) == HazardClass::MotionHistory
                            ? MTLRenderStageVertex
                            : MTLRenderStageFragment];
    }
}
template <typename E> // compute and blit encoders
void WaitFences(E e, const OrderPlan& plan, const id<MTLFence>* fences) {
    plan.ForEachWait([&](u32 f) { [e waitForFence:fences[f]]; });
}
template <typename E>
void UpdateFences(E e, const OrderPlan& plan, const id<MTLFence>* fences) {
    for (u32 i = 0; i < plan.num_updates; ++i) {
        [e updateFence:fences[plan.updates[i]]];
    }
}

MTLBlitOption BlitOptions(BlitAspect aspect) {
    switch (aspect) {
    case BlitAspect::All:
        return MTLBlitOptionNone;
    case BlitAspect::Depth:
        return MTLBlitOptionDepthFromDepthStencil;
    case BlitAspect::Stencil:
        return MTLBlitOptionStencilFromDepthStencil;
    }
    UNREACHABLE();
}

/// BB_METAL_FENCE_CHECK (default 1, 0 = off): checks every fence wait on the CPU before the GPU
/// sees it. A wait on a fence whose pending update belongs to another command buffer not committed
/// yet can stop the GPU (and with it WindowServer); a wait on a fence the same encoder updates is
/// counted separately. Violations are counted and the first ones logged.
class FenceCheck {
public:
    static FenceCheck& Get() {
        static FenceCheck check;
        return check;
    }
    bool On() const noexcept {
        return on;
    }
    /// `cmd` encodes waits for `waits.waits` in an encoder that will update `own.updates`.
    void Waits(const void* cmd, const OrderPlan& waits, const OrderPlan& own, const char* where) {
        std::scoped_lock lock{mutex};
        waits.ForEachWait([&](u32 f) {
            for (u32 i = 0; i < own.num_updates; ++i) {
                if (own.updates[i] == f) {
                    // OpenAhead waits for and updates AheadFence by design; slot fences should not.
                    if (f == OrderTracker::AheadFence) {
                        ++self_waits;
                    } else {
                        Report(self_waits, "waits for a slot fence its own encoder updates", f,
                               where);
                    }
                }
            }
            const auto it = owner.find(f);
            if (it != owner.end() && it->second != cmd) {
                // Fine if that command buffer is committed first (Committed checks).
                ++cross_waits;
                if (f != OrderTracker::AheadFence) {
                    Report(slot_cross, "waits for a slot fence another uncommitted command buffer "
                                       "updates", f, where);
                }
                deps[cmd].push_back({f, it->second});
            }
        });
    }
    /// `cmd` will update `plan.updates` (when its encoder ends).
    void Updates(const void* cmd, const OrderPlan& plan) {
        if (plan.num_updates == 0) {
            return;
        }
        std::scoped_lock lock{mutex};
        auto& list = pending[cmd];
        for (u32 i = 0; i < plan.num_updates; ++i) {
            owner[plan.updates[i]] = cmd;
            list.push_back(plan.updates[i]);
        }
    }
    /// `cmd` is committed: its updates are on the queue.
    void Committed(const void* cmd) {
        std::scoped_lock lock{mutex};
        if (const auto d = deps.find(cmd); d != deps.end()) {
            for (const auto& [f, other] : d->second) {
                if (pending.contains(other)) {
                    Report(inversions, "commit-order inversion: committed before the command "
                                       "buffer that updates a fence it waits for", f, "commit");
                }
            }
            deps.erase(d);
        }
        const auto it = pending.find(cmd);
        if (it == pending.end()) {
            return;
        }
        for (const u32 f : it->second) {
            if (const auto o = owner.find(f); o != owner.end() && o->second == cmd) {
                owner.erase(o);
            }
        }
        pending.erase(it);
    }

private:
    FenceCheck() {
        const char* env = std::getenv("BB_METAL_FENCE_CHECK");
        on = !env || std::strcmp(env, "0") != 0;
    }
    void Report(u64& count, const char* what, u32 fence, const char* where) {
        if (++count <= 8 || (count & (count - 1)) == 0) {
            std::fprintf(stderr,
                         "Metal fence check: %s (fence %u, %s): %llu times; so far %llu "
                         "inversions, %llu slot-fence cross waits, %llu cross waits, %llu waits "
                         "on own fences\n",
                         what, fence, where, static_cast<unsigned long long>(count),
                         static_cast<unsigned long long>(inversions),
                         static_cast<unsigned long long>(slot_cross),
                         static_cast<unsigned long long>(cross_waits),
                         static_cast<unsigned long long>(self_waits));
        }
    }

    bool on{};
    std::mutex mutex;
    std::unordered_map<u32, const void*> owner;                ///< fence -> command buffer
    std::unordered_map<const void*, std::vector<u32>> pending; ///< command buffer -> fences
    /// command buffer -> (fence, uncommitted command buffer that updates it) it waits for
    std::unordered_map<const void*, std::vector<std::pair<u32, const void*>>> deps;
    u64 cross_waits{}, self_waits{}, slot_cross{}, inversions{};
};

/// bbport: BB_ENCODER_STATS=1: Metal encoders opened per frame by kind and by why the one
/// before ended (another kind, a reopen for more access or another lane, an ordering barrier or
/// wait, a fill or clamp kernel, an explicit EndEncoding, the end of a render pass). Printed
/// every 5 s. Render encoders opened in a pass that already had one are pass splits: each stores
/// and reloads the attachments on Apple's tile-based GPUs.
enum EndCause : u8 {
    CauseFresh, CauseSwitchRender, CauseSwitchCompute, CauseSwitchBlit, CauseReopenAccess,
    CauseReopenLane, CauseBarrier, CauseOrderWait, CauseFill, CauseClamp, CauseEndEncoding,
    CausePassEnd, NumCauses
};
constexpr const char* kCauseNames[NumCauses] = {
    "first", "after render", "after compute", "after blit", "reopen (access)", "reopen (lane)",
    "ordering barrier", "ordering wait", "fill kernel", "clamp kernel", "EndEncoding",
    "pass end"};
enum OpenKind : u8 { OpenRender, OpenRenderSplit, OpenCompute, OpenBlit, NumOpenKinds };
constexpr const char* kOpenNames[NumOpenKinds] = {"render (pass start)", "render (pass split)",
                                                  "compute", "blit"};
const bool encoder_stats = [] {
    const char* env = std::getenv("BB_ENCODER_STATS");
    return env && env[0] == '1';
}();
std::atomic<u64> encoder_opens[NumOpenKinds][NumCauses];
/// Callers that open a compute encoder inside a render pass (it splits the pass): two return
/// addresses up from Impl::Compute, named with dladdr when printed.
std::mutex split_callers_mutex;
std::unordered_map<u64, u64> split_callers;
std::unordered_map<u64, std::pair<void*, void*>> split_caller_frames;
void NoteSplitCaller(void* a, void* b) {
    std::scoped_lock lock{split_callers_mutex};
    const u64 key = (u64(uintptr_t(a)) << 1) ^ u64(uintptr_t(b));
    ++split_callers[key];
    split_caller_frames.try_emplace(key, a, b);
}
std::string SymbolName(void* pc) {
    Dl_info info{};
    if (!pc || !dladdr(pc, &info) || !info.dli_sname) {
        return fmt::format("{}", pc);
    }
    int status = 0;
    char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
    std::string name = status == 0 && demangled ? demangled : info.dli_sname;
    std::free(demangled);
    if (name.size() > 90) {
        name.resize(90);
    }
    return name;
}
std::atomic<u64> encoder_stats_frames{0};
std::atomic<s64> encoder_stats_since{0};

void PrintEncoderStats() {
    const s64 now = std::chrono::steady_clock::now().time_since_epoch().count();
    s64 since = encoder_stats_since.load(std::memory_order_relaxed);
    if (since == 0) {
        encoder_stats_since.compare_exchange_strong(since, now);
        return;
    }
    if (now - since < 5'000'000'000 ||
        !encoder_stats_since.compare_exchange_strong(since, now)) {
        return;
    }
    const u64 frames = std::max<u64>(1, encoder_stats_frames.exchange(0));
    u64 total = 0;
    std::string line;
    for (u32 k = 0; k < NumOpenKinds; ++k) {
        for (u32 c = 0; c < NumCauses; ++c) {
            const u64 n = encoder_opens[k][c].exchange(0);
            total += n;
            if (n * 10 >= frames) { // at least 0.1 per frame
                line += fmt::format("\n  {:6.1f}  {} {}", double(n) / frames, kOpenNames[k],
                                    kCauseNames[c]);
            }
        }
    }
    {
        std::scoped_lock lock{split_callers_mutex};
        std::vector<std::pair<u64, u64>> top(split_callers.begin(), split_callers.end());
        std::ranges::sort(top, [](const auto& x, const auto& y) { return x.second > y.second; });
        for (size_t i = 0; i < std::min<size_t>(top.size(), 6); ++i) {
            const auto [a, b] = split_caller_frames[top[i].first];
            line += fmt::format("\n  {:6.1f}  compute in a pass from {} < {}",
                                double(top[i].second) / frames, SymbolName(a), SymbolName(b));
        }
        split_callers.clear();
    }
    std::printf("Encoders: %.1f per frame over %llu frames:%s\n", double(total) / frames,
                static_cast<unsigned long long>(frames), line.c_str());
    std::fflush(stdout);
}

} // namespace

/// Frame boundary for BB_ENCODER_STATS (Presenter, at each guest flip).
void EncoderStatsFrame() {
    if (encoder_stats) {
        encoder_stats_frames.fetch_add(1, std::memory_order_relaxed);
        PrintEncoderStats();
    }
}

struct Encoder::Impl {
    explicit Impl(Device& device_, Ordering ordering)
        : device{device_}, dev{(__bridge id<MTLDevice>)device_.NativeDevice()},
          queue{(__bridge id<MTLCommandQueue>)device_.NativeQueue()}, order{device_.Order()},
          ahead{ordering == Ordering::Ahead}, fences{device_.GetImpl().fences.data()} {}

    Device& device;
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    id<MTLCommandBuffer> cmd;
    id<MTLRenderCommandEncoder> render;
    id<MTLComputeCommandEncoder> compute;
    id<MTLBlitCommandEncoder> blit;
    std::vector<Object> keep;

    // Ordering of untracked buffers (mtl_order.h); `order` is null with hazard tracking.
    OrderTracker* order;
    bool ahead;
    const id<MTLFence>* fences;
    OrderTracker::Accesses open_access{}; ///< of the open render, compute or blit encoder
    OrderPlan open_plan;                  ///< its updates, encoded when it ends
    Access render_access{Access::Write}, compute_access{Access::Write}; ///< of the pipelines
    Lane compute_lane{Lane::Compute}; ///< of the compute pipeline
    Lane open_lane{};                 ///< of the open encoder
    // The renderer's commands (SetOrderCommand): the current one, the oldest held (0: none),
    // those the open encoder holds, and whether the current one made a request.
    u64 command{}, held{}, open_first{}, open_last{};
    bool asked{};

    /// What a command needs (nothing with hazard tracking).
    OrderTracker::Accesses Need(Access memory, Access motion = Access::None) const {
        if (!order) {
            return {};
        }
        return {memory, ahead ? Access::None : motion};
    }
    /// The fences of a Metal encoder opening with `need`.
    OrderPlan Plan(const OrderTracker::Accesses& need, Lane lane) {
        if (!order || (need[0] == Access::None && need[1] == Access::None)) {
            return {};
        }
        if (ahead) {
            if (need[u32(HazardClass::Memory)] == Access::None) {
                return {};
            }
            return order->OpenAhead();
        }
        open_first = held ? std::min(held, command) : command;
        open_last = command;
        return order->Open(need, lane, device.CurrentTick(), device.KnownGpuTick(), open_first);
    }
    /// The open encoder encodes work of the current command (and of the held ones).
    void Touch() {
        open_last = command;
        if (held && held < open_first) {
            open_first = held;
        }
    }
    template <typename E>
    void BeginOrdered(E e, const OrderTracker::Accesses& need, Lane lane) {
        open_access = need;
        open_lane = lane;
        open_plan = Plan(need, lane);
        if (FenceCheck::Get().On() && !open_plan.Empty()) {
            FenceCheck::Get().Waits((__bridge void*)cmd, open_plan, open_plan, "encoder open");
            FenceCheck::Get().Updates((__bridge void*)cmd, open_plan);
        }
        WaitFences(e, open_plan, fences);
    }
    /// After a command: an isolated write ends its encoder, so the next command opens another
    /// one after the barrier (mtl_order.h).
    void AfterCommand(Access memory) {
        if (order && !ahead && memory == Access::Write) {
            EndRender();
            EndCompute();
            EndBlit();
        }
    }
    template <typename E>
    void EndOrdered(E e) {
        UpdateFences(e, open_plan, fences);
        if (order && !ahead && !open_plan.Empty()) {
            order->Close(open_first, open_last);
        }
        open_plan = {};
        open_access = {};
    }

    // Render pass (Task 5).
    bool in_pass{}, pass_opened{}, pass_clears{};
    EndCause last_end = CauseFresh; ///< why the last encoder ended (BB_ENCODER_STATS)
    /// Ends every open encoder for `cause` (BB_ENCODER_STATS records it for the next open).
    void EndAll(EndCause cause) {
        if (render || compute || blit) {
            last_end = cause;
        }
        FlushPendingClear();
        EndRender();
        EndCompute();
        EndBlit();
    }
    void NoteOpen(OpenKind kind, EndCause cause) {
        if (encoder_stats) {
            encoder_opens[kind][cause].fetch_add(1, std::memory_order_relaxed);
        }
        last_end = CauseFresh;
    }
    bool pass_resolves{}; ///< a colour attachment resolves at the end of the pass
    MTLRenderPassDescriptor* pass_first; ///< as begun, with its clears
    MTLRenderPassDescriptor* pass_load;  ///< the same with Load actions (reopening)
    u32 target_width{}, target_height{}; ///< of the pass (the scissor is clamped to it)

    // Render state (Task 5): what the caller set, and what changed since the encoder saw it.
    enum RenderBit : u32 {
        BitPipeline = 1u << 0,
        BitDepthStencil = 1u << 1,
        BitViewport = 1u << 2,
        BitScissor = 1u << 3,
        BitCull = 1u << 4,
        BitWinding = 1u << 5,
        BitBias = 1u << 6,
        BitStencilRef = 1u << 7,
        BitBlend = 1u << 8,
        BitFill = 1u << 9,
        BitDepthClip = 1u << 10,
        BitTessFactors = 1u << 11,
    };
    u32 render_dirty{}, render_used{};
    void* render_pipeline{};
    void* depth_stencil{};
    Viewport viewport{};
    Scissor scissor{};
    CullMode cull{CullMode::None};
    Winding winding{Winding::CounterClockwise};
    float bias[3]{}; ///< constant, slope, clamp
    u32 stencil_front{}, stencil_back{};
    float blend[4]{};
    FillMode fill{FillMode::Fill};
    DepthClip depth_clip{DepthClip::Clip};
    void* tess_factors{};
    u64 tess_offset{};
    u32 tess_stride{};
    StageState vertex, fragment;

    // Compute state (Task 4).
    void* compute_pipeline{};
    bool compute_pipeline_dirty{};
    StageState compute_stage;

    struct Indirect {
        void* buffer;
        IndirectUse use;
    };
    std::vector<Indirect> indirect; ///< declared on every encoder opened (survives Submit)
    void* pass_vertex_rw{};         ///< RenderPassDesc::vertex_rw_buffer of the open pass
    id<MTLComputePipelineState> fill_u32;
    id<MTLComputePipelineState> clamp_count;

    /// Debug groups still open: on the command buffer (`encoder` null) or on the Metal encoder
    /// that was open when pushed (cleared when that encoder ends and closes the group).
    struct DebugGroup {
        bool on_cmd;
        void* encoder;
    };
    std::vector<DebugGroup> groups;

    /// An encoder is about to end: close the groups pushed on it.
    void CloseGroups(id encoder) {
        for (size_t i = groups.size(); i-- > 0;) {
            if (!groups[i].on_cmd && groups[i].encoder == (__bridge void*)encoder) {
                [(id<MTLCommandEncoder>)encoder popDebugGroup];
                groups[i].encoder = nullptr;
            }
        }
    }

    static void Declare(id<MTLRenderCommandEncoder> e, const Indirect& i) {
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)i.buffer;
        if (i.use == IndirectUse::VertexWrite) {
            // Written and read by vertex shaders only: render passes order against each other
            // at the vertex stage, so a later pass's vertex work still overlaps the fragment
            // work of the one before it.
            [e useResource:b
                     usage:MTLResourceUsageRead | MTLResourceUsageWrite
                    stages:MTLRenderStageVertex];
        } else {
            [e useResource:b
                     usage:MTLResourceUsageRead
                    stages:MTLRenderStageVertex | MTLRenderStageFragment];
        }
    }
    static void Declare(id<MTLComputeCommandEncoder> e, const Indirect& i) {
        [e useResource:(__bridge id<MTLBuffer>)i.buffer
                 usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    }
    // bbport: the read-only ones in one call (guest memory can be several buffers: the buffer
    // cache's chunks); one useResources call of three costs about what one useResource does
    // (tests/test_arena_accounting.mm declare).
    static constexpr size_t MaxBatched = 16;
    void DeclareOn(id<MTLRenderCommandEncoder> e) {
        __unsafe_unretained id<MTLResource> reads[MaxBatched];
        NSUInteger count = 0;
        for (const auto& i : indirect) {
            if (i.use == IndirectUse::Read && count < MaxBatched) {
                reads[count++] = (__bridge id<MTLBuffer>)i.buffer;
            } else {
                Declare(e, i);
            }
        }
        if (count == 1) {
            [e useResource:reads[0]
                     usage:MTLResourceUsageRead
                    stages:MTLRenderStageVertex | MTLRenderStageFragment];
        } else if (count > 1) {
            [e useResources:reads
                      count:count
                      usage:MTLResourceUsageRead
                     stages:MTLRenderStageVertex | MTLRenderStageFragment];
        }
    }
    void DeclareOn(id<MTLComputeCommandEncoder> e) {
        __unsafe_unretained id<MTLResource> all[MaxBatched];
        NSUInteger count = 0;
        for (const auto& i : indirect) {
            if (count < MaxBatched) {
                all[count++] = (__bridge id<MTLBuffer>)i.buffer;
            } else {
                Declare(e, i);
            }
        }
        if (count == 1) {
            [e useResource:all[0] usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        } else if (count > 1) {
            [e useResources:all count:count usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        }
    }

    /// A new command buffer starts unbound (as in Vulkan).
    void ResetBindings() {
        vertex = {};
        fragment = {};
        compute_stage = {};
        render_pipeline = depth_stencil = compute_pipeline = nullptr;
        render_used = render_dirty = 0;
        compute_pipeline_dirty = false;
        viewport = {};
        scissor = {};
        cull = CullMode::None;
        winding = Winding::CounterClockwise;
        bias[0] = bias[1] = bias[2] = 0;
        stencil_front = stencil_back = 0;
        blend[0] = blend[1] = blend[2] = blend[3] = 0;
        fill = FillMode::Fill;
        depth_clip = DepthClip::Clip;
        tess_factors = nullptr;
        tess_offset = 0;
        tess_stride = 0;
    }

    id<MTLComputePipelineState> ClampCountPipeline() {
        if (!clamp_count) {
            @autoreleasepool {
                NSError* error = nil;
                id<MTLLibrary> lib = [dev newLibraryWithSource:@(kClampCountSource)
                                                       options:nil
                                                         error:&error];
                ASSERT_MSG(lib, "clamp_indirect_count kernel: {}",
                           error ? error.localizedDescription.UTF8String : "unknown");
                clamp_count = [dev newComputePipelineStateWithFunction:
                                       [lib newFunctionWithName:@"clamp_indirect_count"]
                                                                 error:&error];
                ASSERT_MSG(clamp_count, "clamp_indirect_count pipeline: {}",
                           error ? error.localizedDescription.UTF8String : "unknown");
            }
        }
        return clamp_count;
    }

    id<MTLComputePipelineState> FillPipeline() {
        if (!fill_u32) {
            @autoreleasepool {
                NSError* error = nil;
                id<MTLLibrary> lib = [dev newLibraryWithSource:@R"(
#include <metal_stdlib>
using namespace metal;
kernel void fill_u32(device uint* dst [[buffer(0)]], constant uint2& args [[buffer(1)]],
                     uint i [[thread_position_in_grid]]) {
    if (i < args.y) dst[i] = args.x;
}
)" options:nil error:&error];
                ASSERT_MSG(lib, "fill_u32 kernel: {}",
                           error ? error.localizedDescription.UTF8String : "unknown");
                fill_u32 = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill_u32"]
                                                              error:&error];
                ASSERT_MSG(fill_u32, "fill_u32 pipeline: {}",
                           error ? error.localizedDescription.UTF8String : "unknown");
            }
        }
        return fill_u32;
    }

    /// Ends encoding and commits; `tracked` takes a tick. Returns 0 for an untracked commit.
    u64 Finish(bool tracked) {
        if (in_pass) {
            // bbport: a submission while a pass is open: the scheduler had already counted its
            // pass as ended, so another thread's BeginRenderPass was recorded at the same time
            // (WarnPassMismatch: likely the recording-chunk race). A Metal command buffer cannot
            // commit with a render encoder open, but the pass survives it: the encoder is closed
            // here and the next draw reopens the pass with load actions, as after compute or
            // blit work inside a pass. Clears not yet recorded are kept for that reopening
            // (pass_opened stays false); the pass ends at the scheduler's next EndRenderPass or
            // BeginRenderPass.
            WarnPassMismatch("Submit inside a render pass: the pass continues in the next "
                             "command buffer",
                             pass_opened ? "opened" : "not opened yet");
        }
        EndRender();
        EndCompute();
        EndBlit();
        id<MTLCommandBuffer> c = Cmd();
        for (size_t i = groups.size(); i-- > 0;) {
            if (groups[i].on_cmd) {
                [c popDebugGroup]; // unbalanced pushes must not leave the command buffer open
            }
        }
        groups.clear();
        auto* held = new std::vector<Object>(std::move(keep));
        keep.clear();
        [c addCompletedHandler:^(id<MTLCommandBuffer>) {
            delete held;
        }];
        if (FenceCheck::Get().On()) {
            FenceCheck::Get().Committed((__bridge void*)c);
        }
        u64 tick = 0;
        if (tracked) {
            tick = device.Commit((__bridge void*)c);
        } else {
            device.CommitUntracked((__bridge void*)c);
        }
        cmd = nil;
        ResetBindings();
        return tick;
    }

    id<MTLCommandBuffer> Cmd() {
        if (!cmd) {
            @autoreleasepool {
                cmd = [queue commandBufferWithUnretainedReferences];
            }
        }
        return cmd;
    }

    void EndRender() {
        if (render) {
            CloseGroups(render);
            EndOrdered(render);
            [render endEncoding];
            render = nil;
        }
    }
    void EndCompute() {
        if (compute) {
            CloseGroups(compute);
            EndOrdered(compute);
            [compute endEncoding];
            compute = nil;
        }
    }
    void EndBlit() {
        if (blit) {
            CloseGroups(blit);
            EndOrdered(blit);
            [blit endEncoding];
            blit = nil;
        }
    }

    /// Work recorded inside a pass that has not opened yet must come after the pass's clears.
    void FlushPendingClear() {
        if (in_pass && !pass_opened && pass_clears) {
            Render();
        }
    }

    TimestampSink* sink{}; ///< SetTimestampSink

    /// Fills `a` (MTL{Compute,Blit}PassSampleBufferAttachmentDescriptor) when the sink has room.
    template <typename Attachment>
    bool SampleEncoder(Attachment* a) {
        void* buffer = nullptr;
        u32 index = 0;
        if (!sink || !sink->Reserve(buffer, index, 2)) {
            return false;
        }
        a.sampleBuffer = (__bridge id<MTLCounterSampleBuffer>)buffer;
        a.startOfEncoderSampleIndex = index;
        a.endOfEncoderSampleIndex = index + 1;
        return true;
    }

    /// A new compute encoder on the command buffer (sampled when a sink is set).
    id<MTLComputeCommandEncoder> NewCompute() {
        if (sink) {
            MTLComputePassDescriptor* d = [MTLComputePassDescriptor computePassDescriptor];
            if (SampleEncoder(d.sampleBufferAttachments[0])) {
                return [Cmd() computeCommandEncoderWithDescriptor:d];
            }
        }
        return [Cmd() computeCommandEncoder];
    }

    /// Sets (or clears) the sample attachment of a render pass descriptor about to be encoded.
    void SampleRenderPass(MTLRenderPassDescriptor* pass) {
        MTLRenderPassSampleBufferAttachmentDescriptor* a = pass.sampleBufferAttachments[0];
        void* buffer = nullptr;
        u32 index = 0;
        if (sink && sink->Reserve(buffer, index, 3)) {
            a.sampleBuffer = (__bridge id<MTLCounterSampleBuffer>)buffer;
            a.startOfVertexSampleIndex = index;
            a.endOfVertexSampleIndex = MTLCounterDontSample;
            a.startOfFragmentSampleIndex = index + 1;
            a.endOfFragmentSampleIndex = index + 2;
        } else if (a.sampleBuffer) {
            a.sampleBuffer = nil;
        }
    }

    /// The open blit encoder, reopened when it was opened for less than `access` needs.
    id<MTLBlitCommandEncoder> Blit(Access access = Access::None) {
        const OrderTracker::Accesses need = Need(access);
        EndCause cause = last_end;
        if (blit && !Covers(open_access, need)) {
            EndBlit();
            cause = CauseReopenAccess;
        }
        if (!blit) {
            cause = render ? CauseSwitchRender : compute ? CauseSwitchCompute : cause;
            NoteOpen(OpenBlit, cause);
            FlushPendingClear();
            EndRender();
            EndCompute();
            @autoreleasepool {
                MTLBlitPassDescriptor* d = sink ? [MTLBlitPassDescriptor blitPassDescriptor] : nil;
                blit = d && SampleEncoder(d.sampleBufferAttachments[0])
                           ? [Cmd() blitCommandEncoderWithDescriptor:d]
                           : [Cmd() blitCommandEncoder];
            }
            BeginOrdered(blit, need, Lane::Copy);
        }
        Touch();
        return blit;
    }

    /// The open compute encoder, reopened when it was opened for less than `access` needs.
    id<MTLComputeCommandEncoder> Compute(Access access, Lane lane) {
        const OrderTracker::Accesses need = Need(access);
        EndCause cause = last_end;
        if (compute && (!Covers(open_access, need) || (order && open_lane != lane))) {
            cause = Covers(open_access, need) ? CauseReopenLane : CauseReopenAccess;
            EndCompute();
        }
        if (!compute) {
            cause = render ? CauseSwitchRender : blit ? CauseSwitchBlit : cause;
            NoteOpen(OpenCompute, cause);
            if (encoder_stats && in_pass) {
                NoteSplitCaller(__builtin_return_address(1), __builtin_return_address(2));
            }
            FlushPendingClear();
            EndRender();
            EndBlit();
            @autoreleasepool {
                compute = NewCompute();
            }
            BeginOrdered(compute, need, lane);
            DeclareOn(compute);
            compute_stage.Reapply();
            compute_pipeline_dirty = compute_pipeline != nullptr;
        }
        Touch();
        return compute;
    }

    void ApplyCompute() {
        id<MTLComputeCommandEncoder> e = Compute(compute_access, compute_lane);
        if (compute_pipeline_dirty) {
            [e setComputePipelineState:(__bridge id<MTLComputePipelineState>)compute_pipeline];
            compute_pipeline_dirty = false;
        }
        StageState& s = compute_stage;
        ForEachBit(s.buffers_dirty, [&](u32 slot) {
            const StageState::Slot& b = s.buffers[slot];
            if (!b.bytes.empty()) {
                [e setBytes:b.bytes.data() length:b.bytes.size() atIndex:slot];
            } else {
                [e setBuffer:(__bridge id<MTLBuffer>)b.buffer offset:b.offset atIndex:slot];
            }
        });
        s.textures_dirty.ForEach([&](u32 slot) {
            [e setTexture:(__bridge id<MTLTexture>)s.textures[slot] atIndex:slot];
        });
        ForEachBit(s.samplers_dirty, [&](u32 slot) {
            [e setSamplerState:(__bridge id<MTLSamplerState>)s.samplers[slot] atIndex:slot];
        });
        s.buffers_dirty = s.samplers_dirty = 0;
        s.textures_dirty.Clear();
    }

    /// The open render encoder of the pass, reopened (Load) when it was opened for less than the
    /// draw needs (`draw`: the pipeline's access; false: clears and resolves only).
    id<MTLRenderCommandEncoder> Render(bool draw = false) {
        const OrderTracker::Accesses need =
            draw ? Need(render_access, pass_vertex_rw ? Access::Read : Access::None) : Need(Access::None);
        EndCause cause = last_end;
        if (render && !Covers(open_access, need)) {
            EndRender();
            cause = CauseReopenAccess;
        }
        if (!render) {
            ASSERT_MSG(in_pass, "Draw outside a render pass");
            cause = compute ? CauseSwitchCompute : blit ? CauseSwitchBlit : cause;
            NoteOpen(pass_opened ? OpenRenderSplit : OpenRender, cause);
            EndCompute();
            EndBlit();
            @autoreleasepool {
                MTLRenderPassDescriptor* pass = pass_opened ? pass_load : pass_first;
                if (sink || pass.sampleBufferAttachments[0].sampleBuffer) {
                    SampleRenderPass(pass);
                }
                render = [Cmd() renderCommandEncoderWithDescriptor:pass];
            }
            BeginOrdered(render, need, Lane::Render);
            pass_opened = true;
            DeclareOn(render);
            if (pass_vertex_rw) {
                Declare(render, Indirect{pass_vertex_rw, IndirectUse::VertexWrite});
            }
            render_dirty = render_used; // incl. the scissor, clamped to this pass's target
            vertex.Reapply();
            fragment.Reapply();
        }
        if (draw) {
            Touch();
        }
        return render;
    }

    void ApplyStage(id<MTLRenderCommandEncoder> e, StageState& s, bool is_vertex) {
        ForEachBit(s.buffers_dirty, [&](u32 slot) {
            const StageState::Slot& b = s.buffers[slot];
            if (!b.bytes.empty()) {
                if (is_vertex) {
                    [e setVertexBytes:b.bytes.data() length:b.bytes.size() atIndex:slot];
                } else {
                    [e setFragmentBytes:b.bytes.data() length:b.bytes.size() atIndex:slot];
                }
            } else if (is_vertex && b.attribute_stride) {
                [e setVertexBuffer:(__bridge id<MTLBuffer>)b.buffer
                            offset:b.offset
                    attributeStride:b.stride
                           atIndex:slot];
            } else if (is_vertex) {
                [e setVertexBuffer:(__bridge id<MTLBuffer>)b.buffer offset:b.offset atIndex:slot];
            } else {
                [e setFragmentBuffer:(__bridge id<MTLBuffer>)b.buffer offset:b.offset atIndex:slot];
            }
        });
        s.textures_dirty.ForEach([&](u32 slot) {
            id<MTLTexture> t = (__bridge id<MTLTexture>)s.textures[slot];
            if (is_vertex) {
                [e setVertexTexture:t atIndex:slot];
            } else {
                [e setFragmentTexture:t atIndex:slot];
            }
        });
        ForEachBit(s.samplers_dirty, [&](u32 slot) {
            id<MTLSamplerState> st = (__bridge id<MTLSamplerState>)s.samplers[slot];
            if (is_vertex) {
                [e setVertexSamplerState:st atIndex:slot];
            } else {
                [e setFragmentSamplerState:st atIndex:slot];
            }
        });
        s.buffers_dirty = s.samplers_dirty = 0;
        s.textures_dirty.Clear();
    }

    void ApplyRender() {
        id<MTLRenderCommandEncoder> e = Render(true);
        const u32 d = render_dirty;
        if (d & BitPipeline) {
            [e setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)render_pipeline];
        }
        if (d & BitDepthStencil) {
            // Metal takes no nil state: "none" is a state that always passes and writes nothing.
            [e setDepthStencilState:depth_stencil ? (__bridge id<MTLDepthStencilState>)depth_stencil
                                                  : NoDepthStencil()];
        }
        if (d & BitViewport) {
            [e setViewport:(MTLViewport){viewport.x, viewport.y, viewport.width, viewport.height,
                                         viewport.min_depth, viewport.max_depth}];
        }
        if (d & BitScissor) {
            // Metal requires the scissor inside the target (Vulkan does not); state carried from
            // a bigger pass is common.
            const u32 x = std::min(scissor.x, target_width);
            const u32 y = std::min(scissor.y, target_height);
            [e setScissorRect:(MTLScissorRect){x, y, std::min(scissor.width, target_width - x),
                                               std::min(scissor.height, target_height - y)}];
        }
        if (d & BitCull) {
            [e setCullMode:ToMtl(cull)];
        }
        if (d & BitWinding) {
            [e setFrontFacingWinding:winding == Winding::Clockwise ? MTLWindingClockwise
                                                                    : MTLWindingCounterClockwise];
        }
        if (d & BitBias) {
            [e setDepthBias:bias[0] slopeScale:bias[1] clamp:bias[2]];
        }
        if (d & BitStencilRef) {
            [e setStencilFrontReferenceValue:stencil_front backReferenceValue:stencil_back];
        }
        if (d & BitBlend) {
            [e setBlendColorRed:blend[0] green:blend[1] blue:blend[2] alpha:blend[3]];
        }
        if (d & BitFill) {
            [e setTriangleFillMode:fill == FillMode::Lines ? MTLTriangleFillModeLines
                                                           : MTLTriangleFillModeFill];
        }
        if (d & BitDepthClip) {
            [e setDepthClipMode:depth_clip == DepthClip::Clamp ? MTLDepthClipModeClamp
                                                               : MTLDepthClipModeClip];
        }
        if ((d & BitTessFactors) && tess_factors) {
            [e setTessellationFactorBuffer:(__bridge id<MTLBuffer>)tess_factors
                                    offset:tess_offset
                            instanceStride:tess_stride];
        }
        render_dirty = 0;
        ApplyStage(e, vertex, true);
        ApplyStage(e, fragment, false);
    }

    id<MTLDepthStencilState> NoDepthStencil() {
        if (!no_depth_stencil) {
            no_depth_stencil =
                [dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        }
        return no_depth_stencil;
    }
    id<MTLDepthStencilState> no_depth_stencil; ///< SetDepthStencil(nullptr): no test, no writes

    void MarkRender(u32 bit) {
        render_dirty |= bit;
        render_used |= bit;
    }

    StageState& StateOf(Stage stage) {
        switch (stage) {
        case Stage::Vertex:
            return vertex;
        case Stage::Fragment:
            return fragment;
        case Stage::Compute:
            return compute_stage;
        }
        UNREACHABLE();
    }
};

Encoder::Encoder(Device& device, Ordering ordering)
    : impl{std::make_unique<Impl>(device, ordering)} {}

Encoder::~Encoder() {
    if (impl->in_pass) {
        EndRenderPass();
    }
    if (impl->cmd) {
        Submit(); // nothing recorded is lost, and the keep-alive list is released after the GPU
    }
}

// ---- Blits ----

void Encoder::CopyBuffer(BufferRef src, BufferRef dst, u64 size) {
    if (size == 0) {
        return; // Metal rejects empty blits (MTL_DEBUG_LAYER aborts)
    }
    const Access access = BlitAccess(impl->device, src.buffer, dst.buffer);
    [impl->Blit(access) copyFromBuffer:(__bridge id<MTLBuffer>)src.buffer
                    sourceOffset:src.offset
                        toBuffer:(__bridge id<MTLBuffer>)dst.buffer
               destinationOffset:dst.offset
                            size:size];
    impl->AfterCommand(access);
}

void Encoder::FillBuffer(BufferRef dst, u64 size, u32 pattern) {
    if (size == 0) {
        return; // Metal rejects empty blits (MTL_DEBUG_LAYER aborts)
    }
    if (pattern == u32(u8(pattern)) * 0x01010101u) {
        const Access access = BlitAccess(impl->device, nullptr, dst.buffer);
        [impl->Blit(access) fillBuffer:(__bridge id<MTLBuffer>)dst.buffer
                           range:NSMakeRange(dst.offset, size)
                           value:u8(pattern)];
        impl->AfterCommand(access);
        return;
    }
    // Guest sizes (HTile clears) need not be whole words: fill the whole words only.
    if (dst.offset % 4 != 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            LOG_WARNING(Render, "FillBuffer: pattern fill at unaligned offset {:#x} skipped",
                        dst.offset);
        }
        return;
    }
    if (size % 4 != 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            LOG_WARNING(Render, "FillBuffer: pattern fill of {} bytes rounded down to {}", size,
                        size & ~u64{3});
        }
        size &= ~u64{3};
        if (size == 0) {
            return;
        }
    }
    // A dedicated compute encoder, so the caller's compute state is untouched.
    impl->EndAll(CauseFill);
    id<MTLComputePipelineState> pipeline = impl->FillPipeline();
    const u32 args[2] = {pattern, u32(size / 4)};
    @autoreleasepool {
        id<MTLComputeCommandEncoder> e = impl->NewCompute();
        impl->BeginOrdered(e, impl->Need(BlitAccess(impl->device, nullptr, dst.buffer)), Lane::Copy);
        [e setComputePipelineState:pipeline];
        [e setBuffer:(__bridge id<MTLBuffer>)dst.buffer offset:dst.offset atIndex:0];
        [e setBytes:args length:sizeof(args) atIndex:1];
        [e dispatchThreads:MTLSizeMake(size / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        impl->EndOrdered(e);
        [e endEncoding];
    }
}

void Encoder::CopyBufferToTexture(BufferRef src, u32 bytes_per_row, u32 bytes_per_image,
                                  void* texture, u32 level, u32 slice, Origin3 origin, Size3 size,
                                  BlitAspect aspect) {
    [impl->Blit(BlitAccess(impl->device, src.buffer, nullptr)) copyFromBuffer:(__bridge id<MTLBuffer>)src.buffer
                    sourceOffset:src.offset
               sourceBytesPerRow:bytes_per_row
             sourceBytesPerImage:bytes_per_image
                      sourceSize:MTLSizeMake(size.width, size.height, size.depth)
                       toTexture:(__bridge id<MTLTexture>)texture
                destinationSlice:slice
                destinationLevel:level
               destinationOrigin:MTLOriginMake(origin.x, origin.y, origin.z)
                        options:BlitOptions(aspect)];
}

void Encoder::CopyTextureToBuffer(void* texture, u32 level, u32 slice, Origin3 origin, Size3 size,
                                  BufferRef dst, u32 bytes_per_row, u32 bytes_per_image,
                                  BlitAspect aspect) {
    const Access access = BlitAccess(impl->device, nullptr, dst.buffer);
    [impl->Blit(access) copyFromTexture:(__bridge id<MTLTexture>)texture
                      sourceSlice:slice
                      sourceLevel:level
                     sourceOrigin:MTLOriginMake(origin.x, origin.y, origin.z)
                       sourceSize:MTLSizeMake(size.width, size.height, size.depth)
                         toBuffer:(__bridge id<MTLBuffer>)dst.buffer
                destinationOffset:dst.offset
           destinationBytesPerRow:bytes_per_row
         destinationBytesPerImage:bytes_per_image
                          options:BlitOptions(aspect)];
    impl->AfterCommand(access);
}

void Encoder::CopyTexture(void* src, u32 src_level, u32 src_slice, Origin3 src_origin, void* dst,
                          u32 dst_level, u32 dst_slice, Origin3 dst_origin, Size3 size) {
    [impl->Blit() copyFromTexture:(__bridge id<MTLTexture>)src
                      sourceSlice:src_slice
                      sourceLevel:src_level
                     sourceOrigin:MTLOriginMake(src_origin.x, src_origin.y, src_origin.z)
                       sourceSize:MTLSizeMake(size.width, size.height, size.depth)
                        toTexture:(__bridge id<MTLTexture>)dst
                 destinationSlice:dst_slice
                 destinationLevel:dst_level
                destinationOrigin:MTLOriginMake(dst_origin.x, dst_origin.y, dst_origin.z)];
}

// ---- Command buffer ----

void Encoder::KeepAlive(Object object) {
    impl->keep.push_back(std::move(object));
}

void Encoder::WaitForDeviceTick(u64 device_tick) {
    if (device_tick == 0) {
        return;
    }
    ASSERT_MSG(device_tick < impl->device.CurrentTick(),
               "Waiting for device tick {} that was never committed (it would never signal)",
               device_tick);
    impl->FlushPendingClear(); // commands recorded before the wait stay before it
    EndEncoding();
    [impl->Cmd() encodeWaitForEvent:impl->device.GetImpl().event value:device_tick];
}

void Encoder::EndEncoding() {
    // A pass whose encoder has not opened yet still owes its clears: run them first, so work
    // encoded outside the Encoder next (MetalFX) comes after them.
    impl->EndAll(CauseEndEncoding);
}

void* Encoder::CommandBuffer() {
    return (__bridge void*)impl->Cmd();
}

u64 Encoder::Submit() {
    return impl->Finish(true);
}

void Encoder::SubmitUntracked() {
    impl->Finish(false);
}

void Encoder::SetTimestampSink(TimestampSink* sink) {
    impl->sink = sink;
}

void Encoder::DeclareIndirect(void* buffer, IndirectUse use) {
    const auto it = std::find_if(impl->indirect.begin(), impl->indirect.end(),
                                 [buffer](const Impl::Indirect& i) { return i.buffer == buffer; });
    if (it != impl->indirect.end() && it->use == use) {
        return;
    }
    const Impl::Indirect entry{buffer, use};
    if (it != impl->indirect.end()) {
        *it = entry;
    } else {
        impl->indirect.push_back(entry);
    }
    if (impl->render) {
        Impl::Declare(impl->render, entry);
    }
    if (impl->compute) {
        Impl::Declare(impl->compute, entry);
    }
}

void Encoder::RemoveIndirect(void* buffer) {
    std::erase_if(impl->indirect, [buffer](const Impl::Indirect& i) { return i.buffer == buffer; });
}

void Encoder::OrderBarrier(HazardClass c, BarrierScopes scopes) {
    if (!impl->order || impl->ahead) {
        return;
    }
    if (scopes == ScopeNone) {
        return;
    }
    // Commands after the barrier must not share an encoder with the ones before it (the pass
    // continues with Load, as after a blit).
    if (impl->open_access[u32(c)] != Access::None) {
        impl->EndAll(CauseBarrier);
    }
    impl->order->Barrier(c, scopes);
}

void Encoder::SetOrderCommand(u64 command) {
    if (command == impl->command) {
        return;
    }
    if (impl->asked) {
        impl->order->EndCommand();
        impl->asked = false;
    }
    impl->command = command;
}

u64 Encoder::OrderCommand() const noexcept {
    return impl->command;
}

void Encoder::HoldOrderCommand() {
    if (!impl->held) {
        impl->held = impl->command;
    }
}

void Encoder::ReleaseOrderCommand() {
    impl->held = 0;
}

void Encoder::OrderWait(const OrderRequest& request) {
    if (!impl->order || impl->ahead || request.Empty()) {
        return;
    }
    impl->asked = true;
    const OrderTracker::RequestPlan plan = impl->order->Request(
        request, impl->device.KnownGpuTick(), impl->open_first, impl->open_last);
    if (plan.end_open) {
        // The next Metal encoder waits (a pass continues with Load, as after a blit).
        impl->EndAll(CauseOrderWait);
    } else if (plan.waits.num_waits) {
        // The open encoder waits before the command's work (a pass goes on).
        if (FenceCheck::Get().On()) {
            FenceCheck::Get().Waits((__bridge void*)impl->cmd, plan.waits, impl->open_plan,
                                    "mid-encoder");
        }
        if (impl->render) {
            WaitFences(impl->render, plan.waits, impl->fences);
        } else if (impl->compute) {
            WaitFences(impl->compute, plan.waits, impl->fences);
        } else if (impl->blit) {
            WaitFences(impl->blit, plan.waits, impl->fences);
        }
    }
}

bool Encoder::HasBindings() const noexcept {
    const auto used = [](const StageState& s) {
        return s.buffers_used | s.textures_used.Any() | s.samplers_used;
    };
    return impl->render_used || impl->compute_pipeline || used(impl->vertex) ||
           used(impl->fragment) || used(impl->compute_stage);
}

// ---- Compute (Task 4) ----

void Encoder::SetBuffer(Stage stage, u32 slot, BufferRef buffer) {
    ASSERT(slot < MaxBuffers);
    StageState& s = impl->StateOf(stage);
    StageState::Slot& b = s.buffers[slot];
    b.buffer = buffer.buffer;
    b.offset = buffer.offset;
    b.stride = 0;
    b.attribute_stride = false;
    b.bytes.clear();
    s.buffers_dirty |= 1u << slot;
    s.buffers_used |= 1u << slot;
}

void Encoder::SetVertexBuffer(u32 slot, BufferRef buffer, u32 stride) {
    SetBuffer(Stage::Vertex, slot, buffer);
    impl->vertex.buffers[slot].stride = stride;
    impl->vertex.buffers[slot].attribute_stride = true;
}

void Encoder::SetBytes(Stage stage, u32 slot, const void* data, u32 size) {
    ASSERT(slot < MaxBuffers && size > 0);
    if (size > MaxInlineBytes) {
        id<MTLBuffer> temp;
        @autoreleasepool {
            temp = [impl->dev newBufferWithBytes:data
                                          length:size
                                         options:MTLResourceStorageModeShared];
        }
        SetBuffer(stage, slot, {(__bridge void*)temp, 0});
        KeepAlive(Object::Retain((__bridge void*)temp));
        return;
    }
    StageState& s = impl->StateOf(stage);
    StageState::Slot& b = s.buffers[slot];
    b.buffer = nullptr;
    b.offset = 0;
    b.stride = 0;
    b.attribute_stride = false;
    b.bytes.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
    s.buffers_dirty |= 1u << slot;
    s.buffers_used |= 1u << slot;
}

void Encoder::SetTexture(Stage stage, u32 slot, void* texture) {
    ASSERT(slot < MaxTextures);
    StageState& s = impl->StateOf(stage);
    s.textures[slot] = texture;
    s.textures_dirty.Set(slot);
    s.textures_used.Set(slot);
}

void Encoder::SetSampler(Stage stage, u32 slot, void* sampler) {
    ASSERT(slot < MaxSamplers);
    StageState& s = impl->StateOf(stage);
    s.samplers[slot] = sampler;
    s.samplers_dirty |= 1u << slot;
    s.samplers_used |= 1u << slot;
}

void Encoder::SetComputePipeline(void* pipeline, Access buffers, Lane lane) {
    impl->compute_access = buffers;
    impl->compute_lane = lane;
    if (pipeline != impl->compute_pipeline) {
        impl->compute_pipeline = pipeline;
        impl->compute_pipeline_dirty = true;
    }
}

void Encoder::Dispatch(Size3 groups, Size3 threads_per_group) {
    ASSERT_MSG(impl->compute_pipeline, "Dispatch without a compute pipeline");
    impl->ApplyCompute();
    [impl->compute dispatchThreadgroups:MTLSizeMake(groups.width, groups.height, groups.depth)
                  threadsPerThreadgroup:MTLSizeMake(threads_per_group.width,
                                                    threads_per_group.height,
                                                    threads_per_group.depth)];
    impl->AfterCommand(impl->compute_access);
}

void Encoder::DispatchIndirect(BufferRef args, Size3 threads_per_group) {
    ASSERT_MSG(impl->compute_pipeline, "Dispatch without a compute pipeline");
    args = LowOffset(args); // 32-bit indirect offsets (mtl_buffer.h)
    impl->ApplyCompute();
    [impl->compute dispatchThreadgroupsWithIndirectBuffer:(__bridge id<MTLBuffer>)args.buffer
                                     indirectBufferOffset:args.offset
                                    threadsPerThreadgroup:MTLSizeMake(threads_per_group.width,
                                                                      threads_per_group.height,
                                                                      threads_per_group.depth)];
    impl->AfterCommand(impl->compute_access);
}

void* Encoder::ComputeEncoder(Access buffers) {
    id<MTLComputeCommandEncoder> e = impl->Compute(buffers, Lane::Compute);
    // The caller's kernels change the pipeline and slots: its next Dispatch applies them again.
    impl->compute_stage.Reapply();
    impl->compute_pipeline_dirty = impl->compute_pipeline != nullptr;
    return (__bridge void*)e;
}

// ---- Render passes and draws (Task 5) ----

void Encoder::BeginRenderPass(const RenderPassDesc& desc) {
    if (impl->in_pass) {
        // bbport: a pass the scheduler did not count is still open (the same mismatch Finish
        // survives; nothing else closes it). It ends here as EndRenderPass would end it, clears
        // and resolves included, and the new pass begins.
        WarnPassMismatch("BeginRenderPass inside a render pass: the open pass ends first",
                         impl->pass_opened ? "opened" : "not opened yet");
        EndRenderPass();
    }
    EndEncoding();
    MTLRenderPassDescriptor* first = [MTLRenderPassDescriptor renderPassDescriptor];
    bool clears = false, attachments = false, resolves = false;
    for (u32 i = 0; i < RenderPassDesc::MaxColors; ++i) {
        const ColorAttachment& c = desc.colors[i];
        if (!c.texture) {
            continue;
        }
        attachments = true;
        MTLRenderPassColorAttachmentDescriptor* a = first.colorAttachments[i];
        a.texture = (__bridge id<MTLTexture>)c.texture;
        a.level = c.level;
        a.slice = c.slice;
        a.depthPlane = c.depth_plane;
        a.loadAction = c.clear ? MTLLoadActionClear : MTLLoadActionLoad;
        a.storeAction = MTLStoreActionStore;
        a.clearColor = MTLClearColorMake(c.clear_color[0], c.clear_color[1], c.clear_color[2],
                                         c.clear_color[3]);
        clears |= c.clear;
        if (c.resolve_texture) {
            a.resolveTexture = (__bridge id<MTLTexture>)c.resolve_texture;
            a.resolveLevel = c.resolve_level;
            a.resolveSlice = c.resolve_slice;
            a.storeAction = MTLStoreActionStoreAndMultisampleResolve;
            resolves = true;
        }
    }
    const DepthAttachment& d = desc.depth;
    if (d.texture) {
        attachments = true;
        id<MTLTexture> dt = (__bridge id<MTLTexture>)d.texture;
        if (!d.stencil_only) {
            MTLRenderPassDepthAttachmentDescriptor* a = first.depthAttachment;
            a.texture = dt;
            a.level = d.level;
            a.slice = d.slice;
            a.loadAction = d.clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
            a.storeAction = MTLStoreActionStore;
            a.clearDepth = d.depth;
            clears |= d.clear_depth;
        }
        if (d.has_stencil || d.stencil_only) {
            MTLRenderPassStencilAttachmentDescriptor* s = first.stencilAttachment;
            s.texture = dt;
            s.level = d.level;
            s.slice = d.slice;
            s.loadAction = d.clear_stencil ? MTLLoadActionClear : MTLLoadActionLoad;
            s.storeAction = MTLStoreActionStore;
            s.clearStencil = d.stencil;
            clears |= d.clear_stencil;
        }
    }
    if (!attachments) {
        first.renderTargetWidth = desc.width;
        first.renderTargetHeight = desc.height;
        first.defaultRasterSampleCount = 1;
    }
    // Target size: the first color attachment, else depth, else the declared render area.
    u32 width = desc.width, height = desc.height;
    const auto size_of = [&](void* texture, u32 level) {
        id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
        width = std::max<u32>(1, u32(t.width) >> level);
        height = std::max<u32>(1, u32(t.height) >> level);
    };
    const auto color = std::find_if(desc.colors.begin(), desc.colors.end(),
                                    [](const ColorAttachment& c) { return c.texture != nullptr; });
    if (color != desc.colors.end()) {
        size_of(color->texture, color->level);
    } else if (desc.depth.texture) {
        size_of(desc.depth.texture, desc.depth.level);
    }
    impl->target_width = width;
    impl->target_height = height;
    MTLRenderPassDescriptor* load = [first copy];
    for (u32 i = 0; i < RenderPassDesc::MaxColors; ++i) {
        load.colorAttachments[i].loadAction = MTLLoadActionLoad;
    }
    load.depthAttachment.loadAction = MTLLoadActionLoad;
    load.stencilAttachment.loadAction = MTLLoadActionLoad;
    impl->pass_first = first;
    impl->pass_load = load;
    impl->pass_clears = clears;
    impl->pass_resolves = resolves;
    impl->pass_opened = false;
    impl->pass_vertex_rw = desc.vertex_rw_buffer;
    impl->in_pass = true;
}

void Encoder::EndRenderPass() {
    if (!impl->in_pass) {
        // bbport: the mirror of the mismatch above (a pass counted by the scheduler that the
        // encoder never began): nothing to end.
        WarnPassMismatch("EndRenderPass outside a render pass: nothing to end", "no pass");
        return;
    }
    if (!impl->pass_opened && (impl->pass_clears || impl->pass_resolves)) {
        impl->Render(); // a pass that only clears or resolves still has to do it
    }
    if (impl->render) {
        impl->last_end = CausePassEnd;
    }
    impl->EndRender();
    impl->in_pass = false;
    impl->pass_first = nil;
    impl->pass_load = nil;
    impl->pass_vertex_rw = nullptr;
}

void Encoder::SetRenderPipeline(void* pipeline, Access buffers) {
    impl->render_access = buffers;
    if (pipeline != impl->render_pipeline) {
        impl->render_pipeline = pipeline;
        impl->MarkRender(Impl::BitPipeline);
    }
}

void Encoder::SetDepthStencil(void* state) {
    if (state != impl->depth_stencil) {
        impl->depth_stencil = state;
        impl->MarkRender(Impl::BitDepthStencil);
    }
}

void Encoder::SetViewport(const Viewport& viewport) {
    impl->viewport = viewport;
    impl->MarkRender(Impl::BitViewport);
}

void Encoder::SetScissor(const Scissor& scissor) {
    impl->scissor = scissor;
    impl->MarkRender(Impl::BitScissor);
}

void Encoder::SetCullMode(CullMode mode) {
    impl->cull = mode;
    impl->MarkRender(Impl::BitCull);
}

void Encoder::SetFrontFace(Winding winding) {
    impl->winding = winding;
    impl->MarkRender(Impl::BitWinding);
}

void Encoder::SetDepthBias(float constant, float slope, float clamp) {
    impl->bias[0] = constant;
    impl->bias[1] = slope;
    impl->bias[2] = clamp;
    impl->MarkRender(Impl::BitBias);
}

void Encoder::SetStencilReference(u32 front, u32 back) {
    impl->stencil_front = front;
    impl->stencil_back = back;
    impl->MarkRender(Impl::BitStencilRef);
}

void Encoder::SetBlendColor(float r, float g, float b, float a) {
    impl->blend[0] = r;
    impl->blend[1] = g;
    impl->blend[2] = b;
    impl->blend[3] = a;
    impl->MarkRender(Impl::BitBlend);
}

void Encoder::SetFillMode(FillMode mode) {
    impl->fill = mode;
    impl->MarkRender(Impl::BitFill);
}

void Encoder::SetDepthClip(DepthClip mode) {
    impl->depth_clip = mode;
    impl->MarkRender(Impl::BitDepthClip);
}

void Encoder::SetTessellationFactorBuffer(BufferRef factors, u32 instance_stride) {
    impl->tess_factors = factors.buffer;
    impl->tess_offset = factors.offset;
    impl->tess_stride = instance_stride;
    impl->MarkRender(Impl::BitTessFactors);
}

void Encoder::Draw(Primitive primitive, u32 vertex_count, u32 instance_count, u32 first_vertex,
                   u32 first_instance) {
    ASSERT_MSG(impl->render_pipeline, "Draw without a render pipeline");
    impl->ApplyRender();
    [impl->render drawPrimitives:ToMtl(primitive)
                     vertexStart:first_vertex
                     vertexCount:vertex_count
                   instanceCount:instance_count
                    baseInstance:first_instance];
    impl->AfterCommand(impl->render_access);
}

void Encoder::DrawIndexed(Primitive primitive, u32 index_count, IndexType type, BufferRef indices,
                          u32 instance_count, s32 base_vertex, u32 first_instance) {
    ASSERT_MSG(impl->render_pipeline, "Draw without a render pipeline");
    indices = LowOffset(indices); // 32-bit index offsets (mtl_buffer.h)
    impl->ApplyRender();
    [impl->render drawIndexedPrimitives:ToMtl(primitive)
                             indexCount:index_count
                              indexType:type == IndexType::U32 ? MTLIndexTypeUInt32
                                                               : MTLIndexTypeUInt16
                            indexBuffer:(__bridge id<MTLBuffer>)indices.buffer
                      indexBufferOffset:indices.offset
                          instanceCount:instance_count
                             baseVertex:base_vertex
                           baseInstance:first_instance];
    impl->AfterCommand(impl->render_access);
}

void Encoder::DrawIndirect(Primitive primitive, BufferRef args) {
    ASSERT_MSG(impl->render_pipeline, "Draw without a render pipeline");
    args = LowOffset(args); // 32-bit indirect offsets (mtl_buffer.h)
    impl->ApplyRender();
    [impl->render drawPrimitives:ToMtl(primitive)
                  indirectBuffer:(__bridge id<MTLBuffer>)args.buffer
            indirectBufferOffset:args.offset];
    impl->AfterCommand(impl->render_access);
}

void Encoder::DrawIndexedIndirect(Primitive primitive, IndexType type, BufferRef indices,
                                  BufferRef args) {
    ASSERT_MSG(impl->render_pipeline, "Draw without a render pipeline");
    indices = LowOffset(indices); // 32-bit index and indirect offsets (mtl_buffer.h)
    args = LowOffset(args);
    impl->ApplyRender();
    [impl->render drawIndexedPrimitives:ToMtl(primitive)
                              indexType:type == IndexType::U32 ? MTLIndexTypeUInt32
                                                               : MTLIndexTypeUInt16
                            indexBuffer:(__bridge id<MTLBuffer>)indices.buffer
                      indexBufferOffset:indices.offset
                         indirectBuffer:(__bridge id<MTLBuffer>)args.buffer
                   indirectBufferOffset:args.offset];
    impl->AfterCommand(impl->render_access);
}

void Encoder::DrawIndirectCount(Primitive primitive, BufferRef args, u32 stride, BufferRef count,
                                u32 max_count, bool indexed, IndexType type, BufferRef indices) {
    ASSERT_MSG(impl->render_pipeline, "Draw without a render pipeline");
    ASSERT_MSG(stride % 4 == 0 && stride >= (indexed ? 20u : 16u),
               "indirect argument stride {} cannot hold the records", stride);
    if (max_count == 0) {
        return;
    }
    // The clamp copies the records to a scratch buffer (the source is guest data and stays
    // untouched) on its own compute encoder, so the caller's compute state is untouched. The
    // render encoder ends here; the pass reopens (Load) at the draws below, after the clamp.
    impl->EndAll(CauseClamp);
    id<MTLComputePipelineState> pipeline = impl->ClampCountPipeline();
    const u32 params[2] = {stride / 4, max_count};
    id<MTLBuffer> scratch;
    @autoreleasepool {
        scratch = [impl->dev newBufferWithLength:u64(max_count) * stride
                                         options:MTLResourceStorageModePrivate];
        id<MTLComputeCommandEncoder> e = impl->NewCompute();
        // Reads the records and the count (guest memory); writes only its own tracked scratch.
        impl->BeginOrdered(e, impl->Need(Access::Read), Lane::Compute); // part of a draw
        [e setComputePipelineState:pipeline];
        [e setBuffer:(__bridge id<MTLBuffer>)args.buffer offset:args.offset atIndex:0];
        [e setBuffer:(__bridge id<MTLBuffer>)count.buffer offset:count.offset atIndex:1];
        [e setBytes:params length:sizeof(params) atIndex:2];
        [e setBuffer:scratch offset:0 atIndex:3];
        [e dispatchThreads:MTLSizeMake(max_count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        impl->EndOrdered(e);
        [e endEncoding];
    }
    KeepAlive(Object::Retain((__bridge void*)scratch));
    for (u32 i = 0; i < max_count; ++i) {
        const BufferRef record{(__bridge void*)scratch, u64(i) * stride};
        if (indexed) {
            DrawIndexedIndirect(primitive, type, indices, record);
        } else {
            DrawIndirect(primitive, record);
        }
    }
}

void Encoder::DrawPatches(u32 control_points, u32 patch_start, u32 patch_count,
                          BufferRef patch_index_buffer, u32 instance_count, u32 base_instance) {
    ASSERT_MSG(impl->render_pipeline, "Draw without a render pipeline");
    patch_index_buffer = LowOffset(patch_index_buffer); // 32-bit offsets (mtl_buffer.h)
    impl->ApplyRender();
    [impl->render drawPatches:control_points
                   patchStart:patch_start
                   patchCount:patch_count
             patchIndexBuffer:(__bridge id<MTLBuffer>)patch_index_buffer.buffer
       patchIndexBufferOffset:patch_index_buffer.offset
                instanceCount:instance_count
                 baseInstance:base_instance];
    impl->AfterCommand(impl->render_access);
}

void Encoder::PushDebugGroup(std::string_view label) {
    @autoreleasepool {
        NSString* name = [[NSString alloc] initWithBytes:label.data()
                                                  length:label.size()
                                                encoding:NSUTF8StringEncoding];
        id<MTLCommandEncoder> open = impl->render ? (id<MTLCommandEncoder>)impl->render
                                     : impl->compute ? (id<MTLCommandEncoder>)impl->compute
                                                     : (id<MTLCommandEncoder>)impl->blit;
        if (open) {
            [open pushDebugGroup:name];
            impl->groups.push_back({false, (__bridge void*)open});
        } else {
            [impl->Cmd() pushDebugGroup:name];
            impl->groups.push_back({true, nullptr});
        }
    }
}

void Encoder::PopDebugGroup() {
    ASSERT_MSG(!impl->groups.empty(), "PopDebugGroup without a PushDebugGroup");
    const Impl::DebugGroup group = impl->groups.back();
    impl->groups.pop_back();
    if (group.on_cmd) {
        [impl->Cmd() popDebugGroup];
    } else if (group.encoder) { // else its encoder ended and closed it
        [(__bridge id<MTLCommandEncoder>)group.encoder popDebugGroup];
    }
}

bool Encoder::InRenderPass() const noexcept {
    return impl->in_pass;
}

} // namespace Metal
