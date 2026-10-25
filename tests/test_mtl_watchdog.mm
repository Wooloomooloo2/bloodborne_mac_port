// SPDX-License-Identifier: GPL-2.0-or-later
// Real Metal watchdog regressions, isolated with exec (never fork a live Metal device).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include "video_core/renderer_metal/mtl_device_impl.h"

using namespace std::chrono_literals;
extern char** environ;

// Hold the CPU inside Commit before forwarding to Metal. The watchdog must not count
// the tick as submitted while neither Metal nor the GPU has received it.
@interface SlowCommitBuffer : NSObject
@property(nonatomic, strong) id<MTLCommandBuffer> target;
@end
@implementation SlowCommitBuffer
- (id)forwardingTargetForSelector:(SEL)selector {
    (void)selector;
    return self.target;
}
- (void)commit {
    std::puts("CPU submission held before Metal receives the command buffer");
    std::fflush(stdout);
    std::this_thread::sleep_for(700ms);
    [self.target commit];
}
@end

namespace {
bool Until(auto predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

int Child(const char* mode) {
    setenv("BB_GPU_WATCHDOG_MS", !std::strcmp(mode, "disabled") ? "0" : "200", 1);
    @autoreleasepool {
        Metal::Device device;
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)device.NativeQueue();
        if (!std::strcmp(mode, "idle")) {
            std::this_thread::sleep_for(700ms);
            return 0;
        }
        if (!std::strcmp(mode, "slow-commit")) {
            SlowCommitBuffer* proxy = [SlowCommitBuffer new];
            proxy.target = [queue commandBuffer];
            const u64 tick = device.Commit((__bridge void*)proxy);
            device.Wait(tick);
            return device.IsFree(tick) ? 0 : 2;
        }
        const bool delayed = !std::strcmp(mode, "delayed-handler");
        const bool rearm = !std::strcmp(mode, "rearm");
        if (delayed || rearm) {
            auto release = std::make_shared<std::atomic<bool>>(false);
            auto entered = std::make_shared<std::atomic<bool>>(false);
            id<MTLCommandBuffer> completed = [queue commandBuffer];
            [completed addCompletedHandler:^(id<MTLCommandBuffer>) {
                entered->store(true, std::memory_order_release);
                while (!release->load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
            }];
            const u64 tick = device.Commit((__bridge void*)completed);
            if (!Until([&] { return entered->load(std::memory_order_acquire) &&
                                    device.GetImpl().event.signaledValue >= tick; })) {
                release->store(true, std::memory_order_release);
                return 2;
            }
            if (device.KnownGpuTick() >= tick || device.IsFree(tick)) {
                release->store(true, std::memory_order_release);
                return 3;
            }
            std::puts("GPU event signaled; completion handler and resource retirement still pending");
            std::fflush(stdout);
            if (delayed) {
                std::this_thread::sleep_for(700ms); // > 3 watchdog deadlines
                const bool retired_early = device.KnownGpuTick() >= tick || device.IsFree(tick);
                release->store(true, std::memory_order_release);
                device.Wait(tick);
                return !retired_early && device.IsFree(tick) ? 0 : 4;
            }
            // A later genuinely blocked GPU submission must still time out, even though an
            // earlier signal bypassed the delayed CPU handler for watchdog purposes.
            id<MTLSharedEvent> gate = [(__bridge id<MTLDevice>)device.NativeDevice() newSharedEvent];
            id<MTLCommandBuffer> blocked = [queue commandBuffer];
            [blocked encodeWaitForEvent:gate value:1];
            device.Commit((__bridge void*)blocked);
            std::this_thread::sleep_for(1500ms);
            gate.signaledValue = 1; // bounded rescue if the watchdog is broken
            release->store(true, std::memory_order_release);
            device.Finish();
            return 5;
        }
        const bool disabled = !std::strcmp(mode, "disabled");
        if (!std::strcmp(mode, "blocked-gpu") || disabled) {
            id<MTLSharedEvent> gate = [(__bridge id<MTLDevice>)device.NativeDevice() newSharedEvent];
            id<MTLCommandBuffer> blocked = [queue commandBuffer];
            [blocked encodeWaitForEvent:gate value:1];
            device.Commit((__bridge void*)blocked);
            std::this_thread::sleep_for(1500ms);
            gate.signaledValue = 1;
            device.Finish();
            return disabled ? 0 : 5;
        }
    }
    return 6;
}

bool Run(const std::string& executable, const char* mode, int expected) {
    char* args[] = {const_cast<char*>(executable.c_str()), const_cast<char*>(mode), nullptr};
    pid_t child;
    if (posix_spawn(&child, executable.c_str(), nullptr, nullptr, args, environ)) return false;
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    int status = 0;
    pid_t waited;
    while ((waited = waitpid(child, &status, WNOHANG)) == 0 ||
           (waited < 0 && errno == EINTR)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            std::printf("FAIL: %s exceeded the test deadline\n", mode);
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    if (waited != child) return false;
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == expected;
    std::printf("%s: %s (exit %d, expected %d)\n", ok ? "PASS" : "FAIL", mode,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1, expected);
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2) return Child(argv[1]);
    const std::string executable = std::filesystem::absolute(argv[0]).string();
    bool ok = true;
    ok &= Run(executable, "delayed-handler", 0);
    ok &= Run(executable, "slow-commit", 0);
    ok &= Run(executable, "idle", 0);
    ok &= Run(executable, "disabled", 0);
    ok &= Run(executable, "blocked-gpu", 75);
    ok &= Run(executable, "rearm", 75);
    return ok ? 0 : 1;
}
