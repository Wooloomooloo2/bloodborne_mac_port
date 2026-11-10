// SPDX-License-Identifier: GPL-3.0-or-later
// Summoning the same player again after a session ended badly (the host lost its network):
// the peer table's hole punches. No game, account, router or network is used.
#include "httplib.h"
#include "thd/thd_internal.h"
#include "core/libraries/libs.h"

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
extern "C" int32_t runtime_thread_spawn(void**, void* PS4_SYSV_ABI (*entry)(void*), void* arg, uint64_t, const char*) {
    std::thread([entry, arg] { entry(arg); }).detach();
    return 0;
}
namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char*, const char*, const char*, SymbolType, u64) {}
}
namespace BbThd {
std::string DetectLanAddress(const std::string&) { return "127.0.0.1"; }
bool P2pStun(const char*, std::uint16_t, int, std::uint32_t*, std::uint16_t*, Relay*) { return false; }
void P2pPunch(const char*, std::uint32_t, std::uint16_t, std::uint32_t, std::uint16_t) { ++g_punches; }
void P2pSignal(std::uint32_t, std::uint16_t, const void*, std::size_t) {}
void RegisterNet(Core::Loader::SymbolsResolver*) {}
void RegisterMatching2(Core::Loader::SymbolsResolver*) {}
}

static BbThd::Json HostRecord(const char* addr) {
    return {{"MemberId", 1}, {"OnlineId", "host"}, {"Addr", addr}, {"Port", 3658}};
}

int main() {
    httplib::Server server;
    std::atomic<int> resolve_requests{0};
    std::atomic<bool> resolve_succeed{false};
    server.Post("/np/signaling/resolve", [&](const auto&, auto& res) {
        ++resolve_requests;
        if (!resolve_succeed) {
            res.set_content("{\"ResKind\":1}", "application/json");
        } else {
            res.set_content("{\"ResKind\":0,\"Addr\":\"203.0.113.7\",\"Port\":3658}", "application/json");
        }
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    assert(port > 0);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    setenv("BB_THD_SERVER", base.c_str(), 1);
    setenv("BB_THD_AUTH", base.c_str(), 1);
    setenv("BB_THD_STUN", "off", 1);
    setenv("BB_THD_ONLINE_ID", "fixture", 1);
    setenv("BB_THD_TOKEN", "fixture-token", 1);
    std::thread serving([&] { server.listen_after_bind(); });
    while (!server.is_running()) std::this_thread::sleep_for(2ms);

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

    // The host: a signaling connection activated while the server could not resolve the peer
    // re-resolves once old enough, and transitions to established/active.
    unsigned a = 0;
    int rc = BbThd::SignalingActivate(1, "guest", &a);
    assert(rc == 0);
    assert(a != 0);
    assert(resolve_requests == 1);
    BbThd::Peer p;
    assert(!BbThd::PeersFindOnline("guest", &p));

    resolve_succeed = true;

    unsigned b = 0;
    rc = BbThd::SignalingActivate(1, "guest", &b);
    assert(rc == 0);
    assert(b == a);
    assert(resolve_requests == 1);

    std::this_thread::sleep_for(1100ms);

    unsigned c = 0;
    rc = BbThd::SignalingActivate(1, "guest", &c);
    assert(rc == 0);
    assert(c == a);
    assert(resolve_requests == 2);
    assert(BbThd::PeersFindOnline("guest", &p));
    assert(p.conn_id == a);
    assert(p.sig_status == 2);

    unsigned d = 0;
    rc = BbThd::SignalingActivate(1, "guest", &d);
    assert(rc == 0);
    assert(d == a);
    assert(resolve_requests == 2);

    server.stop();
    serving.join();

    return 0;
}
