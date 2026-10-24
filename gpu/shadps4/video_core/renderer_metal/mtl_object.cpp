// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Metal::Object reference counting through CoreFoundation (plain C++).

#include <CoreFoundation/CoreFoundation.h>
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {

Object Object::Retain(void* object) {
    if (object) {
        CFRetain(object);
    }
    return Adopt(object);
}

void Object::Reset() {
    if (ptr) {
        CFRelease(ptr);
        ptr = nullptr;
    }
}

} // namespace Metal
