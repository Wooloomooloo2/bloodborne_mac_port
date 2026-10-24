// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

#include <array>
#include <atomic>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
} // namespace Shader

namespace Metal {
class PipelineFactory;
} // namespace Metal

namespace Vulkan {

/// A game pipeline: its shader stages and the Metal pipeline states the derived class builds on
/// the pipeline creation pool (BuildAsync).
class Pipeline {
public:
    Pipeline(Metal::PipelineFactory& factory, const Shader::Profile& profile,
             bool is_compute = false);
    virtual ~Pipeline();

    /// bbport: false while the creation pool is still building the pipeline.
    bool IsReady() const noexcept {
        return building.load(std::memory_order_acquire) == 0;
    }

    /// bbport: waits until the creation pool has built the pipeline.
    void WaitReady() const;

    /// bbport: waits up to `max_ns` for the creation pool; true once the pipeline is ready.
    bool WaitReady(u64 max_ns) const;

    /// The build produced no Metal pipeline state (logged once, with the reason, when it
    /// finished): its draws and dispatches are skipped. False while the pipeline is building.
    bool Failed() const noexcept {
        return failed.load(std::memory_order_acquire);
    }
    /// Why the build failed (the reason Failed() logged); empty while building or when built.
    const std::string& FailureReason() const noexcept {
        static const std::string none;
        return Failed() ? failure : none;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    /// The stages' names and hashes, for messages.
    [[nodiscard]] std::string GetDebugString() const;

protected:

    /// bbport: runs `build` (the Metal shader compiles and pipeline-state creation) on the
    /// pipeline creation pool, or right here when the pool is off.
    void BuildAsync(std::function<void()> build);

    /// From the build: no pipeline state; logs `why` with the pipeline's stages.
    void Fail(std::string_view kind, const std::string& debug_str, std::string_view why);

    Metal::PipelineFactory& factory;
    const Shader::Profile& profile;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool is_compute;
    mutable std::atomic<u32> building{0};
    std::atomic<bool> failed{false};
    std::string failure; ///< written before `failed` is set
};

/// bbport: background pipeline creation. Shader translation stays on the GPU command thread;
/// building the Metal pipeline states (SPIR-V to MSL, the Metal compiler) runs on a few
/// low-priority threads, so new pipelines compile in parallel and off the render path.
/// BB_PIPELINE_THREADS=n (default 4; 0 creates pipelines where they are needed, as before).
namespace PipelineBuilder {
bool Enabled();
void Submit(std::function<void()> job);
/// Waits until every submitted pipeline is built.
void Drain();
} // namespace PipelineBuilder

} // namespace Vulkan
