// SPDX-License-Identifier: GPL-3.0-or-later
// Summoning the same player again after a session ended badly (the host lost its network):
// the peer table's hole punches. No game, account, router or network is used.
#include "httplib.h"
#include "thd/thd_internal.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace std::chrono_literals;

static std::atomic<int> g_punches{0};

namespace BbEngine {
bool host_read_safe(const void*, void*, std::size_t) { return false; }
}
extern "C" int32_t runtime_thread_spawn(void**, void* PS4_SYSV_ABI (*)(void*), void*, uint64_t, const char*) {
    std::abort(); // dispatching guest callbacks is outside this fixture
}
namespace BbThd {
std::string DetectLanAddress(const std::string&) { return "127.0.0.1"; }
bool P2pStun(const char*, std::uint16_t, int, std::uint32_t*, std::uint16_t*, Relay*) { return false; }
void P2pPunch(const char*, std::uint32_t, std::uint16_t, std::uint32_t, std::uint16_t) { ++g_punches; }
void RegisterNet(Core::Loader::SymbolsResolver*) {}
void RegisterMatching2(Core::Loader::SymbolsResolver*) {}
void RegisterNp(Core::Loader::SymbolsResolver*) {}
}

static BbThd::Json HostRecord(const char* addr) {
    return {{"MemberId", 1}, {"OnlineId", "host"}, {"Addr", addr}, {"Port", 3658}};
}

int main() {
    setenv("BB_THD_SERVER", "http://127.0.0.1:9", 1); // never contacted
    setenv("BB_THD_AUTH", "http://127.0.0.1:9", 1);
    setenv("BB_THD_STUN", "off", 1);
    setenv("BB_THD_ONLINE_ID", "fixture", 1);
    setenv("BB_THD_TOKEN", "fixture-token", 1);

    // The guest: every invite seeds the host as member 1. The first one punches; one with the
    // same address punches again once the last probes are old enough, and not before.
    BbThd::PeersFromMembers(BbThd::Json::array({HostRecord("203.0.113.5")}));
    assert(g_punches == 1);
    BbThd::PeersFromMembers(BbThd::Json::array({HostRecord("203.0.113.5")}));
    BbThd::PeersPunchAgain(1, 60000);
    assert(g_punches == 1);
    std::this_thread::sleep_for(20ms);
    BbThd::PeersPunchAgain(1, 10);
    assert(g_punches == 2);
    BbThd::PeersPunchAgain(1, 60000); // those probes count as the last ones
    assert(g_punches == 2);
    BbThd::PeersFromMembers(BbThd::Json::array({HostRecord("203.0.113.6")})); // a new address
    assert(g_punches == 3);
    BbThd::PeersPunchAgain(1, 60000);
    assert(g_punches == 3);
    BbThd::PeersPunchAgain(2, 0); // nobody
    assert(g_punches == 3);
    BbThd::PeersClear();

    return 0;
}
