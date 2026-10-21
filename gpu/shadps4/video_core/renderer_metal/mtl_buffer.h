// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: MTLBuffer creation for the buffer cache (Metal-native renderer).

#pragma once

#include <string_view>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_memory_type.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {

class Device;

/// Creates an MTLBuffer of `size` bytes: DeviceLocal is private storage (untracked unless
/// Device::HazardTracking(): Encoders order it, mtl_order.h), HostUncached is shared
/// and write-combined, HostCached and Stream are shared. Returns an empty Object on failure.
/// `gpu_address` receives the buffer's GPU address, `contents` the CPU pointer (null for
/// DeviceLocal).
Object CreateBuffer(Device& device, u64 size, VideoCore::MemoryType type, std::string_view label,
                    u64* gpu_address, u8** contents);

/// Metal's indexed and indirect draws and indirect dispatches use only the low 32 bits of their
/// buffer offset: an index or argument offset of 4 GiB + x reads at x (measured 2026-10-21 on an
/// M2 Max and an M4 Max; shader buffer bindings are not affected). Returns `ref` with its offset
/// below 2 GiB, over the same memory: from 2 GiB on, a window, a second MTLBuffer over the
/// buffer's pages (newBufferWithBytesNoCopy) starting at a 2 GiB boundary, at most 4 GiB long.
/// No copy, and writes through the buffer are seen at once. The windows live as long as their
/// buffer. Unchanged for a buffer without CPU contents (private storage: BB_ARENA_LAZY=0).
BufferRef LowOffset(BufferRef ref);

/// Whether LowOffset can rebase offsets in `buffer` (an id<MTLBuffer>): it has CPU contents.
bool CanLowerOffsets(void* buffer);

} // namespace Metal
