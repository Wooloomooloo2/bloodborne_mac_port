// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: one-frame Metal GPU capture to a .gputrace file (macOS), for Xcode's frame debugger and
// per-pass GPU timings. BB_GPU_CAPTURE=<seconds> captures the frame presented that long after
// the first present; BB_GPU_CAPTURE_FILE sets the output (default frame.gputrace). Needs
// METAL_CAPTURE_ENABLED=1 in the environment. Captures the renderer's MTLDevice. With the default
// 16 GiB guest arena Apple's capture tool crashes while writing the file (stopCapture); with
// BB_ARENA_GIB=4 it writes it (~0.9 GB in the city).
#pragma once

namespace BbGpuCapture {
#ifdef __APPLE__
/// Call right after each present; `device` is the renderer's id<MTLDevice> (captured: every
/// queue of it, the frame's command buffers and the present).
void OnPresent(void* device);
#else
inline void OnPresent(void*) {}
#endif
} // namespace BbGpuCapture
