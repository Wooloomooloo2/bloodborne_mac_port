// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// pkg_extract: unpacks a fake-signed PS4 package (fpkg) into <output>/<TITLE_ID>/,
// the layout bbport expects (eboot.bin, sce_sys/, sce_module/, dvdroot_ps4/ ...).
// Built from shadPS4's PKG/PFS code as it was before its removal (commit be22674f~1).

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "core/file_format/pkg.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <file.pkg> <output-dir>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path pkg_path = argv[1];
    const std::filesystem::path out_dir = argv[2];
    std::filesystem::create_directories(out_dir);

    PKG pkg;
    std::string fail;
    if (!pkg.Open(pkg_path, fail)) {
        std::fprintf(stderr, "open failed: %s\n", fail.c_str());
        return 1;
    }
    const std::string title{pkg.GetTitleID()};
    std::printf("Title %s, flags %s, %.1f GiB\n", title.c_str(), pkg.GetPkgFlags().c_str(),
                pkg.GetPkgSize() / 1073741824.0);

    const auto extract = out_dir / title;
    std::filesystem::create_directories(extract / "sce_sys");
    if (!pkg.Extract(pkg_path, extract, fail)) {
        std::fprintf(stderr, "extract failed: %s\n", fail.c_str());
        return 1;
    }

    const u32 count = pkg.GetNumberOfFiles();
    std::atomic<u32> next{0}, done{0};
    std::vector<std::thread> workers;
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned w = 0; w < n; ++w)
        workers.emplace_back([&] {
            for (u32 i; (i = next++) < count;) {
                pkg.ExtractFiles(static_cast<int>(i));
                const u32 d = ++done;
                if (d % 256 == 0 || d == count) std::printf("\r%u / %u entries", d, count), std::fflush(stdout);
            }
        });
    for (auto& t : workers) t.join();
    std::printf("\nExtracted to %s\n", extract.c_str());
    return 0;
}
