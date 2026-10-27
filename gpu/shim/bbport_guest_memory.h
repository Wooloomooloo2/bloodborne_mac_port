// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "bbport_threads.h"

namespace BbGuest {

// Callers reject low/null table bases before entering this helper. An enclosing
// recovery owns faults (draw-preparation workers must abandon the prepared draw).
// Without one, contain recovery within the copy so a GPU-thread fault cannot jump
// across the caller's C++ objects. Discard partial reads and retry next time.
inline bool Read(void* destination, uintptr_t address, size_t size) noexcept {
    if (!address) {
        std::memset(destination, 0, size);
        return false;
    }
    if (runtime_fault_recover) {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    }
    BbRecoverBuf recover;
    if (BB_RECOVER_SET(recover)) {
        runtime_fault_recover = nullptr;
        std::memset(destination, 0, size);
        return false;
    }
    runtime_fault_recover = &recover;
    std::memcpy(destination, reinterpret_cast<const void*>(address), size);
    runtime_fault_recover = nullptr;
    return true;
}

} // namespace BbGuest
