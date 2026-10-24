// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_metal/mtl_pipeline_build.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(Metal::PipelineFactory& factory, const Shader::Profile& profile,
                                 ComputePipelineKey compute_key_, const Shader::Info& info,
                                 const Metal::ShaderModule* module_, SerializationSupport& sdata,
                                 bool preloading /*=false*/)
    : Pipeline{factory, profile, true}, compute_key{compute_key_}, module{module_} {
    stages[int(Shader::SwStage::Compute)] = &info;
    // bbport: built on the pipeline creation pool when it is on (vk_pipeline_common.h).
    BuildAsync([this, debug_str = GetDebugString()] { Build(debug_str); });
}

void ComputePipeline::Build(const std::string& debug_str) {
    std::string error;
    compute = module ? factory.BuildCompute(compute_key, *module, error) : nullptr;
    if (!compute) {
        Fail("Compute", debug_str, module ? error : "no shader module");
    }
}

ComputePipeline::~ComputePipeline() {
    WaitReady(); // a build still on the creation pool uses this object
}

} // namespace Vulkan
