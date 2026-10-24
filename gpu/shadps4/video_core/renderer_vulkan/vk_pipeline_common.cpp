// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include "bbport_toggles.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace Vulkan {

namespace {
// Stats in nanoseconds (the arm64 cycle counter is 24 MHz before the M4).
u64 ElapsedNs(std::chrono::steady_clock::time_point start) {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count());
}
} // namespace

Pipeline::Pipeline(Metal::PipelineFactory& factory_, const Shader::Profile& profile_,
                   bool is_compute_ /*= false*/)
    : factory{factory_}, profile{profile_}, is_compute{is_compute_} {}

Pipeline::~Pipeline() {
    // A pipeline still in the creation pool must finish before its state goes away.
    WaitReady();
}

void Pipeline::WaitReady() const {
    if (building.load(std::memory_order_acquire) != 0) {
        const auto start = std::chrono::steady_clock::now();
        while (building.load(std::memory_order_acquire) != 0) {
            building.wait(1, std::memory_order_acquire);
        }
        BbStats::compile_wait_ns.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
    }
}

void Pipeline::Fail(std::string_view kind, const std::string& debug_str, std::string_view why) {
    // Each pipeline is built once: this is its only message.
    LOG_ERROR(Render_Vulkan, "{} pipeline {} not built ({}): its {} are skipped", kind, debug_str,
              why, is_compute ? "dispatches" : "draws");
    failure = why;
    failed.store(true, std::memory_order_release);
}

bool Pipeline::WaitReady(u64 max_ns) const {
    if (IsReady()) {
        return true;
    }
    // std::atomic::wait has no timeout; builds take milliseconds, so poll finely.
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::nanoseconds(max_ns);
    bool ready = false;
    while (!(ready = IsReady()) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    BbStats::compile_wait_ns.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
    return ready;
}

void Pipeline::BuildAsync(std::function<void()> build) {
    if (!PipelineBuilder::Enabled()) {
        build();
        return;
    }
    building.store(1, std::memory_order_relaxed);
    PipelineBuilder::Submit([this, build = std::move(build)] {
        const auto start = std::chrono::steady_clock::now();
        // BB_COMPILE_DELAY_MS: test knob, slows every build as a cold Metal compiler cache does.
        static const int delay_ms = [] {
            const char* value = std::getenv("BB_COMPILE_DELAY_MS");
            return value ? std::atoi(value) : 0;
        }();
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        build();
        BbStats::background_build_ns.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
        BbStats::background_builds.fetch_add(1, std::memory_order_relaxed);
        building.store(0, std::memory_order_release);
        building.notify_all();
    });
}

namespace PipelineBuilder {
namespace {
struct Pool {
    std::mutex mutex;
    std::condition_variable work_cv, idle_cv;
    std::deque<std::function<void()>> jobs;
    u32 running = 0;
    std::vector<std::jthread> threads;

    explicit Pool(u32 count) {
        for (u32 i = 0; i < count; ++i) {
            threads.emplace_back([this, i](std::stop_token stop) { Run(stop, i); });
        }
    }
    ~Pool() {
        for (auto& thread : threads) {
            thread.request_stop();
        }
        work_cv.notify_all();
    }

    void Run(std::stop_token stop, u32 index) {
        Common::SetCurrentThreadName(("bb:PipeBuild" + std::to_string(index)).c_str());
#ifdef __APPLE__
        // Below the render threads: mostly the efficiency cores.
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
        std::unique_lock lk{mutex};
        while (true) {
            work_cv.wait(lk, [&] { return stop.stop_requested() || !jobs.empty(); });
            if (stop.stop_requested() && jobs.empty()) {
                return;
            }
            auto job = std::move(jobs.front());
            jobs.pop_front();
            ++running;
            lk.unlock();
            job();
            lk.lock();
            --running;
            if (jobs.empty() && running == 0) {
                idle_cv.notify_all();
            }
        }
    }
};

u32 ThreadCount() {
    static const u32 count = [] {
        const char* env = std::getenv("BB_PIPELINE_THREADS");
        return env ? u32(std::clamp(std::atoi(env), 0, 16)) : 4u;
    }();
    return count;
}

Pool& GetPool() {
    static Pool pool{ThreadCount()};
    return pool;
}
} // namespace

bool Enabled() {
    return ThreadCount() != 0;
}

void Submit(std::function<void()> job) {
    auto& pool = GetPool();
    {
        std::scoped_lock lk{pool.mutex};
        pool.jobs.push_back(std::move(job));
    }
    pool.work_cv.notify_one();
}

void Drain() {
    if (!Enabled()) {
        return;
    }
    auto& pool = GetPool();
    std::unique_lock lk{pool.mutex};
    pool.idle_cv.wait(lk, [&] { return pool.jobs.empty() && pool.running == 0; });
}
} // namespace PipelineBuilder

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
