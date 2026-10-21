// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: MTLBuffer creation for the buffer cache (Metal-native renderer).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <mach/mach.h>
#include <objc/runtime.h>

#include "video_core/renderer_metal/mtl_buffer.h"
#include "video_core/renderer_metal/mtl_device.h"

namespace Metal {

namespace {

MTLResourceOptions OptionsFor(const Device& device, VideoCore::MemoryType type) {
    switch (type) {
    case VideoCore::MemoryType::DeviceLocal:
        // Untracked: the Encoder orders them (mtl_order.h); shared buffers stay tracked.
        return MTLResourceStorageModePrivate |
               (device.HazardTracking() ? 0 : MTLResourceHazardTrackingModeUntracked);
    case VideoCore::MemoryType::HostUncached:
        return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
    case VideoCore::MemoryType::HostCached:
    case VideoCore::MemoryType::Stream:
    default:
        return MTLResourceStorageModeShared;
    }
}

/// BB_ARENA_LAZY (default 1, 0 = off): device-local buffers of 1 GiB and more (the guest arena)
/// live in vm_allocate'd memory wrapped without copying, so only the pages the GPU or an upload
/// touches cost memory. A private buffer is committed in full when created: the 16 GiB arena made
/// the GPU process ~24 GB (measured 2026-10-17: 4/8/16 GiB arena -> 11/15/24 GB).
bool LazyArena(VideoCore::MemoryType type, u64 size) {
    static const bool on = [] {
        const char* env = std::getenv("BB_ARENA_LAZY");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return on && type == VideoCore::MemoryType::DeviceLocal && size >= (u64{1} << 30);
}

id<MTLBuffer> NewLazyBuffer(id<MTLDevice> mtl_device, const Device& device, u64 size) {
    vm_address_t address = 0;
    const vm_size_t length = round_page(size);
    if (vm_allocate(mach_task_self(), &address, length, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        return nil;
    }
    const MTLResourceOptions options =
        MTLResourceStorageModeShared |
        (device.HazardTracking() ? 0 : MTLResourceHazardTrackingModeUntracked);
    id<MTLBuffer> buffer = [mtl_device newBufferWithBytesNoCopy:reinterpret_cast<void*>(address)
                                                         length:length
                                                        options:options
                                                    deallocator:^(void* pointer, NSUInteger bytes) {
                                                      vm_deallocate(mach_task_self(),
                                                                    vm_address_t(pointer), bytes);
                                                    }];
    if (buffer == nil) {
        vm_deallocate(mach_task_self(), address, length);
    }
    return buffer;
}

constexpr u64 WindowStep = u64{2} << 30;
constexpr u64 WindowSize = u64{4} << 30;
/// The key of a buffer's windows (an NSMutableDictionary: window index -> id<MTLBuffer>).
char windows_key;

} // namespace

bool CanLowerOffsets(void* buffer) {
    return buffer != nullptr && ((__bridge id<MTLBuffer>)buffer).contents != nullptr;
}

BufferRef LowOffset(BufferRef ref) {
    if (ref.offset < WindowStep || !CanLowerOffsets(ref.buffer)) {
        return ref;
    }
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)ref.buffer;
    const u64 index = ref.offset / WindowStep;
    const u64 start = index * WindowStep;
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    NSMutableDictionary* windows = objc_getAssociatedObject(buffer, &windows_key);
    if (windows == nil) {
        windows = [NSMutableDictionary dictionary];
        objc_setAssociatedObject(buffer, &windows_key, windows, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    id<MTLBuffer> window = windows[@(index)];
    if (window == nil) {
        @autoreleasepool {
            // The buffer owns the memory (its deallocator frees it) and the windows: none outlives it.
            window = [buffer.device
                newBufferWithBytesNoCopy:static_cast<u8*>(buffer.contents) + start
                                  length:std::min<u64>(WindowSize, buffer.length - start)
                                 options:buffer.resourceOptions
                             deallocator:nil];
            if (window == nil) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    std::fprintf(stderr, "Metal: no window at %llu GiB of %s; offset kept\n",
                                 static_cast<unsigned long long>(start >> 30),
                                 buffer.label.UTF8String ?: "a buffer");
                }
                return ref;
            }
            window.label = [NSString stringWithFormat:@"%@ window %llu GiB", buffer.label ?: @"",
                                                      static_cast<unsigned long long>(start >> 30)];
            windows[@(index)] = window;
            std::printf("Metal: window at %llu GiB of %s (32-bit index/indirect offsets)\n",
                        static_cast<unsigned long long>(start >> 30),
                        buffer.label.UTF8String ?: "a buffer");
        }
    }
    return {(__bridge void*)window, ref.offset - start};
}

Object CreateBuffer(Device& device, u64 size, VideoCore::MemoryType type, std::string_view label,
                    u64* gpu_address, u8** contents) {
    *gpu_address = 0;
    *contents = nullptr;
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLBuffer> buffer = LazyArena(type, size)
                               ? NewLazyBuffer(mtl_device, device, size)
                               : [mtl_device newBufferWithLength:size options:OptionsFor(device, type)];
    if (buffer == nil) {
        return {};
    }
    if (!label.empty()) {
        buffer.label = [[NSString alloc] initWithBytes:label.data()
                                                length:label.size()
                                              encoding:NSUTF8StringEncoding];
    }
    *gpu_address = buffer.gpuAddress;
    if (type != VideoCore::MemoryType::DeviceLocal) {
        *contents = static_cast<u8*>(buffer.contents);
    }
    return Object::Adopt((__bridge_retained void*)buffer);
}

} // namespace Metal
