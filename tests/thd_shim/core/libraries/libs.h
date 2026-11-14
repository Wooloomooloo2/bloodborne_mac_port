// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/types.h"

namespace Core::Loader {
enum class SymbolType { Function, Object };
class SymbolsResolver {
public:
    void AddSymbol(const char* nid, const char* library, const char* module, SymbolType type, u64 address);
};
} // namespace Core::Loader

#define LIB_FUNCTION(nid, lib, libversion, mod, function) \
    sym->AddSymbol(nid, lib, mod, Core::Loader::SymbolType::Function, \
                   reinterpret_cast<u64>(function))
