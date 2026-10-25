// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../bbmac_settings.h"

// Renderer-side adapter for the native game's menu. All live values come from the same
// atomics as the existing overlay; the C bridge handles shared-memory transport separately.
namespace BbMacSettings {
int32_t Get(uint32_t field);
bool Set(uint32_t field, int32_t value);
bool RestartNeeded();
} // namespace BbMacSettings
