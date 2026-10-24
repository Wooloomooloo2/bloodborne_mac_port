// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: SPIR-V specialisation constants fixed to plain constants (Metal-native backend).

#include "video_core/renderer_metal/mtl_specialize.h"

#include <unordered_map>

namespace Metal {

/// Fixes the SPIR-V specialization constants at the values given (or their defaults) and turns
/// them into plain constants, so SPIRV-Cross emits no Metal function constants (a pipeline state
/// would then need a specialized function): OpSpecConstant's literal, OpSpecConstantTrue/False by
/// opcode, the SpecId decorations dropped. OpSpecConstantOp stays (none in the host shaders).
std::vector<u32> Specialize(std::span<const u32> words, std::span<const SpecConstant> constants) {
    if (words.size() < 5) {
        return {words.begin(), words.end()};
    }
    std::unordered_map<u32, u32> spec_ids; // result id -> SpecId
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xffff;
        if (!count) {
            break;
        }
        if (op == 71 && count >= 4 && words[i + 2] == 1) { // OpDecorate id SpecId n
            spec_ids[words[i + 1]] = words[i + 3];
        }
        i += count;
    }
    std::vector<u32> out(words.begin(), words.begin() + 5);
    out.reserve(words.size());
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xffff;
        if (!count || i + count > words.size()) {
            break;
        }
        if (op == 71 && count >= 4 && words[i + 2] == 1) {
            i += count; // SpecId on what is now a plain constant
            continue;
        }
        const size_t at = out.size();
        out.insert(out.end(), words.begin() + i, words.begin() + i + count);
        if ((op == 48 || op == 49 || op == 50 || op == 51) && count >= 3) {
            u32 new_op = op == 48 ? 41u : op == 49 ? 42u : op == 50 ? 43u : 44u;
            auto it = spec_ids.find(words[i + 2]);
            if (it != spec_ids.end()) {
                for (const SpecConstant& constant : constants) {
                    if (constant.id != it->second) {
                        continue;
                    }
                    if (op == 50) {
                        if (count >= 4) {
                            out[at + 3] = constant.value;
                        }
                    } else if (op == 48 || op == 49) {
                        new_op = constant.value != 0 ? 41u : 42u;
                    }
                }
            }
            out[at] = (count << 16) | new_op;
        }
        i += count;
    }
    return out;
}

} // namespace Metal
