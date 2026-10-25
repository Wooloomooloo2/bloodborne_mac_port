#pragma once
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        Stop();
        std::scoped_lock lock{mutex};
        thread = std::jthread([func = std::move(func)](std::stop_token stop) {
            runtime_thread_attach_host("bb:hle");
            func(stop);
        });
        stop_source = thread.get_stop_source();
    }
    // AvPlayer threads stop their own Thread objects while the owner may be stopping them too
    // (AvPlayerSource::Stop). The jthread is taken out under the lock, so exactly one caller
    // ends it: the thread itself detaches, anyone else joins.
    void Join() { Finish(Take()); }
    bool Joinable() const {
        std::scoped_lock lock{mutex};
        return thread.joinable();
    }
    // Through the kept stop source: the jthread may already have been taken by another
    // thread's Join. At the end of a movie the demuxer joins the decoders while the game closes
    // the player: the owner's request then reached an empty jthread, the video decoder kept
    // waiting for a frame buffer the game no longer returns, and the game froze in
    // sceAvPlayerClose (#46: the opening movie of a new game, on slower Macs).
    bool RequestStop() {
        std::scoped_lock lock{mutex};
        return stop_source.request_stop();
    }
    void Stop() {
        RequestStop();
        Finish(Take());
    }

private:
    std::jthread Take() {
        std::scoped_lock lock{mutex};
        return std::move(thread);
    }
    static void Finish(std::jthread taken) {
        if (!taken.joinable()) return;
        if (taken.get_id() == std::this_thread::get_id()) taken.detach();
        else taken.join();
    }

    mutable std::mutex mutex;
    std::jthread thread;
    std::stop_source stop_source{std::nostopstate}; ///< the thread's, kept after a Take
};
} // namespace Libraries::Kernel
