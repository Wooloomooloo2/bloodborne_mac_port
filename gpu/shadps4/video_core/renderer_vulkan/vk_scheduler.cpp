// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <mutex>
#include <string>
#ifndef _WIN32
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_act.h>
#ifdef __has_feature
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define BB_STRIP_RETURN(p) ptrauth_strip(p, ptrauth_key_return_address)
#endif
#endif
#endif
#ifndef BB_STRIP_RETURN
#define BB_STRIP_RETURN(p) (p)
#endif
#include <functional>
#include <fmt/format.h>

#include "bbport_copy.h"
#include "bbport_cpu.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_threads.h"

namespace Vulkan {

namespace BbPassLog {
namespace {
std::mutex mutex;
std::FILE* file = [] {
    const char* path = std::getenv("BB_PASS_LOG");
    return path && *path ? std::fopen(path, "w") : nullptr;
}();
} // namespace
bool Enabled() {
    return file != nullptr;
}
void Pass(u64 tick, const std::string& label) {
    std::scoped_lock lock{mutex};
    std::fprintf(file, "%llu %s\n", static_cast<unsigned long long>(tick), label.c_str());
}
} // namespace BbPassLog

Metal::RenderPassDesc RenderState::ToDesc() const {
    Metal::RenderPassDesc desc{};
    for (u32 i = 0; i < num_colors; ++i) {
        const RenderAttachment& a = colors[i];
        auto& c = desc.colors[i];
        c.texture = a.texture;
        c.level = a.level;
        c.slice = a.slice;
        c.depth_plane = a.depth_plane;
        c.clear = a.clear;
        const auto& f = a.clear_value.color.float32;
        c.clear_color = {f[0], f[1], f[2], f[3]};
    }
    if (has_depth || has_stencil) {
        auto& d = desc.depth;
        d.texture = depth.texture;
        d.level = depth.level;
        d.slice = depth.slice;
        d.has_stencil = has_stencil;
        d.stencil_only = stencil_only;
        d.clear_depth = has_depth && depth.clear;
        d.clear_stencil = has_stencil && depth.clear_stencil;
        d.depth = depth.clear_value.depthStencil.depth;
        d.stencil = depth.clear_value.depthStencil.stencil;
    }
    desc.width = width;
    desc.height = height;
    desc.vertex_rw_buffer = vertex_rw_buffer;
    return desc;
}

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(Metal::Device& device_, bool threaded) : device{device_} {
    // The first command buffer starts without state: apply all of it on the first draws rather
    // than rely on the defaults matching the encoder's.
    dynamic_state.Invalidate();
    // bbport: BB_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("BB_VK_RECORD_THREAD");
    if (threaded && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        recorder_running = true;
    }
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (recorder_running) {
        SyncRecording();
        recorder_running = false;
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
    // Commit what is still recorded through the timeline (not left to ~Encoder, outside
    // submit_mutex and unknown to it) and wait for it. The submit callback's owner may be gone.
    on_submit = nullptr;
    Finish();
}

namespace {
// bbport: why render passes end (BB_PASS_STATS=1). On a tile-based GPU (Apple) each pass loads
// and stores its attachments, so the count per frame matters (docs/MACOS_BACKLOG.md).
const bool pass_stats = [] {
    const char* env = std::getenv("BB_PASS_STATS");
    return env && env[0] == '1';
}();
std::mutex pass_stats_mutex;
std::unordered_map<std::string, u64> pass_ends;
std::unordered_map<std::string, u64> pass_resumes; // the next pass reopened the same attachments
std::string last_end_reason;
u64 pass_begins;
auto pass_stats_since = std::chrono::steady_clock::now();

void CountPass(const std::string* end_reason, bool begin) {
    std::scoped_lock lk{pass_stats_mutex};
    if (end_reason) {
        ++pass_ends[*end_reason];
        last_end_reason = *end_reason;
    }
    pass_begins += begin;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - pass_stats_since).count();
    if (seconds < 2.0) {
        return;
    }
    std::vector<std::pair<u64, std::string>> top;
    u64 total = 0;
    for (const auto& [reason, count] : pass_ends) {
        top.emplace_back(count, reason);
        total += count;
    }
    std::ranges::sort(top, std::greater{});
    std::printf("Render passes: %.0f begins/s, %.0f ends/s by reason:\n", pass_begins / seconds,
                total / seconds);
    for (size_t i = 0; i < std::min<size_t>(top.size(), 14); ++i) {
        std::printf("  %7.0f/s  %s\n", top[i].first / seconds, top[i].second.c_str());
    }
    std::vector<std::pair<u64, std::string>> resumed;
    u64 resumed_total = 0;
    for (const auto& [reason, count] : pass_resumes) {
        resumed.emplace_back(count, reason);
        resumed_total += count;
    }
    std::ranges::sort(resumed, std::greater{});
    std::printf("  split passes (same attachments reopened next): %.0f/s, after:\n",
                resumed_total / seconds);
    for (size_t i = 0; i < std::min<size_t>(resumed.size(), 10); ++i) {
        std::printf("  %7.0f/s  %s\n", resumed[i].first / seconds, resumed[i].second.c_str());
    }
    pass_resumes.clear();
    pass_ends.clear();
    pass_begins = 0;
    pass_stats_since = now;
}

std::string Where(const std::source_location& where) {
    std::string_view file = where.file_name();
    if (const auto slash = file.rfind('/'); slash != std::string_view::npos) {
        file.remove_prefix(slash + 1);
    }
    return std::string(file) + ":" + std::to_string(where.line()) + " " + where.function_name();
}

bool SameAttachments(const RenderState& a, const RenderState& b) {
    if (a.num_colors != b.num_colors || a.depth.texture != b.depth.texture ||
        a.width != b.width || a.height != b.height) {
        return false;
    }
    for (u32 i = 0; i < a.num_colors; ++i) {
        if (a.colors[i].texture != b.colors[i].texture) {
            return false;
        }
    }
    return true;
}

// What differs between two render states, for BB_PASS_STATS.
std::string StateChange(const RenderState& a, const RenderState& b) {
    if (!SameAttachments(a, b)) {
        return "BeginRendering: other attachments/size";
    }
    return "BeginRendering: same attachments, clear/flags differ";
}
} // namespace

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    if (pass_stats) {
        if (!is_rendering && SameAttachments(render_state, new_state)) {
            std::scoped_lock lk{pass_stats_mutex};
            ++pass_resumes[last_end_reason];
        }
        const std::string reason = StateChange(render_state, new_state);
        CountPass(is_rendering ? &reason : nullptr, true);
        if (is_rendering) {
            is_rendering = false; // counted above, not again in EndRendering
            Record([runs = &draw_runs](Metal::Encoder& e) { runs->EndRenderPass(e); });
            PassEnded();
        }
    }
    EndRendering();
    if (FrameCapture::Tracing()) {
        std::string line = fmt::format("P {}x{} d={}", new_state.width, new_state.height,
                                       fmt::ptr(new_state.depth.texture));
        for (u32 i = 0; i < new_state.num_colors; ++i) {
            line += fmt::format(" c={}", fmt::ptr(new_state.colors[i].texture));
        }
        FrameCapture::Trace(std::move(line));
    }
    is_rendering = true;
    render_state = new_state;
    Record([desc = render_state.ToDesc()](Metal::Encoder& e) { e.BeginRenderPass(desc); });
}

void Scheduler::EndRendering(std::source_location where, const char* detail) {
    if (!is_rendering) {
        return;
    }
    if (pass_stats) {
        const std::string reason = Where(where) + detail;
        CountPass(&reason, false);
    }
    if (FrameCapture::Tracing()) {
        FrameCapture::Trace(fmt::format("E {}:{}", where.file_name(), where.line()));
    }
    is_rendering = false;
    // Through the draw runs: waiting patch draws are drawn in this pass first.
    Record([runs = &draw_runs](Metal::Encoder& e) { runs->EndRenderPass(e); });
    PassEnded();
}

void Scheduler::PassEnded() {
    if (pass_end_hook && !in_pass_end_hook) {
        in_pass_end_hook = true;
        pass_end_hook(pass_end_context);
        in_pass_end_hook = false;
    }
}

void Scheduler::ResumeRendering() {
    if (is_rendering) {
        return;
    }
    RenderState state = render_state;
    for (u32 i = 0; i < state.num_colors; ++i) {
        state.colors[i].clear = false;
    }
    state.depth.clear = false;
    state.depth.clear_stencil = false;
    BeginRendering(state);
}

void Scheduler::PushDebugGroup(std::string label) {
    Record([this, label = std::move(label)](Metal::Encoder& e) {
        e.PushDebugGroup(label);
        ++debug_groups;
    });
}

void Scheduler::PopDebugGroup() {
    Record([this](Metal::Encoder& e) {
        if (debug_groups) {
            --debug_groups;
            e.PopDebugGroup();
        }
    });
}

void Scheduler::InsertDebugLabel(std::string label) {
    Record([label = std::move(label)](Metal::Encoder& e) {
        e.PushDebugGroup(label);
        e.PopDebugGroup();
    });
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
#ifndef _WIN32
        Dl_info info{};
        dladdr(top[i].second, &info);
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
#else
        std::printf("Recorder sync caller: %llu x %p\n",
                    static_cast<unsigned long long>(top[i].first),
                    top[i].second);
#endif
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        signal();
        return;
    }
    BbCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    Record([signal = std::move(signal), done = deferred_signals_done](Metal::Encoder&) mutable {
        BbCopy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
            signal();
            done->fetch_add(1, std::memory_order_release);
        });
    });
    KickRecording(true);
}

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        BbToggle::Disabled(BbToggle::OrderedGuestWrites)) {
        return;
    }
    BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        // Helps the copy threads the signals wait for.
        BbCopy::WaitAsync();
        std::this_thread::yield();
    }
}

void Scheduler::WaitHostCopies() {
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
        BbStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
    }
    BbStats::WaitTimer timer{BbStats::copy_threads_wait_ns};
    BbCopy::WaitAsync();
}

namespace {
#ifdef __APPLE__
/// The stack of another thread, read while it is suspended: its pc and (arm64) lr, then the
/// return addresses along its frame-pointer chain within its stack. No allocation while the
/// thread is suspended (it may hold the allocator's lock). Returns the number of frames.
int OtherThreadFrames(pthread_t thread, void** frames, int capacity) {
    const mach_port_t port = pthread_mach_thread_np(thread);
    if (thread_suspend(port) != KERN_SUCCESS) {
        return 0;
    }
    int count = 0;
    uintptr_t fp = 0;
#if defined(__aarch64__)
    arm_thread_state64_t state{};
    mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                         &state_count) == KERN_SUCCESS) {
        frames[count++] = (void*)arm_thread_state64_get_pc(state);
        frames[count++] = BB_STRIP_RETURN((void*)arm_thread_state64_get_lr(state));
        fp = (uintptr_t)arm_thread_state64_get_fp(state);
    }
#elif defined(__x86_64__)
    x86_thread_state64_t state{};
    mach_msg_type_number_t state_count = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(port, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                         &state_count) == KERN_SUCCESS) {
        frames[count++] = reinterpret_cast<void*>(state.__rip);
        fp = static_cast<uintptr_t>(state.__rbp);
    }
#endif
    // The frame chain: [fp] = caller's fp, [fp + 8] = return address; fp grows upwards
    // within the thread's stack.
    const auto stack_top = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(thread));
    const uintptr_t stack_bottom = stack_top - pthread_get_stacksize_np(thread);
    while (count < capacity && fp >= stack_bottom && fp + 2 * sizeof(void*) <= stack_top &&
           (fp & (sizeof(void*) - 1)) == 0) {
        const auto* frame = reinterpret_cast<void* const*>(fp);
        void* const ret = BB_STRIP_RETURN(frame[1]);
        if (!ret) {
            break;
        }
        frames[count++] = ret;
        const auto next = reinterpret_cast<uintptr_t>(frame[0]);
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    thread_resume(port);
    return count;
}
#endif

std::string SymbolName(void* address) {
    Dl_info info{};
    if (dladdr(address, &info) && info.dli_sname) {
        return fmt::format("{}+{:#x}", info.dli_sname,
                           reinterpret_cast<uintptr_t>(address) -
                               reinterpret_cast<uintptr_t>(info.dli_saddr));
    }
    if (info.dli_fbase) {
        return fmt::format("{:#x} in {}", reinterpret_cast<uintptr_t>(address) -
                                              reinterpret_cast<uintptr_t>(info.dli_fbase),
                           info.dli_fname ? info.dli_fname : "lib");
    }
    return fmt::format("{}", address);
}

std::string ThreadName(void* handle) {
    char name[64] = "?";
    pthread_getname_np((pthread_t)(uintptr_t)handle, name, sizeof(name));
    return name[0] ? name : "?";
}
} // namespace

void Scheduler::ReportChunkRace(const ChunkUser* other, const ChunkUser* self,
                                void* previous_caller) {
    static std::atomic<u32> reports{0};
    const u32 n = reports.fetch_add(1, std::memory_order_relaxed);
    if (n >= 8) {
        return;
    }
    if (other == self) {
        // This thread's own record: its caller was already overwritten by the guard, the
        // previous one is the use it is inside.
        std::fprintf(stderr,
                     "Scheduler: the recording chunk is used again by its own thread (#%u): %s "
                     "from %s, inside a use from %s\n",
                     n + 1, ThreadName(self->thread).c_str(), SymbolName(self->caller).c_str(),
                     SymbolName(previous_caller).c_str());
    } else {
        // The other side's record is its thread's: read once, it may leave its guard meanwhile.
        void* const other_caller = other->caller;
        void* const other_thread = other->thread;
        std::fprintf(stderr,
                     "Scheduler: two threads use the recording chunk at once (#%u): %s from %s, "
                     "while %s entered from %s\n",
                     n + 1, ThreadName(self->thread).c_str(), SymbolName(self->caller).c_str(),
                     ThreadName(other_thread).c_str(), SymbolName(other_caller).c_str());
#ifdef __APPLE__
        // Symbolised after OtherThreadFrames resumed the thread (dladdr may take a lock).
        void* other_frames[32];
        const int other_count =
            OtherThreadFrames((pthread_t)(uintptr_t)other_thread, other_frames, 32);
        std::fprintf(stderr, "  %s now (%d frames):\n", ThreadName(other_thread).c_str(),
                     other_count);
        for (int i = 0; i < other_count; ++i) {
            std::fprintf(stderr, "    %-2d %p %s\n", i, other_frames[i],
                         SymbolName(other_frames[i]).c_str());
        }
#endif
    }
    std::fprintf(stderr, "  %s (reporting):\n", ThreadName(self->thread).c_str());
    void* frames[32];
    const int count = backtrace(frames, 32);
    backtrace_symbols_fd(frames, count, 2);
    std::fflush(stderr);
}

void Scheduler::KickRecording(bool force) {
    if (!recorder_thread.joinable()) {
        return;
    }
    const ChunkGuard guard{*this, __builtin_return_address(0)};
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back(std::move(chunk));
        }
        if (!record_chunk->Empty()) {
            recorder_queue.push_back(std::move(record_chunk));
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
}

void Scheduler::SyncRecording() {
    if (!recorder_thread.joinable()) {
        return;
    }
    KickRecording(true);
    BbStats::WaitTimer timer{BbStats::sync_recording_ns};
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return recorder_queue.empty() && !recorder_busy; });
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:MtlRecorder");
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        static const auto spin_time = std::chrono::microseconds(
            BbThreads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            bbport::CpuPause();
            // The clock is read every 256 pauses, not per iteration.
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
        // Direct encoding starts only after SyncRecording(), which waits for this thread.
        chunk->Execute(encoder);
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush() {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution();
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 tick = SubmitExecution();
    BbStats::WaitTimer timer{BbStats::tick_wait_ns};
    timeline.Wait(tick);
}

void Scheduler::Wait(u64 tick) {
    if (tick >= CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        Flush();
    }
    BbStats::WaitTimer timer{BbStats::tick_wait_ns};
    timeline.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    // Checking a tick reads atomics that Metal completion handlers raise (no ioctl), so there is
    // no polling limit.
    std::unique_lock lk(pending_ops_mutex);
    while (!pending_ops.empty() && timeline.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::RecordHoistedCopy(Metal::BufferRef src, Metal::BufferRef dst,
                                  std::span<const VideoCore::BufferCopy> copies,
                                  bool ordered) {
    std::scoped_lock lk{upload_mutex};
    upload_open = true;
    for (const VideoCore::BufferCopy& c : copies) {
        // Metal does not document the order of copies within one blit encoder: a copy that may
        // overlap an earlier one (of this call or one before) starts a new blit encoder, and
        // encoders run in order. No encoder open: nothing to end.
        if (ordered) {
            upload_encoder.EndEncoding();
        }
        upload_encoder.CopyBuffer({src.buffer, src.offset + c.src_offset},
                                  {dst.buffer, dst.offset + c.dst_offset}, c.size);
    }
}

u64 Scheduler::SubmitExecution() {
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = CurrentTick();

    if (on_submit) {
        on_submit();
    }

    EndRendering();
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Encoders of the next submission before its first mark.
        profiler->Mark(0x5B317ull, [] { return std::string{"(after a submission, unmarked)"}; });
    }
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies();

    // bbport: hoisted uploads are committed first (no tick: the queue runs them before this
    // submission, whose tick covers them); held until the main commit so no copy recorded
    // meanwhile lands in a later upload command buffer than the commands that need it.
    std::unique_lock upload_lock{upload_mutex};
    if (upload_open) {
        upload_encoder.SubmitUntracked();
        upload_open = false;
    }

    ImGui::Core::TextureManager::Submit(); // ImGui textures: M5
    const u64 submit_start = bbport::CpuCycles();
    // Through the draw runs: no patch draw is left waiting, and their scratch memory is
    // handed to this submission's tick.
    draw_run_open = false;
    const u64 tick = timeline.Committed(draw_runs.Submit(encoder));
    debug_groups = 0; // Encoder::Submit closed them
    draw_runs.ReportSkips(); // dropped draws by reason, every 5 s when there are any
    BbStats::queue_submit_ns.fetch_add(bbport::CyclesToNs(bbport::CpuCycles() - submit_start),
                                       std::memory_order_relaxed);
    BbStats::queue_submits.fetch_add(1, std::memory_order_relaxed);
    if (auto* order = device.Order(); order && BbStats::enabled) {
        const auto waits = order->TakeWaitStats();
        BbStats::order_waits.fetch_add(waits.waits, std::memory_order_relaxed);
        BbStats::order_encoders.fetch_add(waits.encoders, std::memory_order_relaxed);
        u64 max = BbStats::order_max_waits.load(std::memory_order_relaxed);
        while (waits.max > max && !BbStats::order_max_waits.compare_exchange_weak(
                                      max, waits.max, std::memory_order_relaxed)) {
        }
        static u64 last_memory = 0, last_motion = 0;
        const u64 memory = order->BusyReuses(Metal::HazardClass::Memory);
        const u64 motion = order->BusyReuses(Metal::HazardClass::MotionHistory);
        BbStats::busy_slot_reuses.fetch_add(memory - last_memory, std::memory_order_relaxed);
        BbStats::busy_motion_slot_reuses.fetch_add(motion - last_motion, std::memory_order_relaxed);
        last_memory = memory;
        last_motion = motion;
    }
    ASSERT_MSG(tick == signal_value, "Submission signalled tick {}, expected {}", tick,
               signal_value);
    upload_lock.unlock();

    // A new command buffer starts without state (Encoder::Submit clears it): apply it again.
    dynamic_state.Invalidate();

    // The priority-ops thread waits for its tick to be committed before waiting on the GPU;
    // taking its mutex first means it is either before its check or already waiting (no lost
    // wakeup).
    {
        std::scoped_lock ops_lock{priority_pending_ops_mutex};
    }
    priority_pending_ops_cv.notify_all();

    // Apply pending operations
    PopPendingOperations();
    return tick;
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        // Ticks are deferred before they are committed (Timeline::Wait needs a committed one);
        // only this scheduler's own submission commits it, and it notifies.
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [&] { return op.gpu_tick < timeline.CurrentTick(); });
        }
        if (stoken.stop_requested()) {
            break;
        }
        timeline.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

} // namespace Vulkan
