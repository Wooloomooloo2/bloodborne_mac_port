// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// How the GPU driver accounts no-copy resident buffers: plain Metal, no game.
//
// The renderer's guest arena is vm_allocate'd memory wrapped with newBufferWithBytesNoCopy
// (mtl_buffer.mm, BB_ARENA_LAZY) and added to a residency set attached to the queue
// (mtl_device.mm). Only ~3.3 of its 6 GiB are mapped for the GPU by the game, and one A/B pair
// suggested the driver wires it by its full length anyway. This measures that, and whether
// separate buffers count only their own lengths (a layout with one buffer per GPU run,
// performance/github/MEMORY_FIX_RECOMMENDATION.md, S0(a-e)):
//
//   arena-accounting-test none                   nothing (the baseline)
//   arena-accounting-test one MIB [TOUCH_MIB]    one buffer (legacy: 6144, span: 4059)
//   arena-accounting-test runs [TOUCH_MIB]       3325+64, 13+64 and 0.125+64 MiB, one vm_allocate each
//   arena-accounting-test declare N              CPU cost of N encoders declaring 0, 1 and 3 buffers
//
// TOUCH_MIB (default 2560): bytes the GPU writes (blit fills) from the start of the first buffer
// (the runs touch the same total). Each phase prints one MEASURE line: system wired memory
// (vm_stat's "Pages wired down"), the GPU driver's "In use system memory" and "Alloc system
// memory" (IOAccelerator PerformanceStatistics), MTLDevice.currentAllocatedSize, this process's
// phys_footprint, and the phase's time. Run it alternating with `none` (A/B/A/B): the wired
// count is system-wide.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr unsigned long long MiB = 1ull << 20;

struct Driver {
    long long in_use = -1, alloc = -1;
};

Driver ReadDriver() {
    Driver d;
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) !=
        KERN_SUCCESS) {
        return d;
    }
    for (io_object_t service; (service = IOIteratorNext(it));) {
        CFMutableDictionaryRef props = nullptr;
        if (IORegistryEntryCreateCFProperties(service, &props, kCFAllocatorDefault, 0) ==
                KERN_SUCCESS &&
            props) {
            NSDictionary* stats = ((__bridge NSDictionary*)props)[@"PerformanceStatistics"];
            if ([stats isKindOfClass:[NSDictionary class]]) {
                if (NSNumber* v = stats[@"In use system memory"]) {
                    d.in_use = v.longLongValue;
                }
                if (NSNumber* v = stats[@"Alloc system memory"]) {
                    d.alloc = v.longLongValue;
                }
            }
            CFRelease(props);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
    return d;
}

unsigned long long WiredBytes() {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count);
    return static_cast<unsigned long long>(vm.wire_count) * vm_page_size;
}

unsigned long long Footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return info.phys_footprint;
}

std::string mode_name;
id<MTLDevice> device;

void Measure(const char* phase, double ms) {
    // Three reads a quarter second apart: the median, against other processes' noise.
    std::vector<unsigned long long> wired;
    Driver d;
    for (int i = 0; i < 3; ++i) {
        if (i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        wired.push_back(WiredBytes());
        d = ReadDriver();
    }
    std::sort(wired.begin(), wired.end());
    std::printf("MEASURE mode=%s phase=%s wired_mib=%.0f inuse_mib=%.0f alloc_mib=%.0f "
                "allocated_mib=%.0f footprint_mib=%.0f ms=%.1f\n",
                mode_name.c_str(), phase, wired[1] / double(MiB), d.in_use / double(MiB),
                d.alloc / double(MiB), device.currentAllocatedSize / double(MiB),
                Footprint() / double(MiB), ms);
}

double Ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

struct Arena {
    vm_address_t address = 0;
    unsigned long long length = 0;
    id<MTLBuffer> buffer;
};

Arena MakeArena(unsigned long long length) {
    Arena a;
    a.length = (length + vm_page_size - 1) & ~(unsigned long long)(vm_page_size - 1);
    if (vm_allocate(mach_task_self(), &a.address, a.length, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        std::printf("vm_allocate of %llu MiB failed\n", a.length / MiB);
        std::exit(2);
    }
    // As mtl_buffer.mm: shared, hazard tracking as the renderer's default (tracked), the memory
    // freed by its owner.
    a.buffer = [device newBufferWithBytesNoCopy:reinterpret_cast<void*>(a.address)
                                         length:a.length
                                        options:MTLResourceStorageModeShared
                                    deallocator:nil];
    if (!a.buffer) {
        std::printf("newBufferWithBytesNoCopy of %llu MiB failed\n", a.length / MiB);
        std::exit(2);
    }
    return a;
}

int Layout(const std::vector<unsigned long long>& lengths, unsigned long long touch) {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLResidencySetDescriptor* rd = [MTLResidencySetDescriptor new];
    rd.label = @"accounting test";
    NSError* error = nil;
    id<MTLResidencySet> residency = [device newResidencySetWithDescriptor:rd error:&error];
    [queue addResidencySet:residency];
    Measure("before", 0);

    auto start = std::chrono::steady_clock::now();
    std::vector<Arena> arenas;
    for (const unsigned long long length : lengths) {
        arenas.push_back(MakeArena(length));
    }
    const double create_ms = Ms(start);
    start = std::chrono::steady_clock::now();
    for (const Arena& a : arenas) {
        [residency addAllocation:a.buffer];
    }
    [residency commit];
    Measure("created", create_ms + Ms(start));

    // One small command buffer on the queue: the residency set is made resident with it.
    id<MTLBuffer> small = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    start = std::chrono::steady_clock::now();
    {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit fillBuffer:small range:NSMakeRange(0, 4096) value:1];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
    Measure("first-use", Ms(start));

    // The GPU writes TOUCH bytes (uploads into the arena), first buffer first.
    start = std::chrono::steady_clock::now();
    {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        unsigned long long left = touch;
        for (const Arena& a : arenas) {
            const unsigned long long n = std::min(left, a.length);
            for (unsigned long long off = 0; off < n; off += 256 * MiB) {
                [blit fillBuffer:a.buffer range:NSMakeRange(off, std::min(256 * MiB, n - off)) value:7];
            }
            left -= n;
        }
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
    Measure("touched", Ms(start));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    Measure("held-3s", 0);

    start = std::chrono::steady_clock::now();
    for (Arena& a : arenas) {
        [residency removeAllocation:a.buffer];
    }
    [residency commit];
    for (Arena& a : arenas) {
        a.buffer = nil;
        vm_deallocate(mach_task_self(), a.address, a.length);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    Measure("released", Ms(start));
    return 0;
}

// S0(e): the CPU cost of declaring the arena (useResource) on every encoder, as the renderer
// does (Encoder::DeclareIndirect): N compute encoders with 0, 1 and 3 declarations.
int Declare(int encoders) {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    std::vector<Arena> arenas;
    for (unsigned long long length : {3389 * MiB, 77 * MiB, 64 * MiB}) {
        arenas.push_back(MakeArena(length));
    }
    for (int declared : {0, 1, 3, 0, 1, 3}) {
        const auto start = std::chrono::steady_clock::now();
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        for (int i = 0; i < encoders; ++i) {
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            if (declared == 1) {
                [e useResource:arenas[0].buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            } else if (declared == 3) {
                id<MTLResource> all[3] = {arenas[0].buffer, arenas[1].buffer, arenas[2].buffer};
                [e useResources:all count:3 usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            }
            [e endEncoding];
        }
        const double encode_ms = Ms(start);
        [cb commit];
        [cb waitUntilCompleted];
        std::printf("DECLARE encoders=%d declared=%d encode_ms=%.3f total_ms=%.3f\n", encoders,
                    declared, encode_ms, Ms(start));
    }
    for (Arena& a : arenas) {
        a.buffer = nil;
        vm_deallocate(mach_task_self(), a.address, a.length);
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        device = MTLCreateSystemDefaultDevice();
        if (!device || argc < 2) {
            std::printf("usage: %s none | one MIB [TOUCH_MIB] | runs [TOUCH_MIB] | declare N\n",
                        argv[0]);
            return 2;
        }
        mode_name = argv[1];
        std::printf("Device %s, working set %.0f MiB, largest buffer %.0f MiB\n",
                    device.name.UTF8String, device.recommendedMaxWorkingSetSize / double(MiB),
                    device.maxBufferLength / double(MiB));
        if (mode_name == "none") {
            Measure("before", 0);
            std::this_thread::sleep_for(std::chrono::seconds(4));
            Measure("held-3s", 0);
            return 0;
        }
        if (mode_name == "declare") {
            return Declare(argc > 2 ? std::atoi(argv[2]) : 600);
        }
        if (mode_name == "one" && argc > 2) {
            const unsigned long long mib = std::strtoull(argv[2], nullptr, 10);
            const unsigned long long touch = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 2560;
            mode_name += "-" + std::to_string(mib);
            return Layout({mib * MiB}, std::min(touch, mib) * MiB);
        }
        if (mode_name == "runs") {
            const unsigned long long touch = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2560;
            return Layout({3389 * MiB, 77 * MiB, 64 * MiB + 128 * 1024}, touch * MiB);
        }
        std::printf("unknown mode %s\n", argv[1]);
        return 2;
    }
}
