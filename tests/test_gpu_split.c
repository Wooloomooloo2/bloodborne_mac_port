/* End-to-end test of the GPU process split (M3) without the game: this x86-64 program (under
 * Rosetta) plays bb-probe. It starts the native arm64 bbgpu-host (gpu_process.c), starts the
 * renderer there (bbgpu_init_remote) and calls the real GnmDriver/VideoOut HLE functions, which
 * now forward to the host:
 *   - a VideoOut port and a display buffer in guest memory (mirrored to the host),
 *   - a command buffer submit and SubmitDone: submissions unlock only when the host's GpuIdle
 *     interrupt comes back through the event ring,
 *   - a flip: the flip status updates from the host's flip event; vblanks arrive.
 * Opens the host's window for a moment. Needs out/bbgpu-host (arm64 dependencies).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu_process.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The settings menu of the GPU library restarts through probe.c, which tests do not link. */
void runtime_restart(void) { abort(); }

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); exit(1); } } while (0)

typedef int32_t (ABI *Allocate)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t *);
typedef int32_t (ABI *Map)(void **, uint64_t, int, int, int64_t, uint64_t);
typedef int32_t (ABI *VoOpen)(int32_t, int32_t, int32_t, const void *);
typedef void (ABI *VoSetAttribute)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef int32_t (ABI *VoRegister)(int32_t, int32_t, void *const *, int32_t, const void *);
typedef int32_t (ABI *VoSubmitFlip)(int32_t, int32_t, int32_t, int64_t);
typedef struct {
    uint64_t count, process_time, tsc;
    int64_t flip_arg;
    uint64_t submit_tsc, reserved0;
    int32_t gc_queue_num, flip_pending_num, current_buffer;
    uint32_t reserved1;
} FlipStatus;
typedef int32_t (ABI *VoFlipStatus)(int32_t, FlipStatus *);
typedef struct { uint64_t count, process_time, tsc, reserved; uint8_t flags, pad[7]; } VblankStatus;
typedef int32_t (ABI *VoVblankStatus)(int32_t, VblankStatus *);
typedef int32_t (ABI *GnmSubmit)(uint32_t, const uint32_t **, uint32_t *, const uint32_t **, uint32_t *);
typedef int32_t (ABI *GnmSubmitDone)(void);
typedef int (ABI *GnmAllowed)(void);

static void *hle(const char *nid) {
    void *f = (void *)bbgpu_resolve(nid);
    CHECK(f, "HLE function %s not registered", nid);
    return f;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void sleep_ms(unsigned ms) {
    struct timespec t = {0, (long)ms * 1000000L};
    nanosleep(&t, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(!runtime_memory_reserve_space(), "cannot reserve the guest range");
    gpu_process_start();
    void *shared = gpu_process_shared();
    CHECK(shared, "the GPU host did not start (out/bbgpu-host built?)");
    runtime_start(1);
    bbgpu_register_kernel();

    char user_dir[1024];
    snprintf(user_dir, sizeof user_dir, "%s/out/split-test-user", getenv("PWD") ? getenv("PWD") : ".");
    const BbGpuConfig config = {"GPU split test", "SPLITTEST", user_dir, 0x05050031, 0, 640, 360};
    double t0 = now_ms();
    CHECK(bbgpu_init_remote(&config, shared) == 0, "the renderer did not start in the host");
    gpu_process_rendering();
    printf("split test: renderer started in the host in %.0f ms\n", now_ms() - t0);

    /* Guest memory: 4 MiB of direct memory, mirrored into the host before what follows. */
    int64_t phys = -1;
    void *base = NULL;
    CHECK(((Allocate)runtime_resolve("rTXw65xmLIA#p#J", 0))(0, 536870912, 4u << 20, 2u << 20, 0, &phys) == 0,
          "direct memory allocation");
    CHECK(((Map)runtime_resolve("L-Q3LEjIbgA#p#J", 0))(&base, 4u << 20, 3, 0, phys, 2u << 20) == 0, "direct memory map");
    uint8_t *display = base;
    uint32_t *dcb = (uint32_t *)((uint8_t *)base + (2u << 20));
    for (uint32_t y = 0; y < 360; ++y)
        for (uint32_t x = 0; x < 640; ++x) ((uint32_t *)display)[y * 640 + x] = 0xff000000u | (x * 255 / 640) << 16 | (y * 255 / 360) << 8;

    /* VideoOut port and buffer: the handle and group must match the host's. */
    const int32_t handle = ((VoOpen)hle("Up36PTk687E"))(255, 0, 0, NULL);
    CHECK(handle == 1, "sceVideoOutOpen returned %d", handle);
    uint8_t attribute[40];
    ((VoSetAttribute)hle("i6-sR91Wt-4"))(attribute, 0x80000000u, 1, 0, 640, 360, 640);
    void *buffers[1] = {display};
    const int32_t group = ((VoRegister)hle("w3BY+tAEiQY"))(handle, 0, buffers, 1, attribute);
    CHECK(group == 0, "sceVideoOutRegisterBuffers returned %d", group);

    /* A command buffer (one NOP packet) and the end of the frame. */
    dcb[0] = 0xC0001000u; /* PM4 type 3, NOP, one data word */
    dcb[1] = 0;
    const uint32_t *dcbs[1] = {dcb};
    uint32_t sizes[1] = {8};
    CHECK(((GnmSubmit)hle("zwY0YV91TTI"))(1, dcbs, sizes, NULL, NULL) == 0, "sceGnmSubmitCommandBuffers");
    CHECK(((GnmSubmitDone)hle("yvZ73uQUqrk"))() == 0, "sceGnmSubmitDone");
    GnmAllowed allowed = (GnmAllowed)hle("b08AgtPlHPg");
    t0 = now_ms();
    while (!allowed() && now_ms() - t0 < 10000) sleep_ms(1);
    CHECK(allowed(), "submissions still locked 10 s after SubmitDone (no GpuIdle from the host)");
    printf("split test: GpuIdle came back %.1f ms after SubmitDone\n", now_ms() - t0);

    /* A flip: its status comes back as an event; vblanks too. */
    VoFlipStatus flip_status = (VoFlipStatus)hle("SbU3dwp80lQ");
    VoVblankStatus vblank_status = (VoVblankStatus)hle("1FZBKy8HeNU");
    VblankStatus vb0, vb1;
    CHECK(vblank_status(handle, &vb0) == 0, "sceVideoOutGetVblankStatus");
    t0 = now_ms();
    CHECK(((VoSubmitFlip)hle("U46NwOiJpys"))(handle, 0, 1, 1234) == 0, "sceVideoOutSubmitFlip");
    FlipStatus st;
    do {
        sleep_ms(1);
        CHECK(flip_status(handle, &st) == 0, "sceVideoOutGetFlipStatus");
    } while ((st.count < 1 || st.flip_pending_num) && now_ms() - t0 < 10000);
    CHECK(st.count >= 1 && st.flip_arg == 1234 && st.current_buffer == 0 && st.flip_pending_num == 0,
          "flip status count %llu arg %lld buffer %d pending %d", (unsigned long long)st.count, (long long)st.flip_arg,
          st.current_buffer, st.flip_pending_num);
    printf("split test: flip presented in %.1f ms (flip status from the host's event)\n", now_ms() - t0);
    sleep_ms(500);
    CHECK(vblank_status(handle, &vb1) == 0 && vb1.count > vb0.count + 10, "vblanks: %llu -> %llu",
          (unsigned long long)vb0.count, (unsigned long long)vb1.count);
    printf("split test: %llu vblanks in 0.5 s\n", (unsigned long long)(vb1.count - vb0.count));
    /* Text entry (IME dialog) in the host window: open with a preset, read it back. */
    CHECK(bbgpu_text_input_begin("Hunter", "Name", 0) == 1, "text entry did not open in the host window");
    char text[64] = "";
    int state = -1;
    t0 = now_ms();
    do {
        sleep_ms(5);
        state = bbgpu_text_input_poll(text, sizeof text);
    } while (strcmp(text, "Hunter") && now_ms() - t0 < 2000);
    CHECK(state == 0 && !strcmp(text, "Hunter"), "text entry: state %d, text '%s'", state, text);
    printf("split test: text entry open in the host window, preset read back\n");
    printf("PASS: GPU process split: renderer in the arm64 host, submits, GpuIdle, flips, vblanks, text entry\n");
    fflush(stdout);
    _exit(0); /* the host sees this process exit and ends too */
}
