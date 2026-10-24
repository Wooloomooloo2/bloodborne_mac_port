// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Metal objects held from plain C++ (Metal-native renderer, spec component 1).

#pragma once

#include <utility>
#include "common/types.h"

namespace Metal {

/// Owns one retained Objective-C object (an `id`) for code that is plain C++. Objective-C++
/// makes one with Object::Adopt((__bridge_retained void*)obj) and reads it back with
/// (__bridge id<...>)object.Get() (unretained). Move-only; the last owner releases it.
class Object {
public:
    Object() = default;
    ~Object() {
        Reset();
    }
    Object(Object&& other) noexcept : ptr{std::exchange(other.ptr, nullptr)} {}
    Object& operator=(Object&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr = std::exchange(other.ptr, nullptr);
        }
        return *this;
    }
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    /// Takes over a +1 reference (from __bridge_retained or a `new…` method).
    static Object Adopt(void* retained) {
        Object object;
        object.ptr = retained;
        return object;
    }
    /// Adds a reference to `object` (an unretained pointer).
    static Object Retain(void* object);

    [[nodiscard]] void* Get() const noexcept {
        return ptr;
    }
    explicit operator bool() const noexcept {
        return ptr != nullptr;
    }
    /// Releases the object (if any).
    void Reset();

private:
    void* ptr{};
};

/// A buffer (id<MTLBuffer>, not retained) and a byte offset into it.
struct BufferRef {
    void* buffer{};
    u64 offset{};
};

} // namespace Metal
