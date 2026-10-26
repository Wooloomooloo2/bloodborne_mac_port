// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include "common/types.h"

#include <miniupnpc.h>
#include <upnpcommands.h>

namespace Libraries::Net {

class UPnPClient {
public:
    static UPnPClient& Instance();

    // Kick off async IGD discovery. Safe to call multiple times — only the first call has effect.
    void Start();
    void SetP2PFeaturesEnabled(bool enabled);

    // Block until discovery completes or timeout_ms elapses. Returns true if IGD was found.
    bool WaitReady(int timeout_ms);

    bool IsAvailable() const;

    // External IP in network byte order as reported by the IGD. 0 if not yet known.
    u32 GetExternalIp() const;

    // External IP as a dotted-decimal string. Empty if not yet known.
    std::string GetExternalIpString() const;

    // External port the IGD mapped for us. Matches the internal port unless the router remapped it.
    u16 GetExternalPort() const;

    void AddMapping(u16 port);
    void AddMapping(u16 external, u16 internal); // bbport
    void RemoveMapping(u16 port);

    // Forwards `port` once discovery has finished (waiting up to 10 s for it), on a background
    // thread so the caller never waits for the router. Replaces an earlier request's mapping.
    void MapPortAsync(u16 port);
    // Removes what MapPortAsync forwarded, if it is `port`.
    void UnmapPort(u16 port);
    // bbport: forwards router port `external` to our `internal` port, for routers that give
    // every outbound flow a random external port (the signaling server then tells peers that
    // port, which only the server could reach). Same thread as MapPortAsync; repeated calls
    // with the same ports do nothing, a new external port replaces the previous mapping.
    void MapExternalPortAsync(u16 external, u16 internal);

private:
    UPnPClient() = default;
    ~UPnPClient();
    UPnPClient(const UPnPClient&) = delete;
    UPnPClient& operator=(const UPnPClient&) = delete;

    void DiscoverThread();
    // bbport: mappings are leased for an hour and renewed every half hour while the game runs,
    // so a crashed game leaves nothing forwarded for long; routers that only take permanent
    // mappings get one (removed on exit as before).
    void StartRefresh();
    void RefreshThread();

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_p2p_features_enabled{true};
    std::atomic<bool> m_done{false};
    std::atomic<bool> m_available{false};
    std::atomic<u32> m_external_ip_nbo{0};
    std::atomic<u16> m_external_port{0};
    std::atomic<u16> m_mapped_port{0}; // forwarded by MapPortAsync, removed on exit
    std::atomic<u16> m_mapped_external{0}; // bbport: MapExternalPortAsync's router port
    std::atomic<u16> m_mapped_internal{0}; // bbport: ... and the port it goes to
    std::atomic<bool> m_permanent_only{false}; // bbport: the router refused a lease (725)
    char m_external_ip_str[16]{};

    char m_lan_addr[64]{};
    UPNPUrls m_urls{};
    IGDdatas m_data{};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    std::mutex m_map_mutex; // serializes MapPortAsync / UnmapPort
    std::thread m_map_thread;
    std::mutex m_refresh_mutex; // bbport: the lease renewal
    std::condition_variable m_refresh_cv;
    bool m_refresh_stop = false;
    std::thread m_refresh_thread;
};

} // namespace Libraries::Net
