// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
struct SprjFlipper;
namespace MacTimingVerify {
// Deterministic entry to the production limiter for CPU-only verification.
// Must be used in a dedicated verifier process, before engine installation.
void Replay(SprjFlipper& state, std::int64_t release_us, float target = 0);
struct StepResult { float seconds; std::int32_t play_ms; float wind_seconds; };
void ResetStep();
StepResult Step(float measured_seconds, float target_seconds, float scale = 1);
// CPU-only installation fixture: run the production writes against an installed
// private image, without entering original game code or waiting for a frame.
StepResult TickInstalled(float measured_seconds);
struct OwnedRange { std::uint64_t address; std::size_t size; };
// Actual successfully installed timing edits; the limiter body is bypassed.
const std::vector<OwnedRange>& OwnedRanges();
bool Conflicts(std::uint64_t address, std::size_t size);
}
