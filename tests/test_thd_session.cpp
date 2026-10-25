// SPDX-License-Identifier: GPL-3.0-or-later
// Real HTTP against a loopback fixture; no game, account, router or public service is used.
#include "httplib.h"
#include "thd/thd_internal.h"

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
    std::atomic<int> creates{0}, polls{0}, requested_wait{0}, mode{0};
    server.Get("/warm", [](const auto&, auto& res) { res.set_content("{}", "application/json"); });
    server.Post("/create", [&](const auto&, auto& res) {
        ++creates; // the operation happened, then the response body was lost
        res.set_content_provider(10, "application/json", [](size_t, size_t, httplib::DataSink&) { return false; });
    });
    server.Post("/np/events/poll", [&](const auto& req, auto& res) {
        ++polls;
        requested_wait = BbThd::Json::parse(req.body).value("WaitMs", 0);
        if (mode == 2) {
            while (!req.is_connection_closed()) std::this_thread::sleep_for(2ms);
        }
        BbThd::Json body{{"WaitMaxMs", 1LL << 40}, {"NextCursor", 1}, {"HasEvent", mode == 1}};
        res.set_content(body.dump(), "application/json");
    });
    server.Post("/np/events/ack", [](const auto&, auto& res) { res.set_content("{}", "application/json"); });
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

    BbThd::PollerStart({});
    Await([&] { return polls >= 2; });
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
    std::puts("PASS: THD HTTP retries, empty-poll pacing, wait bounds, and worker stop/restart");
}
