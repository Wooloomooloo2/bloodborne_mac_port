// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend: the vendored bbhost NpSignaling (../vendor/bbhost/np_signaling.cpp)
// on this backend. Its adapter headers (../vendor/bbhost_shim) declare what it needs; this file
// provides it: its log, its import table, its P2P datagrams, bbhost's net:: session layer over the
// peer table, dispatcher and session service, and the libSceNpSignaling NIDs bound to the imports
// it registered. After Lantern Engine 0.9.42's thd_bbhost_glue.cpp (the same backend lineage).
#include "thd_internal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "core/libraries/libs.h"
#include "../vendor/bbhost_shim/hle/common.h"
#include "../vendor/bbhost_shim/hle/net_p2p.h"
#include "../vendor/bbhost_shim/hle/np.h"
#include "../vendor/bbhost_shim/log.h"
#include "../vendor/bbhost_shim/net/session.h"

namespace {
std::mutex g_fn_mu;
std::map<std::string, void*>& Functions() {
    static std::map<std::string, void*> functions;
    return functions;
}

net::Peer ToNet(const BbThd::Peer& p) {
    net::Peer out;
    out.member_id = p.member_id;
    out.online_id = p.online_id;
    out.addr = p.addr;
    out.port = p.port;
    return out;
}
} // namespace

void bbhost_np_log(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    BbThd::Log("%s", text);
}

void register_hle_fn(const char* name, void* fn) {
    std::lock_guard lk(g_fn_mu);
    Functions()[name] = fn;
}

std::string hle_np_npid_text(const void* npid) { return BbThd::NpIdText(npid); }

void hle_net_p2p_signal(std::uint32_t addr, std::uint16_t port, const void* data, std::size_t len) {
    BbThd::P2pSignalSend(addr, port, data, len);
}

namespace net {
void dispatch_after(int delay_ms, Prio prio, std::function<void()> fn) {
    BbThd::DispatchAfter(delay_ms, static_cast<BbThd::Prio>(prio), std::move(fn));
}
bool peers_get(std::uint16_t member_id, Peer* out) {
    BbThd::Peer p;
    if (!BbThd::PeersGet(member_id, &p)) return false;
    *out = ToNet(p);
    return true;
}
bool peers_find_online(const std::string& id, Peer* out) {
    BbThd::Peer p;
    if (!BbThd::PeersFindOnline(id, &p)) return false;
    *out = ToNet(p);
    return true;
}
void peers_set_conn(std::uint16_t member_id, unsigned conn_id, int status) {
    BbThd::PeersSetConn(member_id, conn_id, status);
}
Peer peers_provisional(const std::string& id, std::uint32_t addr, std::uint16_t port) {
    return ToNet(BbThd::PeersProvisional(id, addr, port));
}
bool server_signaling_resolve(const std::string& id, std::uint32_t* addr, std::uint16_t* port, std::string& error) {
    return BbThd::ServerSignalingResolve(id, addr, port, error);
}
std::string online_id() { return BbThd::OnlineId(); }
} // namespace net

namespace {
template <typename F> F Vendored(const char* name) {
    std::lock_guard lk(g_fn_mu);
    const auto it = Functions().find(name);
    if (it == Functions().end()) {
        BbThd::Log("np: the vendored NpSignaling has no %s", name);
        return nullptr;
    }
    return reinterpret_cast<F>(it->second);
}

using InitFn = int(PS4_SYSV_ABI*)(u64, int, int, u64);
using TermFn = int(PS4_SYSV_ABI*)();
using CreateFn = int(PS4_SYSV_ABI*)(const void*, void*, void*, unsigned*);
using DeleteFn = int(PS4_SYSV_ABI*)(unsigned);
using ActivateFn = int(PS4_SYSV_ABI*)(unsigned, const void*, unsigned*);
using DeactivateFn = int(PS4_SYSV_ABI*)(unsigned, unsigned);
using StatusFn = int(PS4_SYSV_ABI*)(unsigned, unsigned, int*, std::uint32_t*, std::uint16_t*);
InitFn g_init = nullptr;
TermFn g_term = nullptr;
CreateFn g_create = nullptr;
DeleteFn g_delete = nullptr;
ActivateFn g_activate = nullptr;
DeactivateFn g_deactivate = nullptr;
StatusFn g_status = nullptr;
constexpr int kNotLoaded = static_cast<int>(0x80552715); // SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED

s32 PS4_SYSV_ABI SigInit(u64 a, int b, int c, u64 d) { return g_init ? g_init(a, b, c, d) : kNotLoaded; }
s32 PS4_SYSV_ABI SigTerm() { return g_term ? g_term() : kNotLoaded; }
s32 PS4_SYSV_ABI SigCreate(const void* npid, void* cb, void* arg, unsigned* ctx) {
    return g_create ? g_create(npid, cb, arg, ctx) : kNotLoaded;
}
s32 PS4_SYSV_ABI SigDelete(unsigned ctx) { return g_delete ? g_delete(ctx) : kNotLoaded; }
s32 PS4_SYSV_ABI SigActivate(unsigned ctx, const void* npid, unsigned* conn) {
    return g_activate ? g_activate(ctx, npid, conn) : kNotLoaded;
}
s32 PS4_SYSV_ABI SigDeactivate(unsigned ctx, unsigned conn) {
    return g_deactivate ? g_deactivate(ctx, conn) : kNotLoaded;
}
s32 PS4_SYSV_ABI SigStatus(unsigned ctx, unsigned conn, int* st, std::uint32_t* addr, std::uint16_t* port) {
    return g_status ? g_status(ctx, conn, st, addr, port) : kNotLoaded;
}
} // namespace

namespace BbThd {
void RegisterVendoredSignaling(Core::Loader::SymbolsResolver* sym) {
    if (Trace()) Log("np: BB_THD_TRACE=1: BBHOST_NP_TRACE=1 traces the signaling handshake too");
    hle_register_np_signaling();
    g_init = Vendored<InitFn>("sceNpSignalingInitialize");
    g_term = Vendored<TermFn>("sceNpSignalingTerminate");
    g_create = Vendored<CreateFn>("sceNpSignalingCreateContext");
    g_delete = Vendored<DeleteFn>("sceNpSignalingDeleteContext");
    g_activate = Vendored<ActivateFn>("sceNpSignalingActivateConnection");
    g_deactivate = Vendored<DeactivateFn>("sceNpSignalingDeactivateConnection");
    g_status = Vendored<StatusFn>("sceNpSignalingGetConnectionStatus");
    Log("np: NpSignaling is bbhost's (v0.2.31 handshake)");
#define SIG(nid, fn) LIB_FUNCTION(nid, "libSceNpSignaling", 1, "libSceNpSignaling", fn)
    SIG("3KOuC4RmZZU", SigInit);
    SIG("NPhw0UXaNrk", SigTerm);
    SIG("5yYjEdd4t8Y", SigCreate);
    SIG("hx+LIg-1koI", SigDelete);
    SIG("0UvTFeomAUM", SigActivate);
    SIG("6UEembipgrM", SigDeactivate);
    SIG("bD-JizUb3JM", SigStatus);
#undef SIG
}
} // namespace BbThd
