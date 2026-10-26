// SPDX-License-Identifier: GPL-3.0-or-later
// Real HTTP against a loopback fixture; no game, account, router or public service is used.
#include "httplib.h"
#include "thd/thd_internal.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

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

template <class F> void Await(F ready) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
}

int main() {
    httplib::Server server;
    std::atomic<int> creates{0}, actions{0}, polls{0}, requested_wait{0}, mode{0};
    std::atomic<int> action_port{0}, poll_port{0}, reused_polls{0};
    std::atomic<long long> requested_cursor{0}, acked_cursor{0};
    server.set_keep_alive_timeout(1); // expire idle sockets quickly, as the public service does
    server.Get("/warm", [](const auto&, auto& res) { res.set_content("{}", "application/json"); });
    server.Post("/action", [&](const auto& req, auto& res) {
        ++actions;
        assert(req.get_header_value("Connection") == "close");
        assert(action_port.exchange(req.remote_port) != req.remote_port);
        res.set_content("{}", "application/json");
    });
    server.Post("/create", [&](const auto&, auto& res) {
        ++creates; // the operation happened, then the response body was lost
        res.set_content_provider(10, "application/json", [](size_t, size_t, httplib::DataSink&) { return false; });
    });
    server.Post("/np/events/poll", [&](const auto& req, auto& res) {
        if (poll_port.exchange(req.remote_port) == req.remote_port) ++reused_polls;
        const auto request = BbThd::Json::parse(req.body);
        requested_wait = request.value("WaitMs", 0);
        const auto cursor = request.value("Cursor", 0LL);
        requested_cursor = cursor;
        ++polls;
        if (mode == 2) {
            while (!req.is_connection_closed()) std::this_thread::sleep_for(2ms);
        }
        // The fixture retains history even after ack, selecting events by the supplied cursor.
        // Event 2 belongs to the stopped context; event 3 arrives for its replacement.
        const long long next = mode == 3 ? 3 : mode == 1 ? 2 : 1;
        const bool has_event = (mode == 1 || mode == 3) && cursor < next;
        BbThd::Json body{{"WaitMaxMs", 1LL << 40}, {"NextCursor", std::max(cursor, next)},
                        {"HasEvent", has_event}, {"EventId", next}, {"Name", "guest_invite"}};
        res.set_content(body.dump(), "application/json");
    });
    server.Post("/np/events/ack", [&](const auto& req, auto& res) {
        acked_cursor = BbThd::Json::parse(req.body).value("Cursor", 0LL);
        res.set_content("{}", "application/json");
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
    Await([&] { return server.is_running(); });

    BbThd::Json reply;
    std::string error;
    assert(BbThd::NpGet("/warm", reply, error));
    assert(!BbThd::NpPost("/create", BbThd::Json::object(), reply, error));
    assert(creates == 1); // never replay a POST after losing its response

    // Action clients close every connection, including on successive calls from one thread.
    assert(BbThd::NpPost("/action", BbThd::Json::object(), reply, error));
    assert(BbThd::NpPost("/action", BbThd::Json::object(), reply, error));
    std::this_thread::sleep_for(1100ms); // longer than this server's idle keep-alive lifetime
    assert(BbThd::NpPost("/action", BbThd::Json::object(), reply, error));
    assert(actions == 3); // the first action after idle succeeds without a second bell/retry

    BbThd::PollerStart({});
    Await([&] { return polls >= 2; });
    assert(reused_polls >= 1); // keep-alive stays enabled on the continuously active poller
    assert(requested_wait == 25000); // clamp the 64-bit server value before narrowing
    const int before = polls;
    std::this_thread::sleep_for(300ms);
    assert(polls - before <= 2); // an immediate empty long-poll answer must not spin
    BbThd::PollerStop();
    const int stopped = polls;
    std::this_thread::sleep_for(300ms);
    assert(polls == stopped);

    // Stop waits for the old event handler to finish before a new context may start.
    mode = 1;
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false, release = false;
    BbThd::PollerStart([&](const auto&) {
        std::unique_lock lk(mu);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return release; });
    });
    {
        std::unique_lock lk(mu);
        assert(cv.wait_for(lk, 5s, [&] { return entered; }));
    }
    auto stopping = std::async(std::launch::async, BbThd::PollerStop);
    assert(stopping.wait_for(50ms) == std::future_status::timeout);
    {
        std::lock_guard lk(mu);
        release = true;
        cv.notify_all();
    }
    assert(stopping.wait_for(2s) == std::future_status::ready);
    stopping.get();
    assert(acked_cursor == 0); // Stop must not ack the old handler's event

    // Even without an ack, the replacement context resumes after the event already observed.
    std::atomic<int> replayed{0};
    const int before_restart = polls;
    BbThd::PollerStart([&](const auto&) { ++replayed; });
    Await([&] { return polls >= before_restart + 2; });
    BbThd::PollerStop();
    assert(requested_cursor == 2);
    assert(replayed == 0);

    // Preserving the cursor must still deliver a new invite normally after a restart.
    mode = 3;
    std::atomic<int> delivered{0};
    BbThd::PollerStart([&](const auto& event) {
        assert(BbThd::IntOf(event, "EventId", 0) == 3);
        ++delivered;
    });
    Await([&] { return acked_cursor == 3; });
    BbThd::PollerStop();
    assert(delivered == 1);

    // A held poll is interrupted promptly, including across repeated context restarts.
    mode = 2;
    for (int i = 0; i < 5; ++i) {
        const int previous = polls;
        BbThd::PollerStart({});
        Await([&] { return polls > previous; });
        auto stop = std::async(std::launch::async, BbThd::PollerStop);
        assert(stop.wait_for(2s) == std::future_status::ready);
        stop.get();
    }
    for (int i = 0; i < 20; ++i) {
        BbThd::PollerStart({});
        auto stop = std::async(std::launch::async, BbThd::PollerStop);
        assert(stop.wait_for(2s) == std::future_status::ready);
        stop.get(); // cancellation may race the very first request's connection setup
    }
    server.stop();
    serving.join();
    std::puts("PASS: fresh action connections, no duplicate POSTs, poll keep-alive, cursor retention, pacing, bounds, and stop/restart");
}
