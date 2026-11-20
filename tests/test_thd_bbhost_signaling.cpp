// SPDX-License-Identifier: GPL-3.0-or-later
// bbhost v0.2.30+ sets up a connection with its own handshake (kSignalMagic datagrams, bbhost
// src/hle/np_signaling.cpp): our side must answer it, or every bbhost peer times out after 30 s.
// No game, account, router or network is used: sent datagrams are captured.
#include "thd/thd_internal.h"
#include "core/libraries/libs.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
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
void deliver(const std::vector<std::uint8_t>& p) { BbThd::SignalingControl(p.data(), p.size(), kRelay, kRelayPort); }

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
} // namespace

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
void P2pPunch(const char*, std::uint32_t, std::uint16_t, std::uint32_t, std::uint16_t) {}
void P2pSignal(std::uint32_t addr, std::uint16_t port, const void* data, std::size_t len) {
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
}

int main() {
    setenv("BB_THD_SERVER", "http://127.0.0.1:9", 1);
    setenv("BB_THD_AUTH", "http://127.0.0.1:9", 1);
    setenv("BB_THD_STUN", "off", 1);
    setenv("BB_THD_ONLINE_ID", "fixture", 1);
    setenv("BB_THD_TOKEN", "fixture-token", 1);

    unsigned ctx = 0;
    assert(BbThd::SignalingCreateContext(reinterpret_cast<void*>(&on_event), nullptr, &ctx) == 0 && ctx);

    // Not for us, too short, or an Offer from a player we know nothing about: no answer.
    deliver(packet(kActivationRequest, 1, "invader", "someone"));
    auto shortp = packet(kActivationRequest, 1, "invader", "fixture");
    BbThd::SignalingControl(shortp.data(), 20, kRelay, kRelayPort);
    deliver(packet(kOffer, 1, "stranger", "fixture"));
    std::this_thread::sleep_for(50ms);
    assert(take_sent().empty());
    assert(take_events().empty());

    // A bbhost invader's game activates its connection to us: ActivationAck, back to where it
    // came from (the relay).
    deliver(packet(kActivationRequest, 1, "invader", "fixture"));
    auto sent = take_sent();
    assert(sent.size() == 1 && sent[0].kind == kActivationAck && sent[0].from == "fixture" && sent[0].to == "invader");
    assert(sent[0].addr == kRelay && sent[0].port == kRelayPort);

    // Its handshake: Offer -> Accept, Check -> CheckAck with the same nonce and our Established.
    deliver(packet(kOffer, 1, "invader", "fixture"));
    sent = take_sent();
    assert(sent.size() == 1 && sent[0].kind == kAccept);
    std::this_thread::sleep_for(50ms);
    assert(take_events().empty()); // not up yet

    deliver(packet(kCheck, 1, "invader", "fixture", 0x1122334455667788ull));
    sent = take_sent();
    assert(sent.size() == 2 && sent[0].kind == kCheckAck && sent[0].nonce == 0x1122334455667788ull);
    assert(sent[1].kind == kEstablished);
    std::this_thread::sleep_for(100ms);
    auto ev = take_events();
    assert(ev.size() == 1 && ev[0].ctx == ctx && ev[0].event == 0x0a && ev[0].conn != 0); // PEER_ACTIVATED
    const unsigned conn = static_cast<unsigned>(ev[0].conn);

    // Resent steps are answered again, but PEER_ACTIVATED goes out once.
    deliver(packet(kCheck, 1, "invader", "fixture", 7));
    sent = take_sent();
    assert(sent.size() == 1 && sent[0].kind == kCheckAck && sent[0].nonce == 7);
    deliver(packet(kEstablished, 1, "invader", "fixture"));
    deliver(packet(kActivationAck, 1, "invader", "fixture"));
    deliver(packet(kAccept, 1, "invader", "fixture"));
    deliver(packet(kCheckAck, 1, "invader", "fixture"));
    std::this_thread::sleep_for(100ms);
    assert(take_sent().empty());
    assert(take_events().empty());

    // The game answers with its own activation: the same connection, ESTABLISHED then ACTIVE,
    // and our ActivationRequest to the invader (bbhost's game waits for MUTUAL_ACTIVATED, which
    // needs it), once.
    unsigned mine = 0;
    assert(BbThd::SignalingActivate(ctx, "invader", &mine) == 0 && mine == conn);
    sent = take_sent();
    assert(sent.size() == 1 && sent[0].kind == kActivationRequest && sent[0].to == "invader");
    assert(BbThd::SignalingActivate(ctx, "invader", &mine) == 0 && mine == conn);
    assert(take_sent().empty());
    std::this_thread::sleep_for(600ms);
    ev = take_events();
    assert(ev.size() == 2 && ev[0].event == 0x01 && ev[1].event == 0x0c && ev[0].conn == conn);
    BbThd::Peer p;
    assert(BbThd::PeersFindOnline("invader", &p) && p.addr == kRelay && p.port == kRelayPort);

    // The invader's game deactivates: PEER_DEACTIVATED; then its transport closes: DEAD.
    deliver(packet(kClose, 1, "invader", "fixture"));
    std::this_thread::sleep_for(100ms);
    ev = take_events();
    assert(ev.size() == 1 && ev[0].event == 0x0b && ev[0].conn == conn);
    deliver(packet(kClose, 2, "invader", "fixture"));
    std::this_thread::sleep_for(300ms);
    ev = take_events();
    assert(ev.size() == 1 && ev[0].event == 0x00 && ev[0].conn == conn);

    // After that, its Offer is a stranger's again.
    deliver(packet(kOffer, 1, "invader", "fixture"));
    assert(take_sent().empty());

    // A peer our game activated first (we host, a bbhost guest joins): its handshake is answered
    // and no PEER_ACTIVATED goes out for a connection the game made itself.
    BbThd::PeersProvisional("guest", kRelay, 6500);
    unsigned g = 0;
    assert(BbThd::SignalingActivate(ctx, "guest", &g) == 0 && g);
    std::this_thread::sleep_for(600ms);
    take_events();
    deliver(packet(kActivationRequest, 1, "guest", "fixture"));
    deliver(packet(kOffer, 1, "guest", "fixture"));
    deliver(packet(kCheck, 1, "guest", "fixture", 9));
    sent = take_sent();
    assert(sent.size() == 5 && sent[0].kind == kActivationAck && sent[1].kind == kActivationRequest &&
           sent[2].kind == kAccept && sent[3].kind == kCheckAck && sent[4].kind == kEstablished);
    std::this_thread::sleep_for(100ms);
    assert(take_events().empty());

    // Our game deactivates it: Close(deactivate) to the peer.
    BbThd::SignalingDeactivate(ctx, g);
    sent = take_sent();
    assert(sent.size() == 1 && sent[0].kind == kClose && sent[0].to == "guest");
    return 0;
}
