// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: one buffer-to-buffer copy region (Metal-native renderer). Replaces vk::BufferCopy in the
// scheduler and the caches, with the same field order.

#pragma once

#include "common/types.h"

namespace VideoCore {

struct BufferCopy {
    u64 src_offset{};
    u64 dst_offset{};
    u64 size{};
};

} // namespace VideoCore
