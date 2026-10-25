// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-only differential fixtures inspired by Paleblood's limiter cases.
// Never calls game startup, initializes SDL, or creates a Metal device/window.
#include "src/fps/timing_verify.h"
#include "src/fps/wind_vector_patch.h"
#include "fps_reference/limiter.h"
#include "src/fps/sprj_flipper.h"
#include "src/fps/frame_rate_step.h"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/mman.h>

static void* runtime_low_map(std::size_t n, int flags) {
    void* p = mmap(nullptr,n,flags,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return p == MAP_FAILED ? nullptr : p;
}

#if defined(__x86_64__)
// Tail-call the emitted instruction bytes with the original register inputs.
// No inline-asm call/red-zone ambiguity; the snippet returns to our caller.
extern "C" void bb_timing_fixture_wind_run(const float*, const float*, float*, const void*);
asm(".text\n.p2align 4\n_bb_timing_fixture_wind_run:\n"
    "vmovups (%rdi), %xmm2\n"
    "vmovss (%rsi), %xmm14\n"
    "vxorps %xmm1, %xmm1, %xmm1\n"
    "jmp *%rcx\n");
#endif

namespace {
constexpr std::uint64_t t0 = 5'000'000'000;
struct Case { std::string name; SprjFlipper before; std::int64_t offset = 5000, increment = 1000; bool spin = true; };
struct ScriptClock final : PalebloodReference::Clock {
    std::uint64_t at, last = 0;
    std::int64_t increment;
    unsigned reads = 0, sleeps = 0;
    ScriptClock(std::int64_t offset, std::int64_t step) : at(t0 + offset), increment(step) {}
    std::uint64_t Now() override {
        // Exhausted scripts cannot count as a match, even if both sides stop.
        if (++reads > 256) { std::fputs("Incomplete clock script\n", stderr); std::exit(3); }
        last = at; at += increment; return last;
    }
    void Sleep(std::uint32_t) override { ++sleeps; }
};
std::vector<Case> cases() {
    SprjFlipper f{};
    f.primary_flip_mode_raw = 1;
    f.secondary_flip_mode_raw = 0;
    f.frame_advance_count = 2; f.allow_frame_skip = 1;
    f.target_frame_seconds = std::bit_cast<float>(0x3d088889u);
    f.previous_frame_microseconds = t0 - 33000; f.current_frame_microseconds = t0;
    f.foreground_history_count = 1; f.background_history_count = 30;
    f.override_foreground_history_count = f.override_background_history_count = -1;
    for (int k = 0; k < 16; ++k) f.fps_frame_time_history[k] = .030f + .0005f * k;
    // Nonzero padding catches implementations that initialize untouched bytes.
    std::memset(f.pad15, 0xa5, sizeof(f.pad15)); std::memset(f.pad1c, 0xa5, sizeof(f.pad1c));
    std::vector<Case> result;
    for (unsigned mode = 0; mode <= 4; ++mode) {
        auto state = f; state.primary_flip_mode_raw = mode;
        result.push_back({"mode_" + std::to_string(mode), state});
    }
    auto add = [&](const char* name, auto edit) { auto state = f; edit(state); result.push_back({name, state}); };
    add("pending_mode", [](auto& s) { s.secondary_mode_requested = 1; s.secondary_flip_mode_raw = 2; });
    add("reset_to_30", [](auto& s) { s.primary_flip_mode_raw = 2; s.reset_to_30_fps_pending = 1; });
    add("override_windows", [](auto& s) { s.override_foreground_history_count = 5; s.override_background_history_count = 12; });
    add("clear_history", [](auto& s) { s.reset_frame_history = 1; });
    add("force_advance", [](auto& s) { s.force_frame_advance = 1; });
    add("forced_late", [](auto& s) { s.force_no_sleep = 1; });
    add("ring_wrap", [](auto& s) { s.frame_history_index = 31; });
    add("catch_up", [](auto& s) { s.frame_behind = 1; s.frame_history[0] = {90000, 1, {}}; });
    add("tiny_history", [](auto& s) { for (float& v : s.fps_frame_time_history) v = 0; s.force_no_sleep = 1; });
    add("missing_notify", [](auto& s) { s.request_window_60_hz = 1; });
    add("invalid_mode", [](auto& s) { s.primary_flip_mode_raw = 9; s.frame_advance_count = 7; });
    result.push_back({"over_budget", f, 50000});
    result.push_back({"exact_budget", f, 33333, 1});
    result.push_back({"sleep_path", f, 1000, 7000, false});
    auto backwards = f; backwards.force_no_sleep = 1;
    result.push_back({"clock_backwards_forced_late", backwards, -2000000, 40000});
    return result;
}

int mismatch(const SprjFlipper& a, const SprjFlipper& b) {
    const auto* left = reinterpret_cast<const unsigned char*>(&a);
    const auto* right = reinterpret_cast<const unsigned char*>(&b);
    for (std::size_t i = 0; i < sizeof(a); ++i) if (left[i] != right[i]) return static_cast<int>(i);
    return -1;
}

unsigned check_steps() {
    unsigned checks = 0;
    for (int fps : {30, 40, 60, 90, 120, 144, 165, 240}) {
        for (float scale : {.5f, 1.f, 2.f}) {
            MacTimingVerify::ResetStep();
            std::int64_t milliseconds = 0;
            const float target = 1.f / fps;
            const float expected = std::min(target * scale, 1.f / 30);
            for (int k = 0; k < fps * 10; ++k) {
                const auto step = MacTimingVerify::Step(target, target, scale);
                if (step.seconds != expected) throw std::runtime_error("Production task step differs from frame-time contract");
                if (step.wind_seconds != expected * 0.5f)
                    throw std::runtime_error("Wind no longer preserves the original 1/60 step at 30 fps");
                milliseconds += step.play_ms;
            }
            if (std::abs(milliseconds - std::int64_t(double(expected) * fps * 10000)) > 1)
                throw std::runtime_error("Play-time clock lost fractional milliseconds");
            ++checks;
        }
        for (float measured : {0.f, .001f, .020f, .2f}) {
            MacTimingVerify::ResetStep();
            const auto step = MacTimingVerify::Step(measured, 1.f / fps);
            if (step.seconds != std::min(std::max(measured, 1.f / fps), 1.f / 30))
                throw std::runtime_error("Task delta clamp differs from FPS++ contract");
            if (step.wind_seconds != step.seconds * 0.5f)
                throw std::runtime_error("Wind does not follow the clamped task delta");
            ++checks;
        }
        // An independent libm oracle for bbhost's camera decay/retention.
        for (float weight : {.01f, .1f, .5f, .95f}) {
            const float frames = 30.f / fps;
            const double expected = 1 - std::pow(1 - double(weight), double(frames));
            if (std::abs(frame_rate_step::weight(weight, frames) - expected) > 1.e-6)
                throw std::runtime_error("Camera smoothing drift");
            ++checks;
        }
    }
    MacTimingVerify::ResetStep();
    // A fluctuating cap and loading frames must use actual measured time.
    for (float measured : {.004f, .025f, .010f, .1f, .006f}) {
        if (MacTimingVerify::Step(measured, 0).seconds != std::min(measured, 1.f / 30))
            throw std::runtime_error("Loading delta clamp differs");
        ++checks;
    }
    return checks;
}

struct WindChecks { unsigned admission = 0, executed = 0; };
WindChecks check_wind_vector() {
    using namespace MacWindVectorPatch;
    WindChecks checks;
    const auto require = [&](bool ok, const char* error) {
        if (!ok) throw std::runtime_error(error);
        ++checks.admission;
    };
    Rewrite output{};
    const Rewrite sentinel{1,2,3,4,5,6,7,8,9,10};
    require(Build(Context.data(), Context.size(), Site, Site + 0x100, output) &&
                output == Rewrite{0xc5,0x68,0x59,0x25,0xf8,0,0,0,0x90,0x90},
            "Wind vector replacement does not encode xmm12=xmm2*[rip+disp32]");
    for (std::size_t k = 0; k < Context.size(); ++k) {
        auto changed = Context; changed[k] ^= 1;
        output = sentinel;
        require(!Build(changed.data(), changed.size(), Site, Site + 0x100, output) && output == sentinel,
                "Changed original wind instruction admitted or refusal changed output");
    }
    for (std::size_t length : {std::size_t(0), Context.size() - 1, Context.size() + 1}) {
        output = sentinel;
        require(!Build(Context.data(), length, Site, Site + 0x100, output) && output == sentinel,
                "Incomplete wind instruction admitted");
    }
    output = sentinel;
    require(!Build(nullptr, Context.size(), Site, Site + 0x100, output) && output == sentinel,
            "Null original wind instruction admitted");
    const std::uint64_t next = 1ull << 33;
    for (const auto delta : {std::int64_t(-0x80000000ll), std::int64_t(-1), std::int64_t(0),
                             std::int64_t(1), std::int64_t(0x7fffffff)}) {
        require(Build(Context.data(), Context.size(), next - 8, next + delta, output),
                "Reachable wind constant refused");
        std::int32_t displacement;
        std::memcpy(&displacement, output.data() + 4, sizeof(displacement));
        require(displacement == delta, "Wind rel32 does not reach its owned constant");
    }
    for (const auto delta : {std::int64_t(-0x80000001ll), std::int64_t(0x80000000ll)}) {
        output = sentinel;
        require(!Build(Context.data(), Context.size(), next - 8, next + delta, output) && output == sentinel,
                "Unreachable wind constant admitted");
    }
    output = sentinel;
    require(!Build(Context.data(), Context.size(), UINT64_MAX - 7, 0, output) && output == sentinel,
            "Overflowing wind instruction admitted");

#if defined(__x86_64__)
    constexpr std::size_t size = 16384;
    auto* code = static_cast<std::uint8_t*>(runtime_low_map(size, PROT_READ | PROT_WRITE));
    if (!code) throw std::runtime_error("Cannot allocate wind instruction fixture");
    struct Release { void* pointer; ~Release() { munmap(pointer, size); } } release{code};
    constexpr std::size_t original_at = 0, replacement_at = 64, constant_at = 128;
    const float authored[4] = {AuthoredSeconds, AuthoredSeconds, AuthoredSeconds, AuthoredSeconds};
    std::memcpy(code + constant_at, authored, sizeof(authored));
    // Store xmm12's product and xmm14's unchanged phase/fade step, then ret.
    constexpr std::uint8_t finish[] = {0xc5,0x78,0x11,0x22,0xc5,0x7a,0x11,0x72,0x10,0xc3};
    std::memcpy(code + original_at, Context.data(), Context.size());
    std::memcpy(code + original_at + Context.size(), finish, sizeof(finish));
    if (!Build(code + original_at, Context.size(), reinterpret_cast<std::uint64_t>(code + replacement_at),
               reinterpret_cast<std::uint64_t>(code + constant_at), output))
        throw std::runtime_error("Cannot build executable wind instruction fixture");
    std::memcpy(code + replacement_at, Context.data(), Context.size());
    std::memcpy(code + replacement_at, output.data(), output.size());
    std::memcpy(code + replacement_at + Context.size(), finish, sizeof(finish));
    if (mprotect(code, size, PROT_READ | PROT_EXEC)) throw std::runtime_error("Cannot execute wind instruction fixture");
    for (const std::array<float,4>& vector : {std::array<float,4>{13.7957659f,8.6758499f,32.8494225f,0.f},
                                            std::array<float,4>{-2.f,.5f,-.125f,4.f},
                                            std::array<float,4>{0.f,-0.f,1.e-10f,-1.e10f}}) {
        std::array<float,4> previous{};
        bool have_previous = false;
        // Includes the observed 1/80 -> 1/60 amplitude-pop transition and
        // rates 30..240. The variable clock must survive both snippets.
        for (const float step : {1.f/480, 1.f/240, 1.f/120, 1.f/80, 1.f/60, 1.f/80, .0031f}) {
            float before[5]{}, after[5]{};
            bb_timing_fixture_wind_run(vector.data(), &step, before, code + original_at);
            bb_timing_fixture_wind_run(vector.data(), &step, after, code + replacement_at);
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (before[lane] != vector[lane] * step || after[lane] != vector[lane] * AuthoredSeconds ||
                    (have_previous && std::bit_cast<std::uint32_t>(after[lane]) != std::bit_cast<std::uint32_t>(previous[lane])))
                    throw std::runtime_error("Emitted wind rewrite still changes amplitude with frame time");
                previous[lane] = after[lane];
            }
            if (before[4] != step || after[4] != step)
                throw std::runtime_error("Emitted wind rewrite changed the phase/fade clock");
            if (step == AuthoredSeconds && std::memcmp(before, after, 4 * sizeof(float)))
                throw std::runtime_error("Emitted wind rewrite differs from original authored 30-fps amplitude");
            have_previous = true;
            ++checks.executed;
        }
    }
#endif
    return checks;
}
}

int main(int argc, char** argv) {
    try {
        bool mutate = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--mutate") mutate = true;
            else if (std::string(argv[i]) != "--self-test") throw std::runtime_error("Usage: timing-test [--self-test] [--mutate]");
        }
        unsigned passed = 0, failed = 0;
        std::printf("{\"schema\":1,\"original_executed\":false,\"cases\":[");
        for (const auto& c : cases()) {
            auto reference = c.before, actual = c.before;
            ScriptClock clock(c.offset, c.increment);
            PalebloodReference::PaceFrame(reference, clock, c.spin, nullptr);
            int original_diff = -1;
            MacTimingVerify::Replay(actual, static_cast<std::int64_t>(clock.last));
            if (mutate) actual.measured_frame_seconds = std::bit_cast<float>(std::bit_cast<std::uint32_t>(actual.measured_frame_seconds) ^ 1u);
            const int diff = mismatch(actual, reference);
            const bool ok = diff == -1 && original_diff == -1;
            std::printf("%s{\"id\":\"%s\",\"passed\":%s,\"bbhost_byte_difference\":%d,\"original_byte_difference\":%d,\"clock_reads\":%u}",
                        passed + failed ? "," : "", c.name.c_str(), ok ? "true" : "false", diff, original_diff, clock.reads);
            if (ok) ++passed; else ++failed;
        }
        const unsigned steps = check_steps();
        const auto wind = check_wind_vector();
        std::printf("],\"passed\":%u,\"failed\":%u,\"step_checks\":%u,\"wind_patch_checks\":%u,\"wind_instruction_cases\":%u,\"mutation\":%s}\n",
                    passed, failed, steps, wind.admission, wind.executed, mutate ? "true" : "false");
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Timing verification incomplete: %s\n", error.what());
        return 3;
    }
}
