// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal stand-in for shadPS4's Common::FS::IOFile, covering what pkg.cpp uses.

#pragma once

#include <cstdio>
#include <filesystem>
#include <type_traits>
#include <vector>
#include "common/types.h"

namespace Common::FS {

enum class FileAccessMode { Read, Write };
enum class SeekOrigin { SetOrigin, CurrentPosition, End };

class IOFile {
public:
    IOFile() = default;
    IOFile(const std::filesystem::path& path, FileAccessMode mode) { Open(path, mode); }
    ~IOFile() { Close(); }
    IOFile(const IOFile&) = delete;
    IOFile& operator=(const IOFile&) = delete;

    bool Open(const std::filesystem::path& path, FileAccessMode mode) {
        Close();
        file = std::fopen(path.c_str(), mode == FileAccessMode::Read ? "rb" : "wb");
        if (!file) std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return file != nullptr;
    }
    void Close() {
        if (file) std::fclose(file);
        file = nullptr;
    }
    bool IsOpen() const { return file != nullptr; }

    u64 GetSize() const {
        const auto pos = ftello(file);
        fseeko(file, 0, SEEK_END);
        const auto size = ftello(file);
        fseeko(file, pos, SEEK_SET);
        return static_cast<u64>(size);
    }
    bool Seek(s64 offset, SeekOrigin origin = SeekOrigin::SetOrigin) const {
        const int whence = origin == SeekOrigin::SetOrigin       ? SEEK_SET
                           : origin == SeekOrigin::CurrentPosition ? SEEK_CUR
                                                                   : SEEK_END;
        return file && fseeko(file, offset, whence) == 0;
    }
    s64 Tell() const { return ftello(file); }

    template <typename T>
    size_t ReadRaw(void* data, size_t count) const {
        return std::fread(data, sizeof(T), count, file);
    }
    template <typename T>
    size_t WriteRaw(const void* data, size_t count) const {
        return std::fwrite(data, sizeof(T), count, file);
    }
    template <typename T>
    size_t Read(std::vector<T>& data) const {
        return ReadRaw<T>(data.data(), data.size());
    }
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    size_t Read(T& data) const {
        return ReadRaw<u8>(&data, sizeof(T));
    }
    template <typename T>
    size_t Write(const std::vector<T>& data) const {
        return WriteRaw<T>(data.data(), data.size());
    }

private:
    std::FILE* file = nullptr;
};

} // namespace Common::FS
