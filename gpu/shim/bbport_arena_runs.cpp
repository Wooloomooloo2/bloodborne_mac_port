// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_arena_runs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

extern "C" {
// The view table (host_runtime.c in the GPU host; runtime_memory.c in one process).
int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace BbArenaRuns {
namespace {

constexpr int GpuProt = 0x30; // ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE
constexpr std::uint64_t MiB = std::uint64_t{1} << 20;
constexpr std::uint64_t FourGiB = std::uint64_t{1} << 32;
constexpr std::uint32_t ExamplesPerClass = 16;
constexpr std::uint32_t MaxTablePrints = 32;
constexpr std::uint32_t MaxLatePrints = 64;

struct Run {
    std::uint64_t start, end;
};

struct Table {
    std::vector<Run> runs;
    std::uint64_t generation = 0;
};

constexpr const char* ClassNames[] = {"in a run",        "straddling a run end",
                                      "across a gap",    "outside, CPU-only memory",
                                      "outside, unmapped", "size 0",
                                      "before the first layout"};
constexpr const char* CallerNames[] = {"bind",  "index", "indirect", "fill",          "copy",
                                       "hle",   "image", "read-back", "direct read-back"};
static_assert(std::size(ClassNames) == std::size_t(Class::Count));
static_assert(std::size(CallerNames) == std::size_t(Caller::Count));

std::atomic<const Table*> current{nullptr};
/// Every table published stays alive (readers never lock); there are a few per session.
std::vector<std::unique_ptr<Table>> tables;
std::atomic<bool> dirty{false};
bool armed = false; ///< ring thread: the first record that is not a view was seen
std::uint32_t table_prints = 0;
std::atomic<std::uint64_t> late_events{0};
std::uint64_t arena_base = 0, arena_end = 0;
bool past_arena_warned = false;
std::atomic<void (*)(void*)> listener{nullptr};
std::atomic<void*> listener_context{nullptr};

std::array<std::array<std::atomic<std::uint64_t>, std::size_t(Class::Count)>,
           std::size_t(Caller::Count)>
    counts{};
std::array<std::atomic<std::uint64_t>, std::size_t(Caller::Count)> over_4g{};
std::array<std::atomic<std::uint32_t>, std::size_t(Class::Count)> examples{};

double Now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int ProtAt(std::uint64_t address, std::uint64_t* end = nullptr) {
    int prot = 0;
    uintptr_t view_end = 0;
    if (!runtime_memory_vma_info(address, &prot, nullptr, &view_end)) {
        return -1;
    }
    if (end) {
        *end = view_end;
    }
    return prot;
}

/// Bytes mapped (any protection) in [start, end).
std::uint64_t MappedBytes(std::uint64_t start, std::uint64_t end) {
    std::uint64_t bytes = 0;
    for (std::uint64_t p = start; p < end;) {
        uintptr_t s = 0, e = 0;
        int mapped = 0;
        if (!runtime_memory_region(p, &s, &e, &mapped) || e <= p) {
            break;
        }
        const std::uint64_t q = std::min<std::uint64_t>(e, end);
        if (mapped) {
            bytes += q - p;
        }
        p = q;
    }
    return bytes;
}

/// The run containing `address`, or the index of the first run after it (negative: -1 - i).
long Find(const Table& t, std::uint64_t address) {
    const auto it = std::upper_bound(t.runs.begin(), t.runs.end(), address,
                                     [](std::uint64_t a, const Run& r) { return a < r.start; });
    if (it != t.runs.begin() && address < std::prev(it)->end) {
        return long(std::prev(it) - t.runs.begin());
    }
    return -1 - long(it - t.runs.begin());
}

bool Overlaps(std::uint64_t address, std::uint64_t size) {
    const Table* t = current.load(std::memory_order_relaxed); // ring thread: its own store
    if (!t) {
        return false;
    }
    const long i = Find(*t, address);
    const std::size_t next = i >= 0 ? std::size_t(i) : std::size_t(-1 - i);
    return i >= 0 || (next < t->runs.size() && t->runs[next].start < address + size);
}

void PrintTable(const Table& t, bool late) {
    std::uint64_t gpu_bytes = 0;
    for (const Run& r : t.runs) {
        gpu_bytes += r.end - r.start;
    }
    std::printf("GPU runs%s: %zu, %.1f MiB of guest memory mapped for the GPU; arena %#llx + %.0f "
                "MiB; t=%.3f\n",
                late ? " (changed while running)" : "", t.runs.size(), gpu_bytes / double(MiB),
                static_cast<unsigned long long>(arena_base),
                (arena_end - arena_base) / double(MiB), Now());
    std::uint64_t previous_end = arena_base;
    for (std::size_t i = 0; i < t.runs.size(); ++i) {
        const Run& r = t.runs[i];
        const bool inside = r.start >= arena_base && r.end <= arena_end;
        std::printf("  run %zu: %#llx-%#llx, %.3f MiB, %s", i + 1,
                    static_cast<unsigned long long>(r.start), static_cast<unsigned long long>(r.end),
                    (r.end - r.start) / double(MiB), inside ? "arena offsets" : "OUTSIDE the arena");
        if (inside) {
            std::printf(" %.3f-%.3f GiB", (r.start - arena_base) / double(1u << 30),
                        (r.end - arena_base) / double(1u << 30));
            if (r.start >= previous_end) {
                std::printf("; %.1f MiB CPU-only and %.1f MiB unmapped before it",
                            MappedBytes(previous_end, r.start) / double(MiB),
                            (r.start - previous_end - MappedBytes(previous_end, r.start)) /
                                double(MiB));
            }
            previous_end = r.end;
        }
        std::printf("\n");
    }
    if (arena_end > previous_end) {
        std::printf("  past the last run: %.1f MiB of the arena (%.1f MiB of it mapped for the CPU)\n",
                    (arena_end - previous_end) / double(MiB),
                    MappedBytes(previous_end, arena_end) / double(MiB));
    }
}

void Rebuild() {
    auto table = std::make_unique<Table>();
    Run run{0, 0};
    for (std::uint64_t p = 0;;) {
        uintptr_t s = 0, e = 0;
        int mapped = 0;
        if (!runtime_memory_region(p, &s, &e, &mapped) || e <= p) {
            break;
        }
        const int prot = mapped ? ProtAt(p) : -1;
        if (prot > 0 && (prot & GpuProt)) {
            if (run.end == p && run.end != 0) {
                run.end = e;
            } else {
                if (run.end != 0) {
                    table->runs.push_back(run);
                }
                run = {p, e};
            }
        }
        p = e;
    }
    if (run.end != 0) {
        table->runs.push_back(run);
    }
    const Table* old = current.load(std::memory_order_relaxed);
    table->generation = old ? old->generation + 1 : 1;
    const bool changed =
        !old || old->runs.size() != table->runs.size() ||
        !std::equal(old->runs.begin(), old->runs.end(), table->runs.begin(),
                    [](const Run& a, const Run& b) { return a.start == b.start && a.end == b.end; });
    if (!changed) {
        return;
    }
    if (table_prints < MaxTablePrints) {
        ++table_prints;
        PrintTable(*table, armed);
    }
    // bbport: GPU memory past the arena's end is reached only to stop the game (GetArena).
    for (const Run& r : table->runs) {
        if (r.end > arena_end && arena_end != 0 && !past_arena_warned) {
            past_arena_warned = true;
            std::printf("GPU runs: guest memory mapped for the GPU at %#llx-%#llx lies past the "
                        "arena (ends at %#llx): a request there stops the game\n",
                        static_cast<unsigned long long>(std::max(r.start, arena_end)),
                        static_cast<unsigned long long>(r.end),
                        static_cast<unsigned long long>(arena_end));
        }
    }
    current.store(table.get(), std::memory_order_release);
    tables.push_back(std::move(table));
    if (auto* fn = listener.load(std::memory_order_acquire)) {
        fn(listener_context.load(std::memory_order_acquire));
    }
}

void NoteViewEvent(const char* what, std::uint64_t address, std::uint64_t size, int prot) {
    dirty.store(true, std::memory_order_relaxed);
    if (!armed) {
        return;
    }
    const std::uint64_t n = late_events.fetch_add(1, std::memory_order_relaxed);
    if (n < MaxLatePrints) {
        std::printf("GPU runs: late change: %s %#llx + %.3f MiB, prot %#x; t=%.3f\n", what,
                    static_cast<unsigned long long>(address), size / double(MiB), prot, Now());
    }
}

} // namespace

void SetArena(std::uint64_t base, std::uint64_t end) {
    arena_base = base;
    arena_end = end;
}

void OnMap(std::uint64_t address, std::uint64_t size) {
    const int prot = ProtAt(address);
    if ((prot > 0 && (prot & GpuProt)) || Overlaps(address, size)) {
        NoteViewEvent("map", address, size, prot);
    }
}

void OnUnmap(std::uint64_t address, std::uint64_t size) {
    if (Overlaps(address, size)) {
        NoteViewEvent("unmap", address, size, -1);
    }
}

void OnProtect(std::uint64_t address, std::uint64_t size) {
    const int prot = ProtAt(address);
    // A protection change that keeps a run's GPU bits changes nothing here; one that adds or
    // removes them does (the whole range has one protection after the event).
    const bool gpu = prot > 0 && (prot & GpuProt);
    const Table* t = current.load(std::memory_order_relaxed);
    bool inside = false;
    if (t) {
        const long i = Find(*t, address);
        inside = i >= 0 && address + size <= t->runs[std::size_t(i)].end;
    }
    if (gpu != inside || (!gpu && Overlaps(address, size))) {
        NoteViewEvent("protect", address, size, prot);
    }
}

void Refresh() {
    if (dirty.load(std::memory_order_relaxed)) {
        dirty.store(false, std::memory_order_relaxed);
        Rebuild();
    }
    armed = true;
}

Class Note(Caller caller, std::uint64_t address, std::uint64_t size) {
    Class c = Class::InRun;
    std::uint64_t run_start = 0;
    const Table* t = current.load(std::memory_order_acquire);
    if (size == 0) {
        c = Class::Size0;
    } else if (!t) {
        c = Class::NoLayout;
    } else if (const long i = Find(*t, address); i >= 0) {
        const Run& r = t->runs[std::size_t(i)];
        run_start = r.start;
        if (address + size > r.end) {
            const std::size_t next = std::size_t(i) + 1;
            c = next < t->runs.size() && t->runs[next].start < address + size
                    ? Class::CrossesGap
                    : Class::StraddlesRunEnd;
        }
    } else {
        c = ProtAt(address) >= 0 ? Class::OutsideCpuOnly : Class::OutsideUnmapped;
    }
    counts[std::size_t(caller)][std::size_t(c)].fetch_add(1, std::memory_order_relaxed);
    if (c == Class::InRun && address - run_start + size > FourGiB) {
        over_4g[std::size_t(caller)].fetch_add(1, std::memory_order_relaxed);
    }
    if (c != Class::InRun &&
        examples[std::size_t(c)].fetch_add(1, std::memory_order_relaxed) < ExamplesPerClass) {
        std::printf("GPU runs: request %s: %s %#llx + %llu bytes; t=%.3f\n", ClassNames[std::size_t(c)],
                    CallerNames[std::size_t(caller)], static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(size), Now());
    }
    return c;
}

std::vector<Range> Runs() {
    std::vector<Range> runs;
    if (const Table* t = current.load(std::memory_order_acquire)) {
        for (const Run& r : t->runs) {
            runs.push_back({r.start, r.end});
        }
    }
    return runs;
}

void SetListener(void (*fn)(void*), void* context) {
    listener_context.store(context, std::memory_order_release);
    listener.store(fn, std::memory_order_release);
}

void PrintStats() {
    std::array<std::uint64_t, std::size_t(Class::Count)> total{};
    for (const auto& per_caller : counts) {
        for (std::size_t c = 0; c < total.size(); ++c) {
            total[c] += per_caller[c].load(std::memory_order_relaxed);
        }
    }
    std::uint64_t over = 0;
    for (const auto& n : over_4g) {
        over += n.load(std::memory_order_relaxed);
    }
    std::printf("Arena requests (since start): %llu in a run (%llu ending over 4 GiB past its "
                "start), %llu straddling a run end, %llu across a gap, outside: %llu CPU-only, "
                "%llu unmapped; %llu of size 0, %llu before the layout; %llu late run changes\n",
                static_cast<unsigned long long>(total[0]), static_cast<unsigned long long>(over),
                static_cast<unsigned long long>(total[1]), static_cast<unsigned long long>(total[2]),
                static_cast<unsigned long long>(total[3]), static_cast<unsigned long long>(total[4]),
                static_cast<unsigned long long>(total[5]), static_cast<unsigned long long>(total[6]),
                static_cast<unsigned long long>(late_events.load(std::memory_order_relaxed)));
    // The misses by caller, when there are any.
    std::uint64_t misses = 0;
    for (std::size_t c = 1; c < total.size(); ++c) {
        misses += total[c];
    }
    if (misses + over == 0) {
        return;
    }
    std::printf("Arena requests by caller (not in a run / over 4 GiB in a run):");
    for (std::size_t k = 0; k < counts.size(); ++k) {
        std::uint64_t miss = 0;
        for (std::size_t c = 1; c < total.size(); ++c) {
            miss += counts[k][c].load(std::memory_order_relaxed);
        }
        const std::uint64_t o = over_4g[k].load(std::memory_order_relaxed);
        if (miss + o != 0) {
            std::printf(" %s %llu/%llu", CallerNames[k], static_cast<unsigned long long>(miss),
                        static_cast<unsigned long long>(o));
        }
    }
    std::printf("\n");
}

} // namespace BbArenaRuns
