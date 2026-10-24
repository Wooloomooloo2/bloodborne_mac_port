// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_key.h"

namespace Serialization {
struct Archive;
}

namespace Metal {
class ShaderModule;
struct ComputePipeline;
} // namespace Metal

namespace Vulkan {

class ComputePipeline : public Pipeline {
public:
    struct SerializationSupport {
        u32 dummy{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    /// `module` outlives the pipeline (the pipeline cache's program owns it).
    ComputePipeline(Metal::PipelineFactory& factory, const Shader::Profile& profile,
                    ComputePipelineKey compute_key, const Shader::Info& info,
                    const Metal::ShaderModule* module, SerializationSupport& sdata,
                    bool preloading);
    ~ComputePipeline();

    /// The Metal pipeline state, once ready (IsReady/WaitReady); null when Failed().
    const Metal::ComputePipeline* Compute() const noexcept {
        return compute;
    }

private:
    void Build(const std::string& debug_str);

    ComputePipelineKey compute_key;
    const Metal::ShaderModule* module;
    const Metal::ComputePipeline* compute{}; ///< written by the build, read once ready
};

} // namespace Vulkan
