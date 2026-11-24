// SPDX-License-Identifier: GPL-3.0-or-later
// Inbound signaling on the P2P port: drop short/oversized datagrams and bound how many sit on
// the dispatcher (PR #165 security review). No network: WireOk is pure, and the queue counter is
// exercised directly.
#include "httplib.h"
#include "thd/thd_internal.h"
#include "thd/thd_signal.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace BbEngine {
bool host_read_safe(const void*, void*, std::size_t) { return false; }
}
extern "C" int32_t runtime_thread_spawn(void**, void* PS4_SYSV_ABI (*)(void*), void*, uint64_t, const char*) {
    std::abort(); // dispatching guest callbacks is outside this fixture
}
namespace BbThd {
std::string DetectLanAddress(const std::string&) { return "127.0.0.1"; }
bool P2pStun(const char*, std::uint16_t, int, std::uint32_t*, std::uint16_t*, Relay*) { return false; }
void P2pPunch(const char*, std::uint32_t, std::uint16_t, std::uint32_t, std::uint16_t) {}
void RegisterNet(Core::Loader::SymbolsResolver*) {}
void RegisterMatching2(Core::Loader::SymbolsResolver*) {}
void RegisterNp(Core::Loader::SymbolsResolver*) {}
}

int main() {
    setenv("BB_THD_SERVER", "http://127.0.0.1:9", 1);
    setenv("BB_THD_AUTH", "http://127.0.0.1:9", 1);
    setenv("BB_THD_STUN", "off", 1);

    assert(!BbThd::Signal::WireOk(0));
    assert(!BbThd::Signal::WireOk(4));
    assert(!BbThd::Signal::WireOk(BbThd::Signal::kPacket - 1));
    assert(BbThd::Signal::WireOk(BbThd::Signal::kPacket));
    assert(BbThd::Signal::WireOk(BbThd::Signal::kMaxWire));
    assert(!BbThd::Signal::WireOk(BbThd::Signal::kMaxWire + 1));
    assert(!BbThd::Signal::WireOk(65507));

    // Magic alone is not enough: the reader still needs WireOk.
    std::uint8_t short_sig[8] = {0xfe, 'b', 'b', 's', 0x01, 0, 0, 0};
    assert(BbThd::Signal::IsSignal(short_sig, sizeof(short_sig)));
    assert(!BbThd::Signal::WireOk(sizeof(short_sig)));

    std::uint8_t ok[BbThd::Signal::kPacket] = {};
    ok[0] = 0xfe;
    ok[1] = 'b';
    ok[2] = 'b';
    ok[3] = 's';
    assert(BbThd::Signal::IsSignal(ok, sizeof(ok)));
    assert(BbThd::Signal::WireOk(sizeof(ok)));

    assert(BbThd::SignalingQueueDepth() == 0);
    for (int i = 0; i < BbThd::Signal::kMaxQueued; ++i) {
        assert(BbThd::SignalingQueueTry());
        assert(BbThd::SignalingQueueDepth() == i + 1);
    }
    assert(!BbThd::SignalingQueueTry());
    assert(BbThd::SignalingQueueDepth() == BbThd::Signal::kMaxQueued);

    for (int i = 0; i < BbThd::Signal::kMaxQueued; ++i) BbThd::SignalingQueueDone();
    assert(BbThd::SignalingQueueDepth() == 0);
    assert(BbThd::SignalingQueueTry());
    BbThd::SignalingQueueDone();
    assert(BbThd::SignalingQueueDepth() == 0);

    std::puts("PASS: signaling ingress bounds (size + queue cap)");
    return 0;
}
