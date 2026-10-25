// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// bbport: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <array>
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "bbport_threads.h"

extern "C" std::uint64_t runtime_disabled_optimizations;

namespace BbToggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    UpdateImageFastPath = 1u << 30,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    /// Scene textures use BB_ANISO (16) times anisotropic filtering instead of the game's
    /// ratio; bit set: the game's own samplers (A/B while the game runs).
    ForcedAniso = 1ull << 61,
    /// Passes with no attachments and no pixel shader use a 16x16 render area instead of the
    /// device maximum (16384x16384, ~0.3 ms of tiling each on Apple GPUs); bit set: maximum.
    NoOutputPassArea = 1ull << 58,
    /// Uploads into buffers inside a render pass run ahead of the submission when nothing in
    /// it used their destination yet (Runtime::HoistCopy), instead of ending the pass; bit set:
    /// every upload ends the pass.
    UploadHoisting = 1ull << 59,
    /// The game's copy shader (ExecuteCopyShaderHLE) is hoisted the same way when neither its
    /// source nor its destination was used in the submission; bit set: it ends the pass.
    CopyShaderHoisting = 1ull << 60,
    /// The vertex streams of a draw come from the draw-preparation worker's V#s (alone of the
    /// prepared resources; PreparedResources turns them all off); bit set: from the registers.
    PreparedVertexInputs = 1ull << 62,
    /// Copy-shader copies issued inside a render pass wait for its end instead of ending it
    /// (vk_shader_hle.cpp, Defer); bit set: they end the pass. BB_DEFER_COPIES=0 too.
    DeferredShaderCopies = 1ull << 63,
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace BbToggle

namespace BbStats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Stall diagnostics (BB_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0}, flips{0};
/// bbport: uploads recorded ahead of their submission (Runtime::HoistCopy).
inline std::atomic<std::uint64_t> hoisted_copies{0};
/// bbport: copy-shader copies recorded ahead of their submission, and those that could not be.
inline std::atomic<std::uint64_t> hoisted_shader_copies{0}, inline_shader_copies{0};
/// bbport: copy-shader copies deferred to the end of their render pass, and the passes a
/// command split anyway because it touched their ranges.
inline std::atomic<std::uint64_t> deferred_shader_copies{0}, deferred_copy_conflicts{0};
/// bbport: arena ordering requests recorded (Runtime::FlushBarriers: commands waiting for
/// render passes, and the others), and uploads not hoisted because their ranges were in use by
/// a submission in flight.
inline std::atomic<std::uint64_t> arena_barriers{0}, arena_other_barriers{0}, hoists_in_flight{0};
/// bbport: texture usage upgrades (Runtime::UpgradeUsage: lazy ShaderWrite/PixelFormatView) and
/// the bytes of the textures they copied.
inline std::atomic<std::uint64_t> texture_upgrades{0}, texture_upgrade_bytes{0};
/// bbport: ordering fence waits encoded at encoder opens, those encoders, and the most on one
/// encoder (Metal::OrderTracker::TakeWaitStats).
inline std::atomic<std::uint64_t> order_waits{0}, order_encoders{0}, order_max_waits{0};
/// bbport: encoders that found every ordering slot busy (Metal::OrderTracker::BusyReuses), for
/// guest memory and object motion's history.
inline std::atomic<std::uint64_t> busy_slot_reuses{0}, busy_motion_slot_reuses{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0};
/// Diagnostics are collected only with BB_FRAME_STATS=1.
inline const bool enabled = [] {
    const char* env = std::getenv("BB_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0};
/// The texture cache collector: the usage it compares, the mark it starts at, images it freed.
inline std::atomic<std::uint64_t> gc_used_bytes{0}, gc_trigger_bytes{0}, gc_freed_images{0};
/// Metal: MTLDevice.currentAllocatedSize and the resident guest buffers it includes (the GC
/// compares the difference).
inline std::atomic<std::uint64_t> gc_allocated_bytes{0}, gc_resident_bytes{0};
/// bbport: the resident bytes this build's layout created (the arena, and the direct memory
/// access tables with BB_DIRECT_MEMORY_ACCESS=1) and those the previous layout created (the
/// arena and both tables, always): the texture budget's reference (GarbageCollectImages).
inline std::atomic<std::uint64_t> layout_resident_bytes{0}, legacy_resident_bytes{0};
/// Copies for Metal's 32-bit index and indirect offsets (ranges ending past 4 GiB into the
/// arena): from guest memory (CPU-owned data) and in command order (GPU-written data); indirect
/// draws with a GPU count there (their records are read at any offset, not copied); page table
/// fills at first touches (direct memory access only).
inline std::atomic<std::uint64_t> index_copies_cpu{0}, index_copies_gpu{0}, indirect_copies_cpu{0},
    indirect_copies_gpu{0}, indirect_counted_high{0}, dma_first_touches{0};
/// Staging pool bytes per memory type (DeviceLocal, HostUncached, HostCached), now and peak.
inline std::array<std::atomic<std::uint64_t>, 3> staging_bytes{}, staging_peak{};
/// Requests that stopped the game before and are absorbed now: size-0 writes outside the arena
/// (a write sink), read-backs of memory the arena does not cover (declined).
inline std::atomic<std::uint64_t> size0_sinks{0}, uncovered_readbacks{0};
/// Seconds of a steady clock, updated at every guest submission (cheap ages for caches).
inline std::atomic<std::uint32_t> coarse_second{0};
/// Images the collector passed over (GPU-written, not evictable without memory pressure).
inline std::atomic<std::uint64_t> gc_kept_images{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
/// Frame timeline (BB_FRAME_STATS, ns totals): the draw recording thread busy and waiting for
/// packets, vkQueueSubmit (MoltenVK encodes there) at the flip and elsewhere, the frames-ahead
/// wait, flip submission to GPU completion, and stage A waiting for stage B (drains).
inline std::atomic<std::uint64_t> rec_busy_ns{0}, rec_idle_ns{0}, flip_submit_ns{0},
    queue_submit_ns{0}, queue_submits{0}, frames_ahead_ns{0}, gpu_latency_ns{0}, gpu_done_frames{0},
    gpu_done_gap_ns{0}, drain_ns{0}, compile_wait_ns{0};
/// Pipelines built by the background creation pool and its time (ns, all pool threads).
inline std::atomic<std::uint64_t> background_builds{0}, background_build_ns{0};
/// Draws skipped because their pipeline was still compiling (shader_compile=skip).
inline std::atomic<std::uint64_t> compile_skipped_draws{0};
/// Draws with real tessellation, and rect/quad lists (emulated with tessellation).
inline std::atomic<std::uint64_t> tess_draws{0}, rect_draws{0};
} // namespace BbStats
