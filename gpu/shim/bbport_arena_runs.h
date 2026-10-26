// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <vector>

// bbport: the GPU-visible runs of guest memory, read from the protection the game maps with
// (the view table's prot, GPU read 0x10 / write 0x20). The arena mirrors guest memory one to
// one from 0x7000000000 and the driver wires it by its full length, but only ~3.3 of its 6 GiB
// are mapped for the GPU (the rest is CPU-only memory or nothing). This is telemetry only: it
// records the runs (printed when they change) and classifies each request that reaches the
// arena by where it lies against them, so a layout with one buffer per run can be judged
// before it exists (performance/github/MEMORY_FIX_RECOMMENDATION.md, stage S0).
//
// Threads: the view hooks run on the host's ring thread, in ring order, after the view table
// is updated; Refresh runs there too, before each record that is not a view. Requests are
// classified on any thread against an immutable table published with release/acquire.
namespace BbArenaRuns {

enum class Class : std::uint8_t {
    InRun,           ///< inside one GPU run
    StraddlesRunEnd, ///< starts in a run, ends past it in CPU-only memory or nothing
    CrossesGap,      ///< starts in a run and reaches the next one across a gap
    OutsideCpuOnly,  ///< starts in guest memory mapped for the CPU only
    OutsideUnmapped, ///< starts where nothing is mapped
    Size0,           ///< zero bytes
    NoLayout,        ///< before the first table
    Count,
};

enum class Caller : std::uint8_t {
    Bind,           ///< ObtainBuffer: shader buffers, vertex buffers and the rest
    Index,          ///< ObtainIndexBuffer
    Indirect,       ///< indirect draw and dispatch arguments, and their counts
    Fill,           ///< Rasterizer::FillBuffer on the GPU
    Copy,           ///< Rasterizer::CopyBuffer (CP DMA) on the GPU
    Hle,            ///< the HLE copy shader
    Image,          ///< ObtainBufferForImage of GPU-modified memory
    ReadBack,       ///< BufferCache::ReadMemory's download
    DirectReadBack, ///< the fault thread's direct read-back
    Count,
};

/// The arena's guest range (BufferCache), for the offsets the run table prints.
void SetArena(std::uint64_t base, std::uint64_t end);

/// View hooks (ring thread): a map, an unmap, a protection change of [address, address + size).
void OnMap(std::uint64_t address, std::uint64_t size);
void OnUnmap(std::uint64_t address, std::uint64_t size);
void OnProtect(std::uint64_t address, std::uint64_t size);

/// Ring thread, before each record that is not a view: rebuilds and publishes the table when a
/// view event changed the GPU runs, and from the first call on reports later changes as late.
void Refresh();

/// Classifies a request and counts it per caller and class; the first examples of each class
/// other than InRun are printed. Requests in a run that end more than 4 GiB past its start are
/// counted too (32-bit Metal offsets would still need a copy with one buffer per run).
Class Note(Caller caller, std::uint64_t address, std::uint64_t size);

/// BB_FRAME_STATS: one line of cumulative counts (driver.cpp, every 5 s).
void PrintStats();

/// A GPU run: guest memory [start, end) mapped with a GPU protection.
struct Range {
    std::uint64_t start, end;
};

/// The current runs, sorted (empty before the first table).
std::vector<Range> Runs();

/// Called on the ring thread (from Refresh) after the runs changed, before the record that
/// follows the view events is dispatched (the buffer cache creates its chunks from it). Null clears.
void SetListener(void (*listener)(void* context), void* context);

} // namespace BbArenaRuns
