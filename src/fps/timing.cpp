// SPDX-License-Identifier: GPL-3.0-or-later
// Timing adapted from evill33t/bloodborne_mac bbacb95c, based on droogie/bbhost
// 566184a74f071166299bb656c4af479e5bbe2771. See PROVENANCE.md.
// Runs in bb-probe (x86-64), without renderer, SDL, plugin or networking dependencies.
#include "timing.h"
#include "timing_verify.h"
#include "frame_rate_step.h"
#include "sprj_flipper.h"
#include "wind_vector_patch.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace {
constexpr std::uint64_t kPreferredGuestSlide = 0x400000;
constexpr std::size_t TailSize = 65536;
struct Memory { std::uint64_t slide; std::size_t size; };
struct ElfImage { Memory mem; std::uint64_t tail_va, tail_used, tail_size; };
std::vector<std::uint64_t> writable_pages;
static void host_log(const char* format, ...) {
    va_list args; va_start(args, format);
    std::fputs("Adaptive FPS: ", stderr); std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr); va_end(args);
}
static std::int64_t host_clock_realtime_us() {
    timespec t{}; clock_gettime(CLOCK_REALTIME, &t);
    return std::int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}
static void host_sleep_us(std::uint64_t us) {
    timespec t{static_cast<time_t>(us / 1000000), static_cast<long>((us % 1000000) * 1000)};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static void guest_keep_writable(Memory*, std::uint64_t at) { writable_pages.push_back(at); }
std::vector<MacTimingVerify::OwnedRange> g_timing_owned;

std::atomic<float> g_time_scale{1.0f};

constexpr std::uint64_t kFrameManager = 0x2434770;
constexpr std::uint8_t kFrameManagerPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                  0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};

// The game's pace: 30 (its own), a fixed rate (60, 90, 120: the game's mode 2
// with a 1/fps target), or uncapped (the same with the target the display's
// refresh rate or 1/240 s). Kyo's FPS++ lists for 1.09 (60 and 90 FPS++ 1.0,
// and his 1.3 lists with yogesh1239's high-FPS fixes) are the source of most
// of what follows; every value and code site is checked against the eboot's
// bytes before anything is written, and each is done natively - values for
// the rate, loads and in-place rewrites, no new code in the image.
//
// The game's task step is a fixed 1/30 a frame (sub_2418d20's step-info,
// 0x2418e36); the 60 FPS++ cave (0x2418e3d, sixty_fps_sites.inc) replaces it
// with the measured frame time clamped to [target, 1/30] and keeps that in a
// slot (0x575a908) that the steps the game builds for itself read instead of
// their 1/30 (the list's own loads, and kFrameTimeSites: the physics and AI
// worlds, the ragdoll/clip drive, grass). What is not a step is converted:
//
// The game's fixed steps as values: every site the "60 FPS++" list gives a
// float constant, as the value it had and the value it gets at 60. 83 are
// 1/30 s steps made 1/60, 30 are 30 a second made 60; the rest are per-frame
// weights and decays retuned to the shorter frame (0.1 -> 0.05, 0.95 ->
// 0.9747...). Some are immediates in instructions, some constants in .rodata;
// each is checked for its old value before anything is written, and one
// mismatch leaves the game at 30. The list's code rewrites are kSixtySites
// below. Above 60 the ones in kRated follow the rate - the .rodata ones written
// from each frame's time - and the rest keep their 60 value, as Kyo's 90 and
// 120 lists hold them; that rule was his ("the known-good 120 FPS++ block
// holds 1/60 at these same addresses while running correctly at ~120 fps"),
// and the physics world's step (kFrameTimeSites) was one it got wrong, so a
// site leaves it only once its system is read.
//
// Not every 1/30 in the game is a step. A duration the data gives in frames
// (an event's "wait N frames", a light's fade frames, param fields) becomes
// seconds as `vcvtsi2ss; vmulss [1/30]`, and what holds it counts down by
// the frame's real seconds (the event wait: sub_1bc8630 makes it, sub_1bb5830
// subtracts the step) - so 1/30 is right at any frame rate. An earlier
// version of the list pointed 22 of those loads at the measured frame time
// and set 15 of their constants to 1/60 (halving the durations at 60), and
// two more conversions (0x4caf1c0, 0x4d27284) to 1/15 (doubling them): one
// "wait N frames" command ran four times as long by one path as by the
// other. Kyo's later list leaves them all at 1/30, and so does this table -
// with three more his 1.3 list (from yogesh1239's timing measurements) puts
// back: 0x4c88688 (a count of 1/30 s frames in sub_11ca140's rate sums) and
// the two shortest-duration floors max(t, 1/30) at 0x12f06f1 and 0x12f6992,
// which at 1/60 let the durations they guard run at half length.
// Also kept at 1/30: the disposal-duration minima 0x4cad65c/660/664.
// Original ELF RVAs 0xf1e5e0/0xf1e630, 0xf201f0 and 0xf37a40 are
// destructors: they convert an object's integer duration in authored frames
// to seconds with frames * (1/30), then call 0x2301fa0/1fe0/2020. Those
// wrappers reach 0xee5920/0xee5a40/0xee5b60, whose vmaxss at
// 0xee596a/0xee5a8a/0xee5baa clamps that duration to these minima before
// queuing disposal. This floor is an authored duration, not a simulation
// step; halving it shortens zero-frame disposal even when elapsed time is
// correct. The native instructions establish this duration contract, not a
// connection between these objects and any particular fog draw group.
// Three per-frame amounts the lists leave at 1/30 are made per-frame here
// instead: the debug camera's step (0x1972a80), the target bank's refresh
// accumulator (0x4d2577c, read through the frame's time below anyway), and
// the follow camera's chase-rate ease (0x4d25e94: 1 - (1 - 1/30)^(30 dt)).
// The physics world's expected minimum step (hkpWorldCinfo, 0x0f383ca) is a
// tolerance hint at 1/fps; 0x4d29170's step is read by one load the 60 FPS++
// sites point at the frame's time.
// Not here: the effects' rate. The effect manager's constructor (sub_26fb190)
// stores 30.0 at its +0 (the immediate at 0x26fb39f) and the GPU particle
// update multiplies elapsed seconds by 0x4cad7fc (30.0): both turn effect
// data, written in 30 fps frames, into seconds - an emitter's interval is its
// frames / 30 counted down by elapsed time - so they are the data's rate, not
// the game's. The FPS++ lists set both to 60, which halved every frame-given
// interval and lifetime of the sfx emitters and ran GPU particles at twice
// their speed; Dark Souls III on PC, at 60, keeps the same constructor's 30.
// The FX particle system steps by the frame's time and reads neither.
struct Constant {
    std::uint32_t at;
    std::uint32_t was, now;  // the eboot's value, and the one at 60
};
constexpr Constant kConstants[] = {
    {0x0a3fc3f, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x0f383ca, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x12b5458, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x12b55b6, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x13734f3, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x13ef51c, 0x41f00000, 0x42700000},  // 30 -> 60
    {0x13ef6ec, 0x41f00000, 0x42700000},  // 30 -> 60
    {0x16efe5c, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x183a4c3, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x183a4cd, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x183a4d7, 0x3e99999a, 0x3d4ccccd},  // 0.3 -> 0.05
    {0x183a4e1, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x183a4f5, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x183a4ff, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x183a509, 0x3cf5c28f, 0x3c75c28f},  // 0.03 -> 0.015
    {0x183a527, 0x3dcccccd, 0x3e4ccccd},  // 0.1 -> 0.2
    {0x183a531, 0x3e99999a, 0x3e4ccccd},  // 0.3 -> 0.2
    {0x183a53b, 0x3e99999a, 0x3e19999a},  // 0.3 -> 0.15
    {0x183a649, 0x3f860a92, 0x3f32b8c2},  // 1.0472 -> 0.698132
    {0x183a653, 0x3d75c28f, 0x3d23d70a},  // 0.06 -> 0.04
    {0x183a65d, 0x3dcccccd, 0x3e19999a},  // 0.1 -> 0.15
    {0x183a8c1, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x183a8cb, 0x3e800000, 0x3d4ccccd},  // 0.25 -> 0.05
    {0x183a8d5, 0x3e99999a, 0x3d4ccccd},  // 0.3 -> 0.05
    {0x183a8df, 0x3e99999a, 0x3d4ccccd},  // 0.3 -> 0.05
    {0x183a8f3, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x183a8fd, 0x3e800000, 0x3d4ccccd},  // 0.25 -> 0.05
    {0x183a907, 0x3cf5c28f, 0x3c75c28f},  // 0.03 -> 0.015
    {0x183a925, 0x3ecccccd, 0x3e4ccccd},  // 0.4 -> 0.2
    {0x183a92f, 0x3ecccccd, 0x3e4ccccd},  // 0.4 -> 0.2
    {0x183a939, 0x3e99999a, 0x3e19999a},  // 0.3 -> 0.15
    {0x183aa47, 0x3f860a92, 0x3f32b8c2},  // 1.0472 -> 0.698132
    {0x183aa51, 0x3d75c28f, 0x3d23d70a},  // 0.06 -> 0.04
    {0x183aa5b, 0x3e99999a, 0x3e19999a},  // 0.3 -> 0.15
    {0x1972a80, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ab67ef, 0x41f00000, 0x42700000},  // 30 -> 60
    {0x1bf9b74, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1c0ca9a, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1daa65e, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea4003, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea40a0, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea4146, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea49ce, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea4bda, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea5248, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea5476, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea552d, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea57c0, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea5ac5, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea6015, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea6045, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea6275, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea62a5, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ea632f, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1eb540b, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1eb54dc, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1eb90aa, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1eb9117, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ebb111, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ebf0d3, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec3f69, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec3f9a, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec3fd8, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec4009, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec404e, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec40a4, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec40e0, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec4136, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec4167, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec41a5, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1ec41dc, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1f032f2, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1f03762, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1f6aa32, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x1f96d9a, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x200d8e2, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x200e142, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x200e2d4, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x200fa52, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x20119d2, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x2012652, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x20127cc, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x2012932, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x2012ac4, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x2012c32, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x203acb7, 0x41f00000, 0x42700000},  // 30 -> 60
    {0x222bcd7, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x23dd026, 0x41f00000, 0x42700000},  // 30 -> 60
    {0x23dede6, 0x41f00000, 0x42700000},  // 30 -> 60
    {0x2418e39, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x2418f38, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x243460c, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x243485f, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x2434883, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x24348b8, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x25df519, 0x3c888889, 0x3c088889},  // 0.0166667 -> 0.00833333
    {0x4befdcc, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4c9f358, 0x41f00000, 0x42700000},  // 30 -> 60
    {0x4ccc3f4, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4cf6420, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x4cf6424, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x4cf6428, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x4cf642c, 0x3dcccccd, 0x3d4ccccd},  // 0.1 -> 0.05
    {0x4d2577c, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4d25e68, 0x3dcccccd, 0x3d523176},  // 0.1 -> 0.0513167 (1 - 0.9^(1/2))
    {0x4d25e94, 0x3d088889, 0x3c89b0c3},  // 0.0333333 -> 0.0168070
    {0x4d265fc, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4d26618, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4d27554, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4d275f4, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4d27c4c, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4d28e98, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4d29170, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4d29b80, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
    {0x4d29e74, 0x3d088889, 0x3c888889},  // 0.0333333 -> 0.0166667
};

// The constants above that follow the rate, and how; the rest of the table
// keeps its 60 value at every rate above 30. `each_frame`: .rodata the game
// reads where it uses it, written again every frame from the frame's seconds
// at every converted rate (the same clamped frame time the FPS++ cave hands
// the game's task step: each_frame below), so it follows a frame that ran
// long as the task step does. The others are code immediates
// or tied to a frame count (0x4cf6420, a tenth of 0x192c405's interval a
// frame) and take the rate's value (uncapped: its reference rate, 120).
enum class Kind : std::uint8_t {
    Step,    // a frame's seconds: dt
    Amount,  // an amount a frame, the game's own at 30 a second: was * 30 dt
    Decay,   // a smoothing weight a frame: 1 - (1 - was)^(30 dt)
    Keep,    // a share kept a frame: was^(30 dt), the same share kept a second
    Rate,    // a count a second the game wrote as 30 (frames in a second): was / (30 dt)
};
struct Rated {
    std::uint32_t at;
    Kind kind;
    bool each_frame;
};
constexpr Rated kRated[] = {
    // The physics world's expected minimum step (hkpWorldCinfo, sub_f38300):
    // a tolerance hint, 1/fps (Kyo's 90 and 120: 1/90, 1/120).
    {0x0f383ca, Kind::Step, false},
    // The debug camera's step: a code immediate.
    {0x1972a80, Kind::Step, false},
    // The ragdoll keyframe controller's drive step (sub_1c0c2b0, an
    // hkaRagdollRigidBodyController's +0x60, checked > 0 at 0xae461a): the
    // physics world's step, which now follows the frame.
    {0x1c0ca9a, Kind::Step, false},
    // YEBIS's elapsed time once more: the post-process context's +4 (set to
    // 1/60 here in its initialisation, 0x25df516, and never again) is stored
    // into CPostEffect+0x6ac every frame (0x25d7e56) after the write of
    // 0x4d2979c's value (0x25d6af0, kDtLoads), so it is the one the eye
    // adaptation and afterimage decays read; half a frame, at the rate's value.
    {0x25df519, Kind::Amount, false},
    // A tenth of the change over 0x192c3ff's 10-frame interval, a frame: it
    // and the interval (kCounts) go together, 1/20 and 20 at 60, 1/30 and 30
    // at 90, 1/40 and 40 at 120.
    {0x4cf6420, Kind::Amount, false},
    {0x4cf6424, Kind::Amount, false},
    {0x4cf6428, Kind::Amount, false},
    {0x4cf642c, Kind::Amount, false},
    // The follow camera's distance ease, 0.1 a frame at 30 (0x183af8c); Kyo's
    // first 60 list held 0.025, half the 60 value (his 1.3: scaled by dt).
    {0x4d25e68, Kind::Decay, true},
    // The follow camera's chase-rate ease (0x183e2b3: value += (target - value) * w).
    {0x4d25e94, Kind::Decay, true},
    // The per-frame values that were here (the sprint's wall test, remote
    // characters' locomotion, the hitbox velocity, the targeting ring, the
    // bone impulse, the UI steps, the ghosts' fade, YEBIS's frame time,
    // momentum, the sprint ramps and the slope slide) are read through
    // kDtLoads' slots instead; 0x4d2577c and 0x4d29170, whose loads the 60
    // FPS++ sites point at the frame-time slot, need nothing.
};
constexpr bool rated_in_constants() {
    for (const Rated& r : kRated) {
        bool found = false;
        for (const Constant& c : kConstants) found = found || c.at == r.at;
        if (!found) return false;
    }
    return true;
}
static_assert(rated_in_constants(), "every rated value is one of kConstants (its old value is checked there)");

// The list's other value changes. Counts that follow the rate: a countdown
// of 10 frames (0x192c3ff's movl into +0xf8), the default of 5 an int setting
// is read with (0x1929675's mov edx, before sub_24eba00), a worker's
// usleep(33000) in a polling loop (0x25629f0), and the sound system's update
// time. That thread (MagicOrchestra over FMOD, its loop 0x2c00e30) updates
// every MainThreadUpdateTime us, read from the text config at 0x4b33b40 into
// +0x8d4 (sub_2bf67f0, strtol base 10; the engine's own default is 16666).
// Bloodborne ships 33333: at 60 the game posts two frames of sound requests
// per update, and some attack sounds played half as often as at 30
// (2026-09-30). The digits, in place: one update a frame at every rate, and
// at the uncapped mode's 240 (04166).
enum class Count : std::uint8_t {
    Frames,  // was * fps / 30
    Micros,  // was * 30 / fps
    Digits,  // Micros, as five ASCII digits
};
struct Counted {
    std::uint32_t at;
    std::uint8_t n;
    std::uint32_t was;
    Count kind;
};
constexpr Counted kCounts[] = {
    {0x192c405, 4, 10, Count::Frames},
    {0x1929676, 4, 5, Count::Frames},
    {0x25629f1, 4, 33000, Count::Micros},
    {0x4b33c51, 5, 33333, Count::Digits},
};

// And the same at every rate: the frame-time manager's catch-up allowance
// zeroed (its movabs rcx, 0x1e0000000n into +0x268/+0x26c) - inside the
// function ours replaces. Retained with the source timing table.
// (A load of the ragdoll/clip drive's step was moved here to a converted
// constant until 2026-10-10; kFrameTimeSites reads the frame's time there.)
struct Bytes {
    std::uint32_t at;
    std::uint8_t n;
    std::uint8_t was[10], now[10];
};
constexpr Bytes kBytes[] = {
    {0x2434863, 10, {0x48, 0xb9, 0x01, 0, 0, 0, 0x1e, 0, 0, 0}, {0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0}},
    {0x2434887, 10, {0x48, 0xb9, 0, 0, 0, 0, 0x1e, 0, 0, 0}, {0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0}},
};

// The rest of that list (Kyo's "60 FPS++" for 1.09), generated with its old
// bytes from the eboot by tools/fps_sites.py; the tables above are left out
// of it. What it does:
//  - the measured frame time, clamped to [target, 1/30] by a cave over a panic
//    path (0x2418e3d, the flipper's +0x264 against +0x18), stored in a free
//    slot (0x575a908) that the loads of shared 1/30 steps the list names read, and
//    multiplied in where 0x21bc181 had a fixed step - those systems step by
//    the frame that ran rather than by 1/30 (the list's author credits the
//    cloth and jumping fixes at 60 to these). The target is ours (1/60, 1/90,
//    or the uncapped limit), so this is what makes 90 and uncapped real time;
//  - two runs of parameter reads (0x183ff29, 0x1840799, beside the camera's
//    field-of-view clamp) replaced by stores of the list's 60 fps values, and
//    two edits in the camera's update (0x183dc97, 0x183dcfe) - left out with
//    the Camera tuning Original (kKyoCameraSites);
//  - a few constants and table bytes (0x47df586, 0x3abc2xx), the cloth's
//    substep count forced to 1 (0xb6e67a: Kyo's "the part files were authored
//    at 30 fps with 2 substeps: a rate of 60"), the motion blur's sample
//    check (0xfbc40f).
// Left out, each with its reason in the generator: the frame-time manager's
// own edits (ours replaces that function), a frame-mode store, the mode-name
// pointer table, the timed wait turned into a return, and the sound thread's
// sleep cap (a cave over a live function) - that thread's update time is in
// kCounts instead. Every required group is admitted before installation.
struct Site {
    std::uint32_t at;
    const char* was;
    const char* now;
};
constexpr Site kSixtySites[] = {
#include "sixty_fps_sites.inc"
};

// Steps the lists left fixed, made to read the frame's time - the clamped
// measured frame time the 60 FPS++ cave keeps in its slot (0x575a908) every
// frame, what the game's task step is at every converted rate. Applied with
// the 60 FPS++ sites (the slot is theirs) at every rate. Each found by Kyo's
// 1.3 lists and yogesh1239's Timing and AI/Camera fixes (min(dt, 1/30) in
// caves at 0x5490000) except the physics world, and done here as loads and
// in-place rewrites, no new code: Binary Ninja addresses, the eboot's bytes,
// ours.
constexpr Site kFrameTimeSites[] = {
    // NS_SPRJ::SprjTargetBankManager::Update's refresh accumulator:
    // vaddss xmm0, xmm0, [0x4d2577c -> the slot].
    {0x016a4416, "c5fa58055e136803", "c5fa5805ea640b04"},
    // The ragdoll/clip drive (sub_1915ec0, from ChrIns::vfunc_34) hands its
    // three players a step of 1/30 s a call (about 220 calls a second near
    // enemies): yogesh1239 measured those clocks at 2.0 and 4.0 s a second at
    // 60 and 120. Path B (to 0x1c11650) and path C (to 0x1c0df90):
    // vmovss xmm0, [0x4d26364 -> the slot].
    {0x01915fab, "c5fa1005b1034103", "c5fa10055549e403"},
    {0x01916030, "c5fa10052c034103", "c5fa1005d048e403"},
    // Path A (to 0x1c11040) uses the same 1/30 first to turn a frame count
    // into seconds - which must stay 1/30 - and then as the step. Before:
    // vcvtsi2ss xmm1,xmm0,ecx; vmovss xmm0,[1/30]; vmulss xmm1,xmm1,xmm0;
    // mov rdi,[rbx+0x20]; mov eax,[rax+0x1e1]; test al,4; sete al; movzx
    // ecx,al; mov rsi,r14; mov rdx,r15; add rsp,8. After: mov rdi,[rbx+0x20];
    // vcvtsi2ss xmm1,xmm0,ecx; vmulss xmm1,xmm1,[1/30]; vmovss xmm0,[slot];
    // xor ecx,ecx; test byte [rax+0x1e1],4; sete cl; mov rsi,r14;
    // mov rdx,r15; pop rax (the prologue's alignment push); nop.
    {0x01915f65, "c5fa2ac9c5fa1005f3034103c5f259c8488b7b208b80e1010000a8040f94c00fb6c84c89f64c89fa4883c408",
     "488b7b20c5fa2ac9c5f2590def034103c5fa10058b49e40331c9f680e1010000040f94c14c89f64c89fa5890"},
    // SprjHkAiManager's Havok AI world step (sub_222bc10): the step-info
    // {0x56efd40, 1/30} it passes to 0x21cdad0 (hkaiWorld's character and
    // behaviour step). The lists run it at 1/60 at every rate - enemy steering
    // 1.5 times as fast at 90. Before: lea rcx,[0x56efd30]; add rcx,0x10;
    // mov [rbp-0x1c0],rcx; mov dword [rbp-0x1b8],1/30. After: lea
    // rcx,[0x56efd40]; mov [rbp-0x1c0],rcx; mov ecx,[slot];
    // mov [rbp-0x1b8],ecx; nop2 (rcx is not an argument of the call).
    {0x0222bcbf, "488d0d6a404c034883c11048898d40feffffc78548feffff8988083d",
     "488d0d7a404c0348898d40feffff8b0d35ec5203898d48feffff6690"},
    // The Havok physics world's step (sub_1bf99d0: hkpWorld's multithreaded
    // step through sub_f5a2e0 and sub_f5b100, a step-info whose dt is 1/30):
    // every list held 1/60 at every rate (Lance McDonald's uncapped list alone
    // reads the frame time, capped at 1/60), so rigid bodies, ragdolls and
    // breakables ran 1.5 times as fast at 90 and twice at 120. Before:
    // mov qword [rbp-0x40],0 (dead: the next store fills it); mov dword
    // [rbp-0x48],1/30. After: mov eax,[slot] (rax is loaded next);
    // mov [rbp-0x48],eax; nop6.
    {0x01bf9b69, "48c745c000000000c745b88988083d", "8b05990db6038945b8660f1f440000"},
    // Grass sway (sub_2713870): with the GX device's +0x100 at 1 it steps a
    // fixed 1/60 a frame (the 60 FPS++ sites already point its other path at
    // the slot): vmovss xmm4, [0x4d29e70 -> the slot].
    {0x02713d8d, "c5fa1025db606102", "c5fa1025736b0403"},
};

// The original 30 fps wind advances by 1/60 per frame, not 1/30. The
// converted mode selects the alternate 1/30 loads, so redirect all four
// loads to a separate half-step instead of the task's full frame time.
// Check complete instructions before the FPS++ displacement edits run.
constexpr Site kWindStepLoads[] = {
    {0x02713d8d, "c5fa1025db606102", ""},
    {0x02713dad, "c5fa1005bb606102", ""},
    {0x02713db7, "c5fa1005b5606102", ""},
    {0x02715d71, "c5fa1025fb406102", ""},
};

// The play time (GameDataMan+0x94, milliseconds: the save list's clock and
// the play log's): NS_SPRJ::MoveMapStep::InGameUpdate adds the frame's step
// to it in whole milliseconds, truncated - (int)(dt * 1000) - so a 16.7 ms
// frame adds 16 and the clock runs 4% slow at 60 and 120 (1% at the game's
// own 30 and at 90, 14% at 144 Hz uncapped). At a converted rate it adds a
// count of ours instead: the same step (the 60 FPS++ cave's
// min(max(measured, target), 1/30)) in milliseconds with the remainder
// carried, written by our SprjFlipper::Update every frame into the image's
// tail (core/elf.h). Before: vmulss xmm0,xmm1,[1000.0]; vcvttss2si
// rcx,xmm0. After: mov ecx,[count]; nop7 - xmm1 (the step) is what the code
// after reads, and xmm0 is written before it is read again.
// (NS_SPRJ::GameDataMan::AccumulatePlayTime, 0x18eb6a0, is the same code out
// of line; nothing calls it.)
constexpr std::uint32_t kPlayTimeAdd = 0x1d419d6;
constexpr char kPlayTimeAddWas[] = "c5f259051267fe02c4e1fa2cc8";
static_assert(sizeof(kPlayTimeAddWas) - 1 == 2 * 13, "the add and its replacement are 13 bytes");

// The task step itself: the 60 FPS++ cave (0x2418e3d, as kSixtySites writes
// it) computes it as movss xmm0,[rbx+0x264]; maxss xmm0,[rbx+0x18]; minss
// xmm0,[1/30] - the flipper's measured frame time clamped to [target, 1/30]
// - and stores it into the step-info and the slot. With our
// SprjFlipper::Update the cave loads ours instead (movss xmm0,[step]; nop8;
// nop5), written every frame in the image's tail just before the cave runs:
// the same clamp, times a plugin's time scale (frame_rate_set_time_scale),
// at most 1/30. Above 30 everything steps by that step, so slow motion
// scales the step rather than stretching the frames.
constexpr std::uint32_t kStepLoad = 0x2418e3d;
constexpr char kStepLoadWas[] = "f30f108364020000f30f5f4318f30f5d0512d59002";
static_assert(sizeof(kStepLoadWas) - 1 == 2 * 21, "the cave's three instructions are 21 bytes");

// Original camera tuning: do not install Kyo's constructor/parameter retune.
// The table-driven camera amounts/decays are still updated from frame time.
constexpr std::uint32_t kKyoCameraSites[] = {0x183dc97, 0x183dcfe, 0x183ff29, 0x1840799};
constexpr std::uint32_t kKyoCameraFirst = 0x183a4c3, kKyoCameraLast = 0x183aa5b;  // the constructor's immediates
bool kyo_camera_site(std::uint32_t at) {
    return std::find(std::begin(kKyoCameraSites), std::end(kKyoCameraSites), at) != std::end(kKyoCameraSites);
}
bool kyo_camera_constant(std::uint32_t at) { return at >= kKyoCameraFirst && at <= kKyoCameraLast; }

// Redirected loads: a constant the game reads where it means "a frame" - a
// weight or a share kept a frame, an amount a frame, a step, a count a
// second - is read from a slot of ours instead, in the image's tail
// (core/elf.h), which each frame's seconds rewrite. The instruction's rip
// displacement is pointed at the slot; the constant itself stays the game's.
// So a constant other code reads with another meaning (a duration, a unit)
// keeps it, no page of .rodata is written while the game runs, and any rate -
// one that moves every frame too - converts exactly. The table,
// engine/frame_rate_dt_loads.inc, is generated by tools/dt_loads.py from
// tools/dt_loads.txt and the eboot: each instruction's bytes and its
// displacement's target are checked first, and one mismatch leaves the whole
// table out. Kinds as kRated's; an Amount may be negative (a timer counted
// down a fixed amount a call).
struct DtLoad {
    std::uint32_t insn;      // the instruction (Binary Ninja address)
    std::uint8_t len, disp;  // its length and where its 4-byte rip displacement is
    std::uint32_t constant;  // the value it reads (.rodata or .data)
    Kind kind;               // how that value follows the frame's time
    std::uint8_t lanes;      // 1 (a dword), 2 (a qword), 4 (an xmmword)
    const char* bytes;       // the instruction as the eboot has it
};
//
// What it holds (each with its reader in the .inc):
//  - kept shares a frame: the movement controller's momentum (0.95 a frame on
//    the body's X/Z, chr_motion_physics_update; 0.95^(30 dt) keeps a jump's
//    reach at any rate), the sprint wall test's ramps (x0.8 blocked, x1.2
//    free), CSSlopeCtrl's slope fall damping (0.65, a .data tunable);
//  - amounts a frame: the slope fall speed (1.0, .data), the replay ghosts'
//    fade (+-0.1), YEBIS's frame time (half a 30 fps frame: the eye
//    adaptation's), a state-table fade (+-0.05 at 0x1d3251e/0x1d3259b), and
//    SprjWorldAiManager::TickLua's timer (-1/30 a call before a full GC of
//    the AI Lua state: every 0.25 s at 60, 0.125 s at 120 until now);
//  - rates (a distance this frame times 30 is a speed: 1/dt): the sprint's
//    wall test (blocked under 1 unit a second; at 60 a sprint from rest
//    started blocked), remote characters' locomotion (NetAIManipulator's 2.5,
//    NetworkManipulator's 0.01/2/6: at 60 a remote player never showed the
//    dash), the attack hitbox velocity (two 4-lane vectors), a physics-driven
//    bone's impulse (close the gap in one physics step), the targeting ring's
//    slot (x30) and overflow (/30);
//  - steps: the equipment screen's character preview, the shop's tab
//    animation.
constexpr DtLoad kDtLoads[] = {
#include "frame_rate_dt_loads.inc"
};
constexpr bool dt_loads_apart() {
    for (const DtLoad& d : kDtLoads) {
        for (const Constant& c : kConstants)
            if (c.at >= d.constant && c.at < d.constant + 4u * d.lanes) return false;
    }
    return true;
}
static_assert(dt_loads_apart(), "a redirected constant is not also rewritten in place (kConstants)");

// The cloth above 60 (tools/fps_sites.py, from Kyo's "90 FPS++"). With one
// substep a frame (0xb6e67a above) the hcl solver runs at the frame rate,
// against the 60 the part files were authored for (30 fps, 2 substeps), and
// the six constraint operators' stiffness takes up the ratio: each gets a
// cave in a dead hkMonitorStream timer block (its guard JAE made a JMP) that
// multiplies the stiffness it is called with (lane 0, which the operator
// broadcasts) by the scalars at 0x4d3bd00 - written over the tail of an
// assert message's file name ("DLLightMutex.cpp"), read only when a mutex
// assert fires. The scalar is (60 dt)^2, at most 1, every frame: Kyo's 1.3
// package (4/9 at 90, 1/4 at 120). His first package multiplied by 60 dt and
// also changed the damping exponent (0xb6fbd4) and the integrator's dt * dt
// (0xb73f84); 1.3 leaves both as the game has them, and so does this table.
// His first 90 list also fixed the character turn's step at 0.0125
// (0x1cbdb42) - turns 12.5% fast at 90 - which 1.3 drops: the turn steps by
// the frame's time, as at every other rate.
constexpr Site kClothSites[] = {
#include "ninety_fps_sites.inc"
};

// No redirected load is an instruction another table rewrites (the 60 FPS++
// list points some loads at the frame-time slot; those constants are dead).
constexpr std::size_t hex_len(const char* h) {
    std::size_t n = 0;
    while (h[n]) ++n;
    return n / 2;
}
template <std::size_t N>
constexpr bool dt_loads_clear_of(const Site (&sites)[N]) {
    for (const DtLoad& d : kDtLoads) {
        for (const Site& site : sites) {
            if (d.insn < site.at + hex_len(site.was) && site.at < d.insn + d.len) return false;
        }
    }
    return true;
}
static_assert(dt_loads_clear_of(kSixtySites) && dt_loads_clear_of(kFrameTimeSites) && dt_loads_clear_of(kClothSites),
              "a redirected load is not rewritten by another table");
constexpr std::uint32_t kClothScalars = 0x4d3bd00;  // 4 x the stiffness scale
constexpr std::uint8_t kClothScalarsWas[16] = {'D', 'L', 'L', 'i', 'g', 'h', 't', 'M', 'u', 't', 'e', 'x', '.', 'c', 'p', 'p'};

constexpr int kUncappedMaxFps = 240, kUncappedReferenceFps = 120;
std::string mode_name(int fps) { return fps == 0 ? "uncapped" : std::to_string(fps); }
std::vector<std::uint8_t> hex_bytes(const char* h) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; h[i] && h[i + 1]; i += 2) {
        auto nib = [](char c) { return static_cast<std::uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
        out.push_back(static_cast<std::uint8_t>(nib(h[i]) << 4 | nib(h[i + 1])));
    }
    return out;
}

float bits_float(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}
std::uint32_t float_bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

// A rated constant at a frame rate (fractional for the uncapped mode's frame
// time). At 60 the table's own value is written instead (the same within a
// rounding of Decay's).
float rated_value(Kind kind, float was, double fps) {
    switch (kind) {
        case Kind::Step: return static_cast<float>(1.0 / fps);
        case Kind::Amount: return static_cast<float>(static_cast<double>(was) * 30.0 / fps);
        case Kind::Decay: return static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(was), 30.0 / fps));
        case Kind::Keep: return static_cast<float>(std::pow(static_cast<double>(was), 30.0 / fps));
        case Kind::Rate: return static_cast<float>(static_cast<double>(was) * fps / 30.0);
    }
    return was;
}

const Rated* rated(std::uint32_t at) {
    for (const Rated& r : kRated)
        if (r.at == at) return &r;
    return nullptr;
}

// A count's bytes at a frame rate.
void count_bytes(const Counted& c, int fps, std::uint8_t* out) {
    const std::uint64_t v = c.kind == Count::Frames ? static_cast<std::uint64_t>(c.was) * static_cast<std::uint64_t>(fps) / 30
                                                    : static_cast<std::uint64_t>(c.was) * 30 / static_cast<std::uint64_t>(fps);
    if (c.kind == Count::Digits) {
        char d[16];
        std::snprintf(d, sizeof(d), "%05u", static_cast<unsigned>(std::min<std::uint64_t>(v, 99999)));
        std::memcpy(out, d, c.n);
        return;
    }
    const auto u = static_cast<std::uint32_t>(v);
    std::memcpy(out, &u, c.n);
}
void count_was(const Counted& c, std::uint8_t* out) {
    if (c.kind == Count::Digits) {
        char d[16];
        std::snprintf(d, sizeof(d), "%05u", static_cast<unsigned>(c.was));
        std::memcpy(out, d, c.n);
        return;
    }
    std::memcpy(out, &c.was, c.n);
}

// The game's pace: 30, a converted rate (60, 90, 120), or 0 for uncapped.
int g_game_fps = 30;
std::size_t g_sites_applied = 0, g_frame_time_applied = 0, g_cloth_applied = 0;
std::atomic<std::uint64_t> g_frames{0};
// The every-frame values (kRated's each_frame) at every converted rate: where
// each goes and what it started from; above 60 the cloth's stiffness scale,
// over a frame time smoothed across ~10 frames so a hitch does not jolt it.
struct EachFrame {
    std::uint64_t va;
    float was;
    Kind kind;
};
std::vector<EachFrame> g_each;
std::uint64_t g_cloth_va = 0;
float g_cloth_dt = 1.0f / 60.0f;
// The play time's count (kPlayTimeAdd) and the part of a millisecond carried.
std::uint64_t g_play_ms_va = 0;
double g_play_ms_carry = 0.0;
// The task step the cave loads (kStepLoad); while it is ours a plugin's time
// scale scales the step, not the frames' pace.
std::uint64_t g_step_va = 0;
std::uint64_t g_wind_step_va = 0;
std::atomic<bool> g_scale_by_step{false};
// The redirected loads' slots (kDtLoads): one per constant, kind and width,
// 16 bytes each in the image's tail, rewritten every frame from the constant
// as it is then (a .data tunable the debug menu edits keeps working).
struct DtSlot {
    std::uint64_t va, src;
    Kind kind;
    std::uint8_t lanes;
};
std::vector<DtSlot> g_dt_slots;
std::size_t g_dt_loads_applied = 0;

template <typename T>
void put(std::uint64_t a, T v) {
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof(v));
}
template <typename T>
T get(std::uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof(v));
    return v;
}

struct FlipClock {
    // The clock as the eboot reads it: gettimeofday in microseconds. In a
    // replay (compare mode) it returns the release time the guest reached,
    // and the sleep does nothing.
    virtual std::int64_t now() = 0;
    virtual void sleep_us(std::int64_t us) = 0;
    // How close to the deadline the wait sleeps before it spins: the eboot's
    // 5 ms, or less for a host sleep that keeps time better (RealClock).
    virtual float spin_margin_s() { return 0.00499999989f; }
    // The least time left that is worth a sleep: the eboot's 1 ms.
    virtual float min_sleep_s() { return 0.00100000005f; }
    // Between two reads of the clock while spinning.
    virtual void spin_pause() {}
    // The wait is over (accounting only).
    virtual void wait_done() {}
    virtual ~FlipClock() = default;
};

// The limiter's margin, learnt from its own sleeps (CPU-04). The eboot
// sleeps to 5 ms before the deadline and spins the rest on gettimeofday: a
// fifth of the main thread's CPU at 60 fps went there (the 2026-10-04
// profile), taken from its SMT sibling - and on an APU (the Radeon 8060S),
// from the GPU's share of the package's power. This is the cost Kyo found in
// KyoPS4x's traces as "the game calls gettimeofday constantly" (Game:Main
// ~30% in posix_gettimeofday at a cap). A fixed 2 ms kept most of it back on
// Linux, where 1 ms cost flip-interval p95 a millisecond on a busy machine;
// how late a sleep wakes is the host's, though: Windows' high-resolution
// waitable timer is usually a few hundred microseconds late, a loaded Linux
// box more. So every sleep's lateness is kept (the last 128), and the margin
// is the second-latest of them plus 200 us, within 0.3-2 ms; it starts at
// 2 ms and moves after 32 sleeps. BBHOST_LIMITER_MARGIN_US=<n> fixes it
// (2000 is the old behaviour).
struct LimiterTuning {
    std::int64_t fixed_us = -1;  // BBHOST_LIMITER_MARGIN_US, -1 for learnt
    std::int64_t late_us[128] = {};
    std::uint32_t samples = 0;
    std::int64_t margin_us = 2000;
    std::int64_t late_p99_us = 0;  // the second-latest of the window
    // Since the last report (frame_rate_limiter_window): the main thread
    // only writes these, the report thread only reads and resets.
    std::atomic<std::uint64_t> waits{0}, sleeps{0}, spin_us{0}, sleep_late_us{0};
    std::atomic<std::int64_t> shown_margin_us{2000}, shown_late_p99_us{0};
};
LimiterTuning g_limiter;
const bool g_limiter_env = [] {
    if (const char* e = std::getenv("BBHOST_LIMITER_MARGIN_US"); e && *e && std::strcmp(e, "auto") != 0) {
        g_limiter.fixed_us = std::clamp<std::int64_t>(std::strtoll(e, nullptr, 10), 0, 5000);
        g_limiter.margin_us = g_limiter.fixed_us;
        g_limiter.shown_margin_us.store(g_limiter.fixed_us);
    }
    return true;
}();

void limiter_note_sleep(std::int64_t asked_us, std::int64_t took_us) {
    LimiterTuning& t = g_limiter;
    const std::int64_t late = std::max<std::int64_t>(0, took_us - asked_us);
    t.sleeps.fetch_add(1, std::memory_order_relaxed);
    t.sleep_late_us.fetch_add(static_cast<std::uint64_t>(late), std::memory_order_relaxed);
    t.late_us[t.samples % 128] = late;
    ++t.samples;
    if (t.fixed_us >= 0 || t.samples < 32 || (t.samples % 16) != 0) return;
    const std::uint32_t n = std::min<std::uint32_t>(t.samples, 128);
    std::int64_t first = 0, second = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::int64_t v = t.late_us[i];
        if (v > first) {
            second = first;
            first = v;
        } else if (v > second) {
            second = v;
        }
    }
    t.late_p99_us = second;
    t.margin_us = std::clamp<std::int64_t>(second + 200, 300, 2000);
    t.shown_margin_us.store(t.margin_us, std::memory_order_relaxed);
    t.shown_late_p99_us.store(second, std::memory_order_relaxed);
}

struct RealClock final : FlipClock {
    // The guest's gettimeofday clock (hle/libc.cpp), read the same way.
    std::int64_t now() override { return host_clock_realtime_us(); }
    void sleep_us(std::int64_t us) override {
        const std::int64_t t0 = now();
        host_sleep_us(static_cast<std::uint64_t>(us));
        const std::int64_t t1 = now();
        limiter_note_sleep(us, t1 - t0);
        slept_until_ = t1;
        spun_from_ = 0;
    }
    float spin_margin_s() override { return static_cast<float>(g_limiter.margin_us) * 1e-6f; }
    // A sleep is worth taking down to the margin itself, not only above the
    // eboot's 1 ms: with a margin under 1 ms the remainder would be spun.
    float min_sleep_s() override { return spin_margin_s(); }
    // The spin pauses between reads.
    void spin_pause() override {
        if (!spun_from_) spun_from_ = slept_until_ ? slept_until_ : now();
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
    void wait_done() override {
        g_limiter.waits.fetch_add(1, std::memory_order_relaxed);
        if (spun_from_) g_limiter.spin_us.fetch_add(static_cast<std::uint64_t>(std::max<std::int64_t>(0, now() - spun_from_)), std::memory_order_relaxed);
        slept_until_ = 0;
        spun_from_ = 0;
    }

private:
    // This frame's wait (one RealClock lives for one update): when its last
    // sleep ended, and when its spin began.
    std::int64_t slept_until_ = 0;
    std::int64_t spun_from_ = 0;
};

struct ReplayClock final : FlipClock {
    std::int64_t at;
    explicit ReplayClock(std::int64_t t) : at(t) {}
    std::int64_t now() override { return at; }
    void sleep_us(std::int64_t) override {}
};

// The unsigned-to-float conversion the compiler emitted for the frame's
// microseconds (a negative count halves and doubles); the count is never
// negative in practice, so this is float(int64).
inline float us_to_float(std::int64_t us) {
    if (us >= 0) return static_cast<float>(us);
    const auto u = static_cast<std::uint64_t>(us);
    const float h = static_cast<float>((u >> 1) | (u & 1));
    return h + h;
}

inline std::int64_t seconds_to_us(float s) {
    const double d = static_cast<double>(s) * 1000000.0;  // the eboot converts to double first
    return d >= 9223372036854775808.0 ? static_cast<std::int64_t>(d - 9223372036854775808.0) ^ INT64_MIN
                                      : static_cast<std::int64_t>(d);
}

// The window object at data_5940e00: told once to run at 60 Hz when the
// flipper is asked to (+0x4c, +0x4d flags and +0x50 = 60).
constexpr std::uint64_t kWindowObjectSlot = 0x5940e00;
std::uint64_t g_slide = 0;
// This frame runs with no target (a loading screen, engine/loading.h); set by
// the hook for its own update only, never for compare mode's replay.
bool g_fast_target = false;
constexpr float kThirtieth = 0.0333333351f, kSixtieth = 0.0166666675f;  // 0x3d088889, 0x3c888889

// `target`: 0 runs the game's own mode (30); above 0 the converted pace, the
// game's mode 2 with this target - kSixtieth at 60, so 60 is the eboot's own
// mode 2 exactly.
void sprj_flipper_update(SprjFlipper* f, FlipClock& clock, float target) {
    const bool converted = target > 0.0f;
    f->secondary_mode_active = f->secondary_mode_requested;
    f->secondary_mode_requested = 0;
    std::uint32_t mode = f->secondary_mode_active ? f->secondary_flip_mode_raw : f->primary_flip_mode_raw;
    if (converted) mode = static_cast<std::uint32_t>(SprjFlipMode::Fps60WithoutSkip);
    switch (mode) {
        case 0: f->frame_advance_count = 2; f->allow_frame_skip = 1; f->target_frame_seconds = kThirtieth; f->foreground_history_count = 1; f->background_history_count = 30; break;
        case 1: f->frame_advance_count = 2; f->allow_frame_skip = 1; f->target_frame_seconds = kThirtieth; f->foreground_history_count = 0; f->background_history_count = 30; break;
        case 2: f->frame_advance_count = 1; f->allow_frame_skip = 1; f->target_frame_seconds = kSixtieth; f->foreground_history_count = 0; f->background_history_count = 30; break;
        case 3: f->frame_advance_count = 1; f->allow_frame_skip = 0; f->target_frame_seconds = kThirtieth; f->foreground_history_count = 1; f->background_history_count = 30; break;
        case 4: f->frame_advance_count = 1; f->allow_frame_skip = 0; f->target_frame_seconds = kThirtieth; f->foreground_history_count = 0; f->background_history_count = 30; break;
        // Paleblood's reference and the original 1.09 instructions both keep
        // these fields unchanged for an invalid mode.
        default: break;
    }
    if (converted) {
        f->target_frame_seconds = target;
        // The 60 FPS patch list zeroes both history counts (its two movabs
        // edits); the same here.
        f->foreground_history_count = 0;
        f->background_history_count = 0;
        // A loading screen (engine/loading.h): no target, so no wait; the
        // 60 FPS++ frame-time cave (0x2418e3d) clamps the measured frame time
        // to [target, 1/30], so game time stays real time.
        if (g_fast_target) f->target_frame_seconds = 0.0f;
    }
    bool no_wait = false;
    if (!converted && f->reset_to_30_fps_pending) {
        f->frame_advance_count = 1;
        f->allow_frame_skip = 0;
        f->target_frame_seconds = kThirtieth;
        f->foreground_history_count = 0;
        f->background_history_count = 0;
        f->reset_to_30_fps_pending = 0;
        no_wait = true;
    }
    if (!converted) {
        if (f->override_foreground_history_count >= 0) f->foreground_history_count = static_cast<std::uint32_t>(f->override_foreground_history_count);
        if (f->override_background_history_count >= 0) f->background_history_count = static_cast<std::uint32_t>(f->override_background_history_count);
        if (f->reset_frame_history) {
            f->foreground_history_count = 0;
            f->background_history_count = 0;
            f->reset_frame_history = 0;
        }
        if (f->force_frame_advance) {  // one 64-bit store over both counts; the flag is not cleared here
            f->foreground_history_count = 0;
            f->background_history_count = 0;
        }
    } else {
        f->override_foreground_history_count = -1;
        f->override_background_history_count = -1;
        f->reset_frame_history = 0;
        f->force_frame_advance = 0;
        f->reset_to_30_fps_pending = 0;
    }
    f->previous_frame_microseconds = f->current_frame_microseconds;
    std::int64_t now = clock.now();
    f->current_frame_microseconds = static_cast<std::uint64_t>(now);
    // A plugin's time scale (frame_rate_set_time_scale): at 30 the game
    // advances a fixed step a frame, so a longer frame is slower time and a
    // shorter one faster, as far as the machine keeps up. At a converted rate
    // with the step ours (each_frame) the step scales and the pace stays.
    const float pace_scale = converted && g_scale_by_step.load(std::memory_order_relaxed)
                                 ? 1.0f
                                 : g_time_scale.load(std::memory_order_relaxed);
    std::int64_t interval_us = seconds_to_us(f->target_frame_seconds / pace_scale);

    // The last frames that ran behind, up to the foreground count of them:
    // how much time they took together.
    const std::uint32_t allowance = f->foreground_history_count;
    std::uint32_t walked = 0;
    std::uint64_t behind_us = 0;
    for (int back = 0; walked < allowance; --back) {
        const SprjFlipperFrameHistoryEntry& e = f->frame_history[(f->frame_history_index + static_cast<std::uint32_t>(back)) & 0x1f];
        behind_us += e.elapsed_microseconds;
        ++walked;
        if (e.delayed_or_behind == 0) break;
    }
    if (f->frame_behind) {
        const std::uint64_t budget = static_cast<std::uint64_t>(walked + 1) * static_cast<std::uint64_t>(interval_us);
        if (behind_us + static_cast<std::uint64_t>(interval_us) > budget) {
            const std::uint64_t third = static_cast<std::uint64_t>(interval_us) / 3;
            if (budget <= behind_us) {
                interval_us = static_cast<std::int64_t>(third);
            } else {
                std::uint64_t rem = budget - behind_us;
                if (static_cast<std::uint64_t>(interval_us) < rem) rem = static_cast<std::uint64_t>(interval_us);
                if (third > rem) rem = third;
                interval_us = static_cast<std::int64_t>(rem);
            }
        }
    }
    if (!(no_wait || f->force_no_sleep)) {
        const float target_s = us_to_float(interval_us) / 1000000.0f;
        const std::int64_t prev = static_cast<std::int64_t>(f->previous_frame_microseconds);
        float remaining = target_s - us_to_float(now - prev) / 1000000.0f;
        const float margin = clock.spin_margin_s();
        const float min_sleep = clock.min_sleep_s();
        while (!(0.0f > remaining)) {
            const float left = std::min(target_s, remaining);
            if (left <= 0.0f) break;
            if (!(left <= min_sleep) && !(left <= margin)) {
                const auto us = static_cast<std::int32_t>((left - margin) * 1000.0f * 1000.0f);
                if (us > 0) clock.sleep_us(us);
            } else {
                clock.spin_pause();
            }
            now = clock.now();
            f->current_frame_microseconds = static_cast<std::uint64_t>(now);
            remaining = target_s + us_to_float(now - prev) / -1000000.0f;
        }
        clock.wait_done();
    }
    if (f->force_no_sleep) f->frame_behind = 1;
    const std::uint32_t idx = (f->frame_history_index + 1) & 0x1f;
    f->frame_history_index = idx;
    const std::int64_t frame_us = static_cast<std::int64_t>(f->current_frame_microseconds - f->previous_frame_microseconds);
    f->frame_history[idx].elapsed_microseconds = static_cast<std::uint64_t>(frame_us);
    f->frame_history[idx].delayed_or_behind = f->frame_behind;
    const float frame_s = us_to_float(frame_us) / 1000000.0f;
    f->measured_frame_seconds = frame_s;
    // On time, unless this frame and the late ones before it overran their
    // budget (plus 1% of the target), in which case only when the allowance
    // is used up.
    bool exhausted = allowance == 0;
    if (f->frame_behind) exhausted = walked >= allowance;
    const std::int64_t tolerance_us = seconds_to_us(f->target_frame_seconds * 0.00999999978f);
    const std::int64_t target_us = seconds_to_us(f->target_frame_seconds);
    const std::uint64_t budget = static_cast<std::uint64_t>(walked + 1) * static_cast<std::uint64_t>(target_us) + static_cast<std::uint64_t>(tolerance_us);
    bool on_time = true;
    if (static_cast<std::uint64_t>(frame_us) + behind_us >= budget) on_time = exhausted;
    if (f->force_no_sleep) on_time = false;
    f->frame_behind = on_time ? 0 : 1;
    if (!on_time) f->frame_advance_count = 0;
    float sum = 0.0f;
    for (int k = 0; k < 15; ++k) {
        const float v = f->fps_frame_time_history[k + 1];
        f->fps_frame_time_history[k] = v;
        sum = sum + v;
    }
    f->fps_frame_time_history[15] = frame_s;
    sum = sum + frame_s;
    f->calculated_fps = sum <= 0.00100000005f ? 0.0f : 16.0f / sum;
    f->force_no_sleep = 0;
    if (g_slide) {
        const auto window = get<std::uint64_t>(g_slide + (kWindowObjectSlot - kPreferredGuestSlide));
        if (window && f->request_window_60_hz) {
            put<std::uint8_t>(window + 0x4c, 1);
            put<std::uint8_t>(window + 0x4d, 1);
            put<std::uint32_t>(window + 0x50, 60);
        }
    }
    f->request_window_60_hz = 0; // one-shot, including a missing notification target
}

float pace_target() { return 1.f / static_cast<float>(g_game_fps ? g_game_fps : kUncappedMaxFps); }
// After the update, at every converted rate, for the frame about to run: the
// task step (kStepLoad) - the measured frame time clamped to [target, 1/30]
// as the 60 FPS++ cave has it (a loading screen has no target: the measured
// time), times a plugin's time scale, at most 1/30 - the play time's
// milliseconds from it, and the values that follow the frame's seconds, from
// dt: the same with the uncapped limit as the floor while a loading screen
// has no target. Above 60 the cloth's stiffness scale: the square of its
// solver rate (one substep a frame) against the 60 its part files were
// authored at, (60 dt)^2, at most 1 (below 60 the cloth keeps the 60
// tuning), over a smoothed frame time. Each store is one aligned value; a
// reader on a worker sees the last frame's value or this one's.
void each_frame(const SprjFlipper* f) {
    const float scale = g_scale_by_step.load(std::memory_order_relaxed) ? g_time_scale.load(std::memory_order_relaxed) : 1.0f;
    const auto scaled = [scale](float s) { return scale == 1.0f ? s : std::min(s * scale, kThirtieth); };
    const float step = scaled(std::min(std::max(f->measured_frame_seconds, f->target_frame_seconds), kThirtieth));
    if (step > 0.0f) {  // also NaN: the cave keeps the last step
        if (g_step_va) put<float>(g_step_va, step);
        if (g_wind_step_va) put<float>(g_wind_step_va, step * 0.5f);
        if (g_play_ms_va) {
            // The step in whole milliseconds, the rest carried to the next frame.
            g_play_ms_carry += static_cast<double>(step) * 1000.0;
            const auto ms = static_cast<std::int32_t>(g_play_ms_carry);
            g_play_ms_carry -= ms;
            put<std::int32_t>(g_play_ms_va, ms);
        }
    }
    const float floor_s = std::max(f->target_frame_seconds, 1.0f / static_cast<float>(kUncappedMaxFps));
    const float dt = scaled(std::min(std::max(f->measured_frame_seconds, floor_s), kThirtieth));
    if (!(dt > 0.0f)) return;  // also NaN
    frame_rate_step::g_frame_seconds.store(dt, std::memory_order_relaxed);
    const double fps = 1.0 / static_cast<double>(dt);
    for (const EachFrame& e : g_each) put<float>(e.va, rated_value(e.kind, e.was, fps));
    for (const DtSlot& d : g_dt_slots) {
        for (std::uint8_t k = 0; k < d.lanes; ++k) put<float>(d.va + 4u * k, rated_value(d.kind, get<float>(d.src + 4u * k), fps));
    }
    g_cloth_dt += (dt - g_cloth_dt) * 0.1f;
    const float cloth = std::min(1.0f, 60.0f * g_cloth_dt);
    if (g_cloth_va) {
        for (int k = 0; k < 4; ++k) put<float>(g_cloth_va + 4 * static_cast<std::uint64_t>(k), cloth * cloth);
    }
    // BBHOST_FRAME_RATE_LOG=1 (or the older BBHOST_UNCAPPED_LOG=1): what this
    // wrote, once a second.
    static const bool log_each = [] {
        const char* e = std::getenv("BBHOST_FRAME_RATE_LOG");
        const char* u = std::getenv("BBHOST_UNCAPPED_LOG");
        return (e && e[0] == '1') || (u && u[0] == '1');
    }();
    if (log_each) {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1)) {
            last = now;
            host_log("frame rate: frame %.2f ms (%.0f fps, %.0f measured over 16), momentum kept %.5f, sprint test x%.1f, "
                     "cloth stiffness %.3f",
                     static_cast<double>(dt) * 1000.0, fps, static_cast<double>(f->calculated_fps),
                     static_cast<double>(rated_value(Kind::Keep, 0.95f, fps)),
                     static_cast<double>(rated_value(Kind::Rate, 30.0f, fps)),
                     static_cast<double>(g_cloth_va ? cloth * cloth : 1.0f));
        }
    }
}

void frame_update(SprjFlipper* f) {
    RealClock clock;
    sprj_flipper_update(f, clock, pace_target());
    each_frame(f);
    g_frames.fetch_add(1, std::memory_order_relaxed);
}
// Preserve the original camera constructor and parameter stores.
bool camera_original() { return true; }
// Whether every site of a group holds the list's old bytes; the first that
// does not is named.
template <std::size_t N>
bool sites_hold(ElfImage* image, const Site (&sites)[N], const char* group) {
    for (const Site& site : sites) {
        const std::vector<std::uint8_t> was = hex_bytes(site.was);
        const std::uint64_t at = image->mem.slide + (site.at - kPreferredGuestSlide);
        if (std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), was.data(), was.size()) != 0) {
            host_log("frame rate: 0x%x does not hold %s's old bytes (%zu bytes); its %zu code rewrites refused", site.at, group,
                     was.size(), N);
            return false;
        }
    }
    return true;
}

// Checks every value and site, picks the mode they allow, then writes. 30
// (nothing written) if a value table does not hold the eboot's values; 60
// without the code rewrites if those do not hold theirs; 60 if 90's or
// uncapped's do not. Never part of a group.
int apply_rate(ElfImage* image, int want) {
    const auto va = [&](std::uint32_t bn) { return image->mem.slide + (bn - kPreferredGuestSlide); };
    for (const Constant& c : kConstants) {
        if (get<std::uint32_t>(va(c.at)) != c.was) {
            host_log("frame rate: refused, 0x%x holds 0x%08x, not 0x%08x", c.at, get<std::uint32_t>(va(c.at)), c.was);
            return 30;
        }
    }
    for (const Counted& c : kCounts) {
        std::uint8_t was[8];
        count_was(c, was);
        if (std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va(c.at))), was, c.n) != 0) {
            host_log("frame rate: refused, 0x%x does not hold %u", c.at, c.was);
            return 30;
        }
    }
    for (const Bytes& b : kBytes) {
        if (std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va(b.at))), b.was, b.n) != 0) {
            host_log("frame rate: refused, 0x%x does not hold the expected %u bytes", b.at, b.n);
            return 30;
        }
    }
    // The code rewrites, by group: all of a group or none of it. The
    // frame-time sites read the slot the 60 FPS++ cave keeps, so they go with
    // the 60 FPS++ sites; the cloth's caves are for rates above 60.
    const bool sites_on = true;
    const bool sixty_ok = sites_on && sites_hold(image, kSixtySites, "the 60 FPS++ list") &&
                          sites_hold(image, kFrameTimeSites, "the frame-time sites") &&
                          sites_hold(image, kWindStepLoads, "the original wind-step loads");
    int mode = want;
    if (mode != 60) {
        const bool cloth_ok = sixty_ok && sites_hold(image, kClothSites, "the cloth caves") &&
                              std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va(kClothScalars))),
                                          kClothScalarsWas, sizeof(kClothScalarsWas)) == 0;
        if (!cloth_ok) {
            host_log("frame rate: %s needs the 60 FPS++, frame-time and cloth code rewrites, which were refused; the game "
                     "runs at 60",
                     mode_name(mode).c_str());
            mode = 60;
        }
    }

    // The 1.09 image's executable segment ends at 0x54d96dc (its first PT_LOAD);
    // a site past it is in the read-write data segment, and its page goes back
    // to read-write - the frame-time slot (0x575a908) is written every frame.
    constexpr std::uint32_t kCodeEnd = 0x54d96dc;
    const auto write = [&](std::uint32_t bn, const void* src, std::size_t n) {
        g_timing_owned.push_back({bn, n});
        const std::uint64_t at = va(bn);
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(at)), src, n);
    };
    // Preserve the original camera constructor and parameter stores.
    const bool original_camera = camera_original();
    const auto write_sites = [&](const auto& sites) {
        std::size_t n = 0;
        for (const Site& site : sites) {
            if (original_camera && kyo_camera_site(site.at)) continue;
            const std::vector<std::uint8_t> now = hex_bytes(site.now);
            write(site.at, now.data(), now.size());
            ++n;
        }
        return n;
    };
    // The reference rate's values for uncapped's code immediates and frame
    // counts; the sound thread keeps up with the fastest frame.
    const int fps = mode == 0 ? kUncappedReferenceFps : mode;
    // A rated constant's rule gives the table's own value at 60.
    std::size_t rated_fixed = 0, camera_kept = 0;
    for (const Constant& c : kConstants) {
        if (original_camera && kyo_camera_constant(c.at)) {
            ++camera_kept;
            continue;
        }
        std::uint32_t now = c.now;
        if (const Rated* r = rated(c.at); r && mode != 60) {
            now = float_bits(rated_value(r->kind, bits_float(c.was), fps));
            rated_fixed += r->each_frame ? 0 : 1;
        }
        write(c.at, &now, 4);
    }
    for (const Counted& c : kCounts) {
        std::uint8_t now[8];
        count_bytes(c, c.kind == Count::Digits && mode == 0 ? kUncappedMaxFps : fps, now);
        write(c.at, now, c.n);
    }
    for (const Bytes& b : kBytes) write(b.at, b.now, b.n);
    // After the constants: two frame-time sites rewrite instructions whose
    // immediates kConstants converts (0x1bf9b74, 0x222bcd7, for a run
    // without the code rewrites).
    if (sixty_ok) {
        g_sites_applied = write_sites(kSixtySites);
        g_frame_time_applied = write_sites(kFrameTimeSites);
        // The task step and the play time's count, ours: written by our
        // SprjFlipper::Update every frame, in 16 bytes of the image's tail
        // (plus an immutable 16-byte authored wind-vector scale when enabled)
        const std::uint64_t block = image->tail_va + image->tail_used + kPreferredGuestSlide;  // as a BN address
        const std::uint64_t step_bn = block, wind_bn = block + 4, play_bn = block + 8;
        const std::int64_t step_disp = static_cast<std::int64_t>(step_bn) - static_cast<std::int64_t>(kStepLoad + 8);
        const std::int64_t play_disp = static_cast<std::int64_t>(play_bn) - static_cast<std::int64_t>(kPlayTimeAdd + 6);
        const auto holds = [&](std::uint32_t at, const char* hex) {
            const std::vector<std::uint8_t> was = hex_bytes(hex);
            return std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va(at))), was.data(), was.size()) == 0;
        };
        const auto rel32 = [](std::int64_t d) { return d >= INT32_MIN && d <= INT32_MAX; };
        const bool wind_reachable = std::all_of(std::begin(kWindStepLoads), std::end(kWindStepLoads),
            [&](const Site& load) {
                return rel32(static_cast<std::int64_t>(wind_bn) - (static_cast<std::int64_t>(load.at) + 8));
            });
        const char* wind_vector_setting = std::getenv("BBHOST_WIND_VECTOR_FIXED");
        const bool wind_vector_enabled = !(wind_vector_setting && wind_vector_setting[0] == '0');
        const std::uint64_t vector_bn = block + 16;
        MacWindVectorPatch::Rewrite wind_vector_code{};
        const bool wind_vector_ok = wind_vector_enabled && image->tail_used + 32 <= image->tail_size &&
            vector_bn <= UINT32_MAX && MacWindVectorPatch::Build(
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va(MacWindVectorPatch::Site))),
                MacWindVectorPatch::Context.size(), MacWindVectorPatch::Site, vector_bn, wind_vector_code);
        if (image->tail_used + 16 > image->tail_size || !rel32(step_disp) || !rel32(play_disp) || !wind_reachable) {
            host_log("frame rate: no room in the image's tail; the task step and the play time stay the game's");
        } else {
            image->tail_used += wind_vector_ok ? 32 : 16;
            const auto le = [](std::int64_t d, std::uint8_t* out) {
                const auto u = static_cast<std::uint32_t>(static_cast<std::int32_t>(d));
                for (int k = 0; k < 4; ++k) out[k] = static_cast<std::uint8_t>(u >> (8 * k));
            };
            if (holds(kStepLoad, kStepLoadWas)) {
                g_step_va = va(static_cast<std::uint32_t>(step_bn));
                put<float>(g_step_va, 1.0f / static_cast<float>(fps));
                std::uint8_t now[21] = {0xf3, 0x0f, 0x10, 0x05, 0, 0, 0, 0,               // movss xmm0, [step]
                                        0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,  // nop8
                                        0x0f, 0x1f, 0x44, 0x00, 0x00};                   // nop5
                le(step_disp, now + 4);
                write(kStepLoad, now, sizeof(now));
                g_scale_by_step.store(true, std::memory_order_relaxed);

                // The spare aligned word in this owned tail block carries
                // the authored wind clock. All original instructions were
                // checked with sixty_ok before any list was written.
                g_wind_step_va = va(static_cast<std::uint32_t>(wind_bn));
                put<float>(g_wind_step_va, 0.5f / static_cast<float>(fps));
                for (const Site& load : kWindStepLoads) {
                    const auto displacement = static_cast<std::int32_t>(
                        static_cast<std::int64_t>(wind_bn) - (static_cast<std::int64_t>(load.at) + 8));
                    write(load.at + 4, &displacement, sizeof(displacement));
                }
                host_log("frame rate: wind follows half the task step (the original 30 fps sway speed)");

                // REA: 0x2314a66's XYZ product reaches the two published wind
                // vector banks; it is not an accumulated position. Using the
                // fluctuating phase delta here makes bend amplitude pop with
                // frame time. Keep the original 30-fps vector scale (1/60),
                // while the four redirected loads continue to advance phase
                // and fades by half the actual task step. The independent
                // scalar product at 0x2314ae7 is deliberately unchanged.
                if (wind_vector_ok) {
                    const float authored[4] = {MacWindVectorPatch::AuthoredSeconds, MacWindVectorPatch::AuthoredSeconds,
                                               MacWindVectorPatch::AuthoredSeconds, MacWindVectorPatch::AuthoredSeconds};
                    write(static_cast<std::uint32_t>(vector_bn), authored, sizeof(authored));
                    write(MacWindVectorPatch::Site, wind_vector_code.data(), wind_vector_code.size());
                    host_log("frame rate: published wind XYZ uses the authored 1/60 scale; phase/fades remain variable");
                } else if (!wind_vector_enabled) {
                    host_log("frame rate: BBHOST_WIND_VECTOR_FIXED=0, wind XYZ retains the variable-step control");
                } else {
                    host_log("frame rate: wind XYZ rewrite refused (original bytes, tail capacity or rel32); phase unchanged");
                }
            } else {
                host_log("frame rate: 0x%x does not hold the 60 FPS++ cave's step; the cave keeps computing it", kStepLoad);
            }
            if (holds(kPlayTimeAdd, kPlayTimeAddWas)) {
                g_play_ms_va = va(static_cast<std::uint32_t>(play_bn));
                put<std::int32_t>(g_play_ms_va, static_cast<std::int32_t>(1000 / fps));
                std::uint8_t now[13] = {0x8b, 0x0d, 0, 0, 0, 0,                    // mov ecx, [count]
                                        0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00};  // nop7
                le(play_disp, now + 2);
                write(kPlayTimeAdd, now, sizeof(now));
            } else {
                host_log("frame rate: 0x%x does not hold the play time's add; the game's stays", kPlayTimeAdd);
            }
        }
        // The redirected loads (kDtLoads): every instruction checked, then
        // the slots placed and the displacements pointed at them.
        bool dt_ok = sizeof(kDtLoads) > 0;
        for (const DtLoad& d : kDtLoads) {
            const std::vector<std::uint8_t> was = hex_bytes(d.bytes);
            const auto* p = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(va(d.insn)));
            std::int32_t disp;
            std::memcpy(&disp, p + d.disp, 4);
            if (was.size() != d.len || std::memcmp(p, was.data(), was.size()) != 0 ||
                static_cast<std::int64_t>(d.insn) + d.len + disp != static_cast<std::int64_t>(d.constant)) {
                host_log("frame rate: 0x%x is not the load of 0x%x the redirected loads expect; the %zu are left out", d.insn,
                         d.constant, sizeof(kDtLoads) / sizeof(kDtLoads[0]));
                dt_ok = false;
                break;
            }
        }
        if (dt_ok) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> keys;  // (constant | kind | lanes, slot BN address)
            const auto key_of = [](const DtLoad& d) {
                return static_cast<std::uint64_t>(d.constant) << 16 | static_cast<std::uint64_t>(d.kind) << 8 | d.lanes;
            };
            for (const DtLoad& d : kDtLoads) {
                if (std::none_of(keys.begin(), keys.end(), [&](const auto& k) { return k.first == key_of(d); })) keys.push_back({key_of(d), 0});
            }
            const std::uint64_t start = (image->tail_used + 15) & ~std::uint64_t(15);
            if (start + 16 * keys.size() > image->tail_size) {
                host_log("frame rate: no room in the image's tail for %zu redirected loads' slots; left out", keys.size());
            } else {
                image->tail_used = start + 16 * keys.size();
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    keys[i].second = image->tail_va + start + 16 * i + kPreferredGuestSlide;  // as a BN address
                    DtSlot slot{};
                    slot.va = va(static_cast<std::uint32_t>(keys[i].second));
                    const auto constant = static_cast<std::uint32_t>(keys[i].first >> 16);
                    slot.kind = static_cast<Kind>((keys[i].first >> 8) & 0xff);
                    slot.lanes = static_cast<std::uint8_t>(keys[i].first & 0xff);
                    slot.src = va(constant);
                    for (std::uint8_t k = 0; k < slot.lanes; ++k)
                        put<float>(slot.va + 4u * k, rated_value(slot.kind, get<float>(slot.src + 4u * k), fps));
                    g_dt_slots.push_back(slot);
                }
                for (const DtLoad& d : kDtLoads) {
                    const auto it = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return k.first == key_of(d); });
                    const std::int64_t nd = static_cast<std::int64_t>(it->second) - (static_cast<std::int64_t>(d.insn) + d.len);
                    if (nd < INT32_MIN || nd > INT32_MAX) continue;  // the tail is within reach of all of the code
                    const auto d32 = static_cast<std::int32_t>(nd);
                    write(d.insn + d.disp, &d32, 4);
                    ++g_dt_loads_applied;
                }
            }
        }
    }
    // No decompiled camera replacement is installed in this port. Preserve the
    // original constructor/stores; the camera constants in kRated follow dt.
    host_log("original camera constructor retained (%zu defaults)", camera_kept);
    if (mode != 60) {
        g_cloth_applied = write_sites(kClothSites);
        g_cloth_va = va(kClothScalars);
        const float scale = std::min(1.0f, 60.0f / static_cast<float>(fps));
        const float scalars[4] = {scale * scale, scale * scale, scale * scale, scale * scale};
        write(kClothScalars, scalars, sizeof(scalars));
        g_cloth_dt = 1.0f / static_cast<float>(fps);
    }
    // The every-frame values: their pages stay writable for good (they are
    // .rodata in the executable segment, RWX in case a page also holds code),
    // whatever another writer does to them later - live resolution writes
    // the plate sizes beside the sprint test's value and puts that page back
    // to read-only (core/memory.h).
    std::vector<std::uint64_t> pages;
    for (const Rated& r : kRated) {
        if (!r.each_frame) continue;
        const Constant* c = std::find_if(std::begin(kConstants), std::end(kConstants), [&](const Constant& k) { return k.at == r.at; });
        g_each.push_back({va(r.at), bits_float(c->was), r.kind});
        pages.push_back(va(r.at) & ~0xfffull);
    }
    if (g_cloth_va) pages.push_back(g_cloth_va & ~0xfffull);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (const std::uint64_t p : pages) {
        if (p < va(kCodeEnd)) guest_keep_writable(&image->mem, p);  // .data pages are writable already
    }
    host_log("frame rate: %zu values%s follow the frame time every frame (%zu pages of .rodata kept writable), and %zu loads "
             "read %zu slots of ours that do; %zu other constants follow the rate (code immediates and counts at %d's values), "
             "the rest at their 60 values",
             g_each.size(), g_cloth_va ? " and the cloth's stiffness scale" : "", pages.size(), g_dt_loads_applied, g_dt_slots.size(),
             rated_fixed, fps);
    return mode;
}

// Validate all input groups before the first mutation. A partial timing conversion
// is unsafe; unlike the source's optional fallback groups, refuse the whole install.
void preflight(const ElfImage& image, int cap) {
    auto holds = [&](std::uint32_t bn, const void* expected, std::size_t n) {
        const std::uint64_t offset = std::uint64_t(bn) - kPreferredGuestSlide;
        if (bn < kPreferredGuestSlide || offset > image.mem.size || n > image.mem.size - offset ||
            std::memcmp(reinterpret_cast<const void*>(image.mem.slide + offset), expected, n))
            throw std::runtime_error("unsupported timing bytes at " + std::to_string(bn));
    };
    holds(kFrameManager, kFrameManagerPrologue, sizeof(kFrameManagerPrologue));
    for (const auto& c : kConstants) holds(c.at, &c.was, 4);
    for (const auto& c : kCounts) { std::uint8_t was[8]; count_was(c, was); holds(c.at, was, c.n); }
    for (const auto& b : kBytes) holds(b.at, b.was, b.n);
    auto group = [&](const auto& sites) {
        for (const auto& site : sites) { const auto was = hex_bytes(site.was); holds(site.at, was.data(), was.size()); }
    };
    group(kSixtySites); group(kFrameTimeSites); group(kWindStepLoads);
    if (cap != 60) { group(kClothSites); holds(kClothScalars, kClothScalarsWas, sizeof(kClothScalarsWas)); }
    for (const auto& d : kDtLoads) {
        const auto was = hex_bytes(d.bytes); holds(d.insn, was.data(), was.size());
        std::int32_t displacement; std::memcpy(&displacement, was.data() + d.disp, 4);
        if (was.size() != d.len || std::int64_t(d.insn) + d.len + displacement != d.constant ||
            std::uint64_t(d.constant) + 4 * d.lanes > kPreferredGuestSlide + image.mem.size)
            throw std::runtime_error("unsupported timing load");
    }
    const auto play = hex_bytes(kPlayTimeAddWas); holds(kPlayTimeAdd, play.data(), play.size());
    holds(MacWindVectorPatch::Site, MacWindVectorPatch::Context.data(), MacWindVectorPatch::Context.size());
    // Every RIP-relative slot stays in the same mapping, within signed rel32 reach.
    if (image.mem.size < kWindowObjectSlot - kPreferredGuestSlide + 8 ||
        image.tail_va + TailSize + kPreferredGuestSlide > INT32_MAX ||
        32 + 16 * std::size(kDtLoads) > TailSize)
        throw std::runtime_error("unsupported timing image extent");
}
std::uint64_t tail_address = 0;
} // namespace

extern "C" std::size_t bb_fps_tail_size() { return TailSize; }
extern "C" int bb_fps_install(unsigned char* base, std::uint64_t size, int cap) {
    if (cap == 30) return 0;
    try {
        if (!base || (cap != 0 && (cap < 31 || cap > kUncappedMaxFps)) || g_slide ||
            size > 512u * 1024 * 1024) throw std::runtime_error("invalid timing installation");
        const auto page = static_cast<std::uint64_t>(getpagesize());
        const auto tail = (size + page - 1) & ~(page - 1);
        ElfImage image{{reinterpret_cast<std::uint64_t>(base), size}, tail, 0, TailSize};
        preflight(image, cap);
        g_slide = image.mem.slide; g_game_fps = cap;
        if (apply_rate(&image, cap) != cap || !g_step_va || !g_wind_step_va || !g_play_ms_va ||
            g_dt_loads_applied != std::size(kDtLoads)) throw std::runtime_error("incomplete timing installation");
        // The complete decoded instructions belong to timing, including their
        // opcodes. Protecting only the rewritten displacements would admit an
        // external opcode edit that invalidates a redirected load.
        for (const auto& d : kDtLoads) g_timing_owned.push_back({d.insn,d.len});
        for (const auto& s : kWindStepLoads) g_timing_owned.push_back({s.at,hex_bytes(s.was).size()});
        tail_address = g_slide + tail;
        // Same SysV ABI as the original x86-64 member; no cross-process callback.
        auto* entry = base + kFrameManager - kPreferredGuestSlide;
        const std::uint8_t jump[]{0xff, 0x25, 0, 0, 0, 0};
        std::memcpy(entry, jump, sizeof(jump));
        const auto target = reinterpret_cast<std::uint64_t>(&frame_update);
        std::memcpy(entry + sizeof(jump), &target, 8);
        std::memset(entry + 14, 0x90, sizeof(kFrameManagerPrologue) - 14);
        g_timing_owned.push_back({kFrameManager, 2077});
        host_log("installed %s FPS timing; %zu redirected loads; original files unchanged",
                 mode_name(cap).c_str(), g_dt_loads_applied);
        return 1;
    } catch (const std::exception& e) { host_log("refused: %s", e.what()); return -1; }
}
extern "C" int bb_fps_conflicts(std::uint64_t offset, std::uint64_t size) {
    if (offset > UINT64_MAX - kPreferredGuestSlide || size > SIZE_MAX) return 1;
    return MacTimingVerify::Conflicts(offset + kPreferredGuestSlide, static_cast<std::size_t>(size));
}
extern "C" int bb_fps_protect() {
    if (!tail_address) return 0;
    const auto page = static_cast<std::uint64_t>(getpagesize());
    for (auto at : writable_pages) {
        at &= ~(page - 1);
        if (mprotect(reinterpret_cast<void*>(at), page, PROT_READ | PROT_WRITE | PROT_EXEC)) return -1;
    }
    return mprotect(reinterpret_cast<void*>(tail_address), TailSize, PROT_READ | PROT_WRITE);
}
namespace MacTimingVerify {
StepResult TickInstalled(float measured_seconds) {
    if (!g_slide || !g_step_va || !std::isfinite(measured_seconds) || measured_seconds < 0)
        throw std::logic_error("Installed timing fixture requires a valid private image");
    SprjFlipper state{};
    state.measured_frame_seconds = measured_seconds;
    state.target_frame_seconds = pace_target();
    each_frame(&state);
    return {get<float>(g_step_va), get<std::int32_t>(g_play_ms_va), get<float>(g_wind_step_va)};
}
const std::vector<OwnedRange>& OwnedRanges() { return g_timing_owned; }
bool Conflicts(std::uint64_t address, std::size_t size) {
    if (!size) return false;
    if (address + size < address) return true;
    for (const auto& r : g_timing_owned)
        if (address < r.address + r.size && r.address < address + size) return true;
    return false;
}
namespace {
void require_uninstalled() {
    if (g_slide || !g_timing_owned.empty()) throw std::logic_error("Timing verification requires a dedicated process before engine installation");
}
}
void Replay(SprjFlipper& state, std::int64_t release_us, float target) {
    require_uninstalled();
    struct BoundedClock final : FlipClock {
        std::int64_t release;
        unsigned reads = 0;
        explicit BoundedClock(std::int64_t t) : release(t) {}
        std::int64_t now() override {
            if (++reads > 8) throw std::runtime_error("Replay release time does not satisfy the frame budget");
            return release;
        }
        void sleep_us(std::int64_t) override {}
    } clock(release_us);
    sprj_flipper_update(&state, clock, target);
}
void ResetStep() {
    require_uninstalled();
    g_play_ms_carry = 0;
    frame_rate_step::g_frame_seconds.store(0);
}
StepResult Step(float measured_seconds, float target_seconds, float scale) {
    require_uninstalled();
    if (!std::isfinite(measured_seconds) || measured_seconds < 0 || !std::isfinite(target_seconds) || target_seconds < 0 ||
        !std::isfinite(scale) || scale < 0.1f || scale > 4) throw std::invalid_argument("Invalid timing fixture");
    float step = 0;
    float wind_step = 0;
    std::int32_t milliseconds = 0;
    g_step_va = reinterpret_cast<std::uint64_t>(&step);
    g_wind_step_va = reinterpret_cast<std::uint64_t>(&wind_step);
    g_play_ms_va = reinterpret_cast<std::uint64_t>(&milliseconds);
    g_scale_by_step.store(true);
    g_time_scale.store(scale);
    SprjFlipper state{};
    state.measured_frame_seconds = measured_seconds;
    state.target_frame_seconds = target_seconds;
    each_frame(&state);
    g_step_va = g_wind_step_va = g_play_ms_va = 0;
    g_time_scale.store(1);
    g_scale_by_step.store(false);
    return {step, milliseconds, wind_step};
}
}
