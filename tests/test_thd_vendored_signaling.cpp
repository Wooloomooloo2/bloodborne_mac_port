// SPDX-License-Identifier: GPL-3.0-or-later
// The vendored bbhost NpSignaling (gpu/bbnet/vendor/bbhost/np_signaling.cpp) on this backend's
// glue (thd_bbhost_glue.cpp): the v0.2.31 handshake with a bbhost peer, resends, strangers and
// short packets, and a connection the server could not resolve at first (the host's half of the
// rejoin bug). No game, account, router or network is used: sent datagrams are captured, and the
// session service is a loopback mock.
#include "httplib.h"
#include "thd/thd_internal.h"
#include "core/libraries/libs.h"
#include "../vendor/bbhost_shim/hle/np.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct Sent {
    std::uint8_t kind;
    std::string from, to;
    std::uint64_t nonce;
    std::uint32_t addr;
    std::uint16_t port;
};
struct Event {
    std::int64_t ctx, conn, event, error;
};
std::mutex g_mu;
std::vector<Sent> g_sent;
std::vector<Event> g_events;
std::map<std::string, u64> g_nids;
std::vector<std::string> g_transport_up;

std::string id_at(const std::uint8_t* d) {
    std::string s(reinterpret_cast<const char*>(d), 16);
    s.resize(std::strlen(s.c_str()));
    return s;
}

std::int64_t PS4_SYSV_ABI on_event(std::int64_t ctx, std::int64_t conn, std::int64_t event, std::int64_t error, void*) {
    std::lock_guard lk(g_mu);
    g_events.push_back({ctx, conn, event, error});
    return 0;
}

constexpr std::uint8_t kActivationRequest = 0x01, kActivationAck = 0x02, kClose = 0x03, kEstablished = 0x04,
                       kOffer = 0x11, kAccept = 0x12, kCheck = 0x13, kCheckAck = 0x14;
constexpr std::uint16_t kEvDead = 0x00, kEvEstablished = 0x01, kEvPeerActivated = 0x0a;
constexpr std::uint32_t kRelay = 0x0700710bu; // 11.113.0.7, network order
constexpr std::uint16_t kRelayPort = 7919;

std::vector<std::uint8_t> packet(std::uint8_t kind, std::uint8_t reason, const char* from, const char* to,
                                 std::uint64_t nonce = 0) {
    std::vector<std::uint8_t> p(46, 0);
    const std::uint8_t magic[4] = {0xfe, 'b', 'b', 's'};
    std::memcpy(p.data(), magic, 4);
    p[4] = kind;
    p[5] = reason;
    std::memcpy(p.data() + 6, from, std::strlen(from));
    std::memcpy(p.data() + 22, to, std::strlen(to));
    std::memcpy(p.data() + 38, &nonce, 8);
    return p;
}
void deliver(const std::vector<std::uint8_t>& p, std::size_t len = 46) {
    // As thd_net.cpp hands them over: the sender's port in network order.
    hle_np_signaling_control(p.data(), len, kRelay, static_cast<std::uint16_t>((kRelayPort << 8) | (kRelayPort >> 8)));
}

std::vector<Sent> take_sent() {
    std::lock_guard lk(g_mu);
    auto out = g_sent;
    g_sent.clear();
    return out;
}
std::vector<Event> take_events() {
    std::lock_guard lk(g_mu);
    auto out = g_events;
    g_events.clear();
    return out;
}
std::size_t count_kind(const std::vector<Sent>& sent, std::uint8_t kind, const char* to) {
    std::size_t n = 0;
    for (const auto& s : sent) n += s.kind == kind && s.to == to;
    return n;
}
bool has_event(const std::vector<Event>& ev, std::int64_t conn, std::uint16_t event) {
    for (const auto& e : ev) {
        if (e.conn == conn && e.event == event) return true;
    }
    return false;
}

// The game's SceNpId: the online id, NUL-padded to 16 bytes, then unused bytes.
struct NpId {
    char name[16]{};
    std::uint8_t rest[20]{};
};
NpId npid(const char* name) {
    NpId id;
    std::strncpy(id.name, name, sizeof(id.name));
    return id;
}

// The libSceNpSignaling imports as the game reaches them, by NID.
template <typename F> F import(const char* nid) {
    const auto it = g_nids.find(nid);
    assert(it != g_nids.end());
    return reinterpret_cast<F>(it->second);
}
using CreateFn = int(PS4_SYSV_ABI*)(const void*, void*, void*, unsigned*);
using ActivateFn = int(PS4_SYSV_ABI*)(unsigned, const void*, unsigned*);
using DeactivateFn = int(PS4_SYSV_ABI*)(unsigned, unsigned);

template <typename P> bool wait_for(P pred, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}
} // namespace

namespace BbEngine {
bool host_read_safe(const void*, void*, std::size_t) { return false; }
}
extern "C" int32_t runtime_thread_spawn(void**, void* PS4_SYSV_ABI (*entry)(void*), void* arg, uint64_t, const char*) {
    std::thread([entry, arg] { entry(arg); }).detach();
    return 0;
}
namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char*, const char*, SymbolType, u64 address) {
    g_nids[nid] = address;
}
}
void hle_np_matching2_transport_up(const std::string& online_id) {
    std::lock_guard lk(g_mu);
    g_transport_up.push_back(online_id);
}
void hle_np_matching2_transport_dead(const std::string&, int) {}
void hle_np_matching2_peer_closed(const std::string&) {}
namespace BbThd {
std::string DetectLanAddress(const std::string&) { return "127.0.0.1"; }
bool P2pStun(const char*, std::uint16_t, int, std::uint32_t*, std::uint16_t*, Relay*) { return false; }
void P2pPunch(const char*, std::uint32_t, std::uint16_t, std::uint32_t, std::uint16_t) {}
void P2pSignalSend(std::uint32_t addr, std::uint16_t port, const void* data, std::size_t len) {
    assert(len == 46);
    const auto* d = static_cast<const std::uint8_t*>(data);
    assert(std::memcmp(d, "\xfe" "bbs", 4) == 0);
    Sent s{d[4], id_at(d + 6), id_at(d + 22), 0, addr, port};
    std::memcpy(&s.nonce, d + 38, 8);
    std::lock_guard lk(g_mu);
    g_sent.push_back(s);
}
void RegisterNet(Core::Loader::SymbolsResolver*) {}
void RegisterMatching2(Core::Loader::SymbolsResolver*) {}
void RegisterNp(Core::Loader::SymbolsResolver*) {}
}

int main() {
    // The session service: signaling/resolve fails until the test lets it succeed.
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

    Core::Loader::SymbolsResolver resolver;
    BbThd::RegisterVendoredSignaling(&resolver);
    const auto create = import<CreateFn>("5yYjEdd4t8Y");
    const auto activate = import<ActivateFn>("0UvTFeomAUM");
    const auto deactivate = import<DeactivateFn>("6UEembipgrM");

    const NpId me = npid("fixture");
    unsigned ctx = 0;
    assert(create(&me, reinterpret_cast<void*>(&on_event), nullptr, &ctx) == 0 && ctx);

    // Too short, an Offer from a player we know nothing about, or no sender: no answer.
    // (bbhost answers a request whatever receiver id it names: not checked here.)
    deliver(packet(kActivationRequest, 1, "invader", "fixture"), 20);
    deliver(packet(kOffer, 1, "stranger", "fixture"));
    deliver(packet(kActivationRequest, 1, "", "fixture"));
    std::this_thread::sleep_for(100ms);
    assert(take_sent().empty());
    assert(take_events().empty());

    // A bbhost invader's game activates its connection to us: ActivationAck back to where it
    // came from (the relay); its handshake: Offer -> Accept, Check -> CheckAck with the same
    // nonce. PEER_ACTIVATED tells our game once the transport is up.
    deliver(packet(kActivationRequest, 1, "invader", "fixture"));
    auto sent = take_sent();
    assert(count_kind(sent, kActivationAck, "invader") == 1);
    for (const auto& s : sent) assert(s.from == "fixture" && s.addr == kRelay && s.port == kRelayPort);
    deliver(packet(kOffer, 1, "invader", "fixture"));
    assert(count_kind(take_sent(), kAccept, "invader") == 1);
    deliver(packet(kCheck, 1, "invader", "fixture", 0x1122334455667788ull));
    sent = take_sent();
    assert(count_kind(sent, kCheckAck, "invader") == 1);
    for (const auto& s : sent) {
        if (s.kind == kCheckAck) assert(s.nonce == 0x1122334455667788ull);
    }
    // bbhost's transport is up once both directions checked: our Offer (resent while stepping)
    // is accepted, our Check is acknowledged.
    deliver(packet(kAccept, 1, "invader", "fixture"));
    assert(wait_for([&] {
        std::lock_guard lk(g_mu);
        for (const auto& s : g_sent) {
            if (s.kind == kCheck && s.to == "invader") return true;
        }
        return false;
    }, 2s)); // at once, or with the next 500 ms step
    take_sent();
    std::vector<Event> ev;
    assert(take_events().empty()); // not up yet
    deliver(packet(kCheckAck, 1, "invader", "fixture"));
    assert(wait_for([&] {
        auto more = take_events();
        ev.insert(ev.end(), more.begin(), more.end());
        for (const auto& e : ev) {
            if (e.event == kEvPeerActivated) return true;
        }
        return false;
    }, 2s));
    std::int64_t conn = 0;
    for (const auto& e : ev) {
        if (e.event == kEvPeerActivated) {
            assert(e.ctx == ctx && e.conn != 0 && !conn);
            conn = e.conn;
        }
    }

    // Resent steps are answered again (a CheckAck carries the new nonce), PEER_ACTIVATED goes
    // out once.
    deliver(packet(kCheck, 1, "invader", "fixture", 7));
    sent = take_sent();
    assert(count_kind(sent, kCheckAck, "invader") == 1);
    for (const auto& s : sent) {
        if (s.kind == kCheckAck) assert(s.nonce == 7);
    }
    deliver(packet(kEstablished, 1, "invader", "fixture"));
    std::this_thread::sleep_for(300ms);
    take_sent();
    for (const auto& e : take_events()) assert(e.event != kEvPeerActivated);

    // The game answers with its own activation: the same connection, ESTABLISHED, and our
    // ActivationRequest to the invader (bbhost's game waits for MUTUAL_ACTIVATED, which needs it).
    const NpId invader = npid("invader");
    unsigned mine = 0;
    assert(activate(ctx, &invader, &mine) == 0 && mine == conn);
    assert(wait_for([&] { return has_event(take_events(), conn, kEvEstablished); }, 2s));
    assert(count_kind(take_sent(), kActivationRequest, "invader") == 1);
    assert(wait_for([&] {
        std::lock_guard lk(g_mu);
        for (const auto& id : g_transport_up) {
            if (id == "invader") return true;
        }
        return false;
    }, 3s));

    // Its transport closes: DEAD to our game.
    deliver(packet(kClose, 2, "invader", "fixture"));
    assert(wait_for([&] { return has_event(take_events(), conn, kEvDead); }, 2s));
    deactivate(ctx, mine);
    std::this_thread::sleep_for(100ms);
    take_sent();
    take_events();

    // The host's half of the rejoin bug: our game activates a connection to a guest the server
    // cannot resolve yet (the host's network was down). It must not stay stuck: the vendored code
    // asks the server again every 500 ms, and once it answers the handshake starts.
    const NpId guest = npid("guest");
    unsigned g = 0;
    assert(activate(ctx, &guest, &g) == 0 && g);
    assert(wait_for([&] { return resolve_requests >= 2; }, 3s)); // retried, not given up
    assert(count_kind(take_sent(), kActivationRequest, "guest") == 0);
    resolve_succeed = true;
    assert(wait_for([&] {
        std::lock_guard lk(g_mu);
        for (const auto& s : g_sent) {
            if (s.kind == kActivationRequest && s.to == "guest") return true;
        }
        return false;
    }, 3s));
    BbThd::Peer p;
    assert(BbThd::PeersFindOnline("guest", &p) && p.port == 3658);
    sent = take_sent();
    for (const auto& s : sent) {
        if (s.to == "guest") assert(s.port == 3658);
    }

    // Our game deactivates it before the guest answered: DEAD to our game, nothing left live.
    deactivate(ctx, g);
    assert(wait_for([&] { return has_event(take_events(), g, kEvDead); }, 2s));

    server.stop();
    serving.join();
    std::_Exit(0); // the dispatcher's threads are still parked
}
