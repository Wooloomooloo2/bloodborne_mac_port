// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the GPU library split across two processes (docs/DESIGN_NATIVE_GPU_PROCESS.md, M3).
// The same library runs in both: in bb-probe ("remote": the game's side, no renderer) and in
// bbgpu-host ("host": the renderer). Single-process mode is the default and changes nothing.
// Records and the shared control block: host/bbgpu_shared.h.
#pragma once

#include "common/types.h"
#include "host/bbgpu_shared.h"

namespace BbSplit {

enum class Mode { Single, Remote, Host };
Mode GetMode();
inline bool Remote() {
    return GetMode() == Mode::Remote;
}
inline bool Host() {
    return GetMode() == Mode::Host;
}
BbGpuShared* Shared();

/// Game side, before the renderer starts: pushes (the view mirror) work, the mode is unchanged.
void Attach(BbGpuShared* shared);
void InitRemote(BbGpuShared* shared);
void InitHost(BbGpuShared* shared);

// ---- game side ----
/// Pushes a record (record->bytes long) onto the submit ring, in order with every other push.
void Push(const void* record);
/// Sends a request record whose u32 reply-slot field is at `slot`, then waits for the host's
/// answer (every record pushed before it has been handled by then).
s64 Request(void* record, u32* slot);
/// The SubmitDone count the host's latest forwarded GpuIdle follows (see ResetSubmissionLock).
u64 GpuIdleSeq();
/// M4: a fault on a GPU-protected page, handled by the host (which unprotects before it
/// answers). False for addresses outside the mirrored guest range or not GPU memory.
bool RelayFault(u64 address, bool write);
/// Starts the thread that applies the host's events (IRQs, flips, vblanks).
void StartEventDispatcher();

// ---- host side ----
/// Pushes an event record onto the event ring (any host thread).
void Post(const void* record);
/// Forwards every GPU interrupt the host raises to the game (after the renderer started).
void ForwardInterrupts();

} // namespace BbSplit
