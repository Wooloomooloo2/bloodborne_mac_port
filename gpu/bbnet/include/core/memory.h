// bbport co-op: the one MemoryManager call the NP libraries make (NpSignaling dumps the
// guest memory around an invalid activation for its log). Guest memory lives at identical
// host addresses; the runtime knows what is mapped (BbNetHost::memory_is_mapped).
#pragma once
#include "common/types.h"

namespace Core {
class MemoryManager {
public:
    static MemoryManager* Instance() {
        static MemoryManager instance;
        return &instance;
    }
    bool IsValidMapping(VAddr virtual_addr, u64 size);
};
using Memory = MemoryManager;
} // namespace Core
