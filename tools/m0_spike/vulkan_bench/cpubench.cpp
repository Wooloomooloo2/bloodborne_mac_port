// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// M0(c) companion: CPU-only micro-benchmark of renderer-style C++ (no Vulkan, no MoltenVK).
// Mimics what the shadPS4 renderer does per draw: hash-map cache lookups keyed by hashed
// state, small vectors of bindings, virtual dispatch, atomics/refcounts, short sorts and
// memcpy of descriptor-ish structs. Built x86-64 (Rosetta) and arm64 from this source.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

static uint64_t rng = 0x243F6A8885A308D3ull;
static inline uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static inline uint64_t hash64(const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

struct Resource {
    virtual ~Resource() = default;
    virtual uint64_t Bind(uint64_t slot) = 0;
    std::atomic<uint32_t> refs{1};
    uint64_t gpu_addr = 0;
};
struct BufferRes : Resource { uint64_t size = 256; uint64_t Bind(uint64_t s) override { return gpu_addr + s * size; } };
struct ImageRes : Resource { uint32_t w = 64, h = 64; uint64_t Bind(uint64_t s) override { return gpu_addr ^ (s * w + h); } };
struct SamplerRes : Resource { uint32_t mode = 3; uint64_t Bind(uint64_t s) override { return (gpu_addr + s) * mode; } };

struct PipelineKey {
    uint32_t vs_hash, fs_hash, blend, depth, formats[6];
    bool operator==(const PipelineKey &o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
struct KeyHash { size_t operator()(const PipelineKey &k) const { return hash64(&k, sizeof(k)); } };

struct Binding { uint32_t type, slot; uint64_t addr, size; };

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 2000000;
    int reps = argc > 2 ? atoi(argv[2]) : 5;

    // caches as in a renderer: pipeline cache, texture cache by guest address, buffer cache
    std::unordered_map<PipelineKey, uint32_t, KeyHash> pipe_cache;
    std::unordered_map<uint64_t, std::unique_ptr<Resource>> tex_cache, buf_cache;
    std::vector<PipelineKey> keys;
    for (int i = 0; i < 400; i++) {
        PipelineKey k{};
        k.vs_hash = (uint32_t)rnd(); k.fs_hash = (uint32_t)rnd(); k.blend = rnd() % 6; k.depth = rnd() % 4;
        for (auto &f : k.formats) f = rnd() % 40;
        keys.push_back(k);
        pipe_cache.emplace(k, i);
    }
    std::vector<uint64_t> tex_addrs, buf_addrs;
    for (int i = 0; i < 3000; i++) {
        uint64_t a = (rnd() & 0xFFFFFFF000ull);
        tex_addrs.push_back(a);
        auto r = (i % 7 == 0) ? std::unique_ptr<Resource>(new SamplerRes) : std::unique_ptr<Resource>(new ImageRes);
        r->gpu_addr = a;
        tex_cache.emplace(a, std::move(r));
    }
    for (int i = 0; i < 2000; i++) {
        uint64_t a = (rnd() & 0xFFFFFFFF00ull);
        buf_addrs.push_back(a);
        auto r = std::unique_ptr<Resource>(new BufferRes);
        r->gpu_addr = a;
        buf_cache.emplace(a, std::move(r));
    }
    std::mutex mtx;
    std::atomic<uint64_t> tick{0};
    std::string name_buf;

    double best = 1e30, sum = 0;
    uint64_t checksum = 0;
    for (int rep = 0; rep < reps; rep++) {
        rng = 0x9E3779B97F4A7C15ull;
        auto t0 = std::chrono::steady_clock::now();
        uint64_t acc = 0;
        std::vector<Binding> bindings;
        for (int it = 0; it < iters; it++) {
            // 1) pipeline lookup by hashed key (occasionally a miss -> key tweak)
            PipelineKey k = keys[rnd() % keys.size()];
            if ((it & 63) == 0) k.depth ^= 8;
            auto pit = pipe_cache.find(k);
            acc += pit != pipe_cache.end() ? pit->second : 7;
            // 2) gather bindings into a small vector (reused, like a per-draw scratch)
            bindings.clear();
            int nb = 3 + (int)(rnd() % 6);
            for (int b = 0; b < nb; b++) {
                bool tex = (b & 1);
                uint64_t addr = tex ? tex_addrs[rnd() % tex_addrs.size()] : buf_addrs[rnd() % buf_addrs.size()];
                auto &cache = tex ? tex_cache : buf_cache;
                auto rit = cache.find(addr);
                Resource *r = rit->second.get();
                r->refs.fetch_add(1, std::memory_order_acq_rel);   // refcount like shared handles
                bindings.push_back({(uint32_t)tex, (uint32_t)b, r->Bind(b), 256});  // virtual call
                r->refs.fetch_sub(1, std::memory_order_acq_rel);
            }
            // 3) sort / dedupe bindings, hash the result (descriptor-set-ish)
            std::sort(bindings.begin(), bindings.end(),
                      [](const Binding &a, const Binding &b) { return a.addr < b.addr; });
            acc ^= hash64(bindings.data(), bindings.size() * sizeof(Binding));
            // 4) shared counters + occasional lock (stats, page tracking)
            tick.fetch_add(1, std::memory_order_relaxed);
            if ((it & 255) == 0) { std::lock_guard<std::mutex> lg(mtx); acc += tick.load(); }
            // 5) occasional small string work (debug names / logging formatting)
            if ((it & 1023) == 0) { name_buf = "draw#" + std::to_string(it); acc += name_buf.size(); }
        }
        auto t1 = std::chrono::steady_clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
        best = std::min(best, ns);
        sum += ns;
        checksum ^= acc;
    }
#if defined(__x86_64__)
    const char *arch = "x86_64";
#else
    const char *arch = "arm64";
#endif
    printf("CPURESULT arch=%s iters=%d reps=%d best_ns_per_iter=%.1f mean_ns_per_iter=%.1f checksum=%016llx\n", arch,
           iters, reps, best, sum / reps, (unsigned long long)checksum);
    return 0;
}
