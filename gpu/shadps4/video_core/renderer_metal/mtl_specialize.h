// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: SPIR-V specialisation constants fixed to plain constants (Metal-native backend).

#pragma once

#include <span>
#include <vector>
#include "common/types.h"

namespace Metal {

struct SpecConstant {
    u32 id;
    u32 value;
};

/// Returns `spirv` with every OpSpecConstant/OpSpecConstantTrue/False whose SpecId is in
/// `constants` turned into a plain OpConstant/OpConstantTrue/False with that value (others keep
/// their defaults as plain constants). SPIRV-Cross then emits literals (no function constants).
std::vector<u32> Specialize(std::span<const u32> spirv, std::span<const SpecConstant> constants);

} // namespace Metal
