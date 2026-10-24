// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: buffer memory kinds, shared by the cache and the Metal buffer creation.

#pragma once

#include "common/types.h"

namespace VideoCore {

/// Hints and requirements for the backing memory type of a commit
enum class MemoryType : u8 {
    DeviceLocal,  ///< Private storage (GPU only; no CPU mapping).
    HostUncached, ///< Shared, write-combined: optimized for CPU to GPU uploads.
    HostCached,   ///< Shared, cached: optimized for GPU to CPU readbacks.
    Stream,       ///< Shared, host-written every frame.
};

} // namespace VideoCore
