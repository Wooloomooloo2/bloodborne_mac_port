// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstring>
#include <upnperrors.h>
#include "common/logging/log.h"
#include "net_upnp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <net/if.h>
#endif

namespace Libraries::Net {

// bbport: the lease of a mapping, renewed at half of it (RefreshThread).
constexpr int UpnpLeaseSeconds = 3600;
constexpr int UpnpOnlyPermanentLeases = 725;

UPnPClient& UPnPClient::Instance() {
    static UPnPClient instance;
    return instance;
}

UPnPClient::~UPnPClient() {
    if (m_thread.joinable())
        m_thread.join();
    if (m_map_thread.joinable())
        m_map_thread.join();
    {
        std::scoped_lock lock{m_refresh_mutex};
        m_refresh_stop = true;
    }
    m_refresh_cv.notify_all();
    if (m_refresh_thread.joinable())
        m_refresh_thread.join();
    // Leave nothing behind on the router (a lease would end within the hour anyway).
    if (const u16 port = m_mapped_port.exchange(0); port != 0)
        RemoveMapping(port);
    if (const u16 port = m_mapped_external.exchange(0); port != 0)
        RemoveMapping(port);
    if (m_available.load())
        FreeUPNPUrls(&m_urls);
}

void UPnPClient::Start() {
    if (!m_p2p_features_enabled.load()) {
        LOG_INFO(Lib_Net, "UPNP: skipped because Matching2 is disabled");
        return;
    }

    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true))
        return;
    LOG_INFO(Lib_Net, "UPNP: starting discovery thread");
    m_thread = std::thread(&UPnPClient::DiscoverThread, this);
}

void UPnPClient::SetP2PFeaturesEnabled(bool enabled) {
    m_p2p_features_enabled.store(enabled);
}

bool UPnPClient::WaitReady(int timeout_ms) {
    std::unique_lock lock(m_mutex);
    m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return m_done.load(); });
    return m_available.load();
}

bool UPnPClient::IsAvailable() const {
    return m_available.load();
}

u32 UPnPClient::GetExternalIp() const {
    return m_external_ip_nbo.load();
}

std::string UPnPClient::GetExternalIpString() const {
    return m_available.load() ? std::string(m_external_ip_str) : std::string{};
}

u16 UPnPClient::GetExternalPort() const {
    return m_external_port.load();
}

void UPnPClient::DiscoverThread() {
    int error = 0;
    UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error);

    if (!devlist) {
        LOG_INFO(Lib_Net, "UPNP: no IGD found (error={})", error);
        m_done = true;
        m_cv.notify_all();
        return;
    }

    char wan_addr[40]{};
    const int ret = UPNP_GetValidIGD(devlist, &m_urls, &m_data, m_lan_addr, sizeof(m_lan_addr),
                                     wan_addr, sizeof(wan_addr));
    freeUPNPDevlist(devlist);

    if (ret != 1) {
        LOG_INFO(Lib_Net, "UPNP: no valid IGD (ret={})", ret);
        m_done = true;
        m_cv.notify_all();
        return;
    }

    char external_ip[40]{};
    if (UPNP_GetExternalIPAddress(m_urls.controlURL, m_data.first.servicetype, external_ip) !=
        UPNPCOMMAND_SUCCESS) {
        LOG_WARNING(Lib_Net, "UPNP: IGD found but failed to get external IP");
        FreeUPNPUrls(&m_urls);
        m_done = true;
        m_cv.notify_all();
        return;
    }

    u32 ip_nbo = 0;
    inet_pton(AF_INET, external_ip, &ip_nbo);
    m_external_ip_nbo.store(ip_nbo);
    std::strncpy(m_external_ip_str, external_ip, sizeof(m_external_ip_str) - 1);
    m_available = true;

    LOG_INFO(Lib_Net, "UPNP: IGD found - external IP={} lan={} (port mapping available)",
             external_ip, m_lan_addr);

    m_done = true;
    m_cv.notify_all();
}

void UPnPClient::AddMapping(u16 port) {
    AddMapping(port, port);
}

void UPnPClient::AddMapping(u16 external, u16 internal) {
    if (!m_p2p_features_enabled.load())
        return;

    if (!m_available.load())
        return;

    const std::string external_str = std::to_string(external);
    const std::string internal_str = std::to_string(internal);
    // bbport: a lease ("0" was permanent), unless the router takes only permanent mappings.
    const std::string lease = std::to_string(UpnpLeaseSeconds);
    auto add = [&](const char* duration) {
        return UPNP_AddPortMapping(m_urls.controlURL, m_data.first.servicetype,
                                   external_str.c_str(), internal_str.c_str(), m_lan_addr,
                                   "shadPS4", "UDP", nullptr, duration);
    };
    int ret = add(m_permanent_only.load() ? "0" : lease.c_str());
    if (ret == UpnpOnlyPermanentLeases && !m_permanent_only.exchange(true)) {
        LOG_INFO(Lib_Net, "UPNP: the router takes only permanent mappings (removed on exit)");
        ret = add("0");
    }

    if (ret == UPNPCOMMAND_SUCCESS) {
        StartRefresh();
        m_external_port.store(external);
        if (external == internal)
            LOG_INFO(Lib_Net, "UPNP: mapped UDP port {}", external);
        else
            LOG_INFO(Lib_Net, "UPNP: mapped router UDP port {} to our port {}", external, internal);
    } else {
        // Store the requested port as best-guess even on failure so callers still get a value.
        m_external_port.store(external);
        LOG_WARNING(Lib_Net, "UPNP: failed to map port {} -> {} ({})", external, internal,
                    strupnperror(ret));
    }
}

void UPnPClient::MapExternalPortAsync(u16 external, u16 internal) {
    std::scoped_lock lock{m_map_mutex};
    if (external == 0 || internal == 0 || m_mapped_external.load() == external)
        return;
    if (m_map_thread.joinable())
        m_map_thread.join();
    m_map_thread = std::thread([this, external, internal] {
        if (!WaitReady(10'000)) {
            LOG_INFO(Lib_Net, "UPNP: no IGD, router UDP port {} is not forwarded", external);
            return;
        }
        m_mapped_internal.store(internal);
        const u16 previous = m_mapped_external.exchange(external);
        if (previous != 0 && previous != external)
            RemoveMapping(previous);
        AddMapping(external, internal);
    });
}

void UPnPClient::MapPortAsync(u16 port) {
    std::scoped_lock lock{m_map_mutex};
    if (m_map_thread.joinable())
        m_map_thread.join();
    m_map_thread = std::thread([this, port] {
        if (!WaitReady(10'000)) {
            LOG_INFO(Lib_Net, "UPNP: no IGD, UDP port {} is not forwarded", port);
            return;
        }
        const u16 previous = m_mapped_port.exchange(port);
        if (previous != 0 && previous != port)
            RemoveMapping(previous);
        AddMapping(port);
    });
}

void UPnPClient::UnmapPort(u16 port) {
    std::scoped_lock lock{m_map_mutex};
    if (m_map_thread.joinable())
        m_map_thread.join();
    u16 expected = port;
    if (m_mapped_port.compare_exchange_strong(expected, 0))
        RemoveMapping(port);
}

void UPnPClient::StartRefresh() {
    std::scoped_lock lock{m_refresh_mutex};
    if (!m_refresh_stop && !m_refresh_thread.joinable())
        m_refresh_thread = std::thread(&UPnPClient::RefreshThread, this);
}

void UPnPClient::RefreshThread() {
    std::unique_lock lock{m_refresh_mutex};
    while (!m_refresh_cv.wait_for(lock, std::chrono::seconds(UpnpLeaseSeconds / 2),
                                  [this] { return m_refresh_stop; })) {
        if (m_permanent_only.load())
            continue;
        lock.unlock();
        if (const u16 port = m_mapped_port.load(); port != 0)
            AddMapping(port);
        if (const u16 port = m_mapped_external.load(); port != 0)
            AddMapping(port, m_mapped_internal.load());
        lock.lock();
    }
}

void UPnPClient::RemoveMapping(u16 port) {
    if (!m_available.load())
        return;

    const std::string port_str = std::to_string(port);
    UPNP_DeletePortMapping(m_urls.controlURL, m_data.first.servicetype, port_str.c_str(), "UDP",
                           nullptr);

    LOG_INFO(Lib_Net, "UPNP: removed UDP port mapping {}", port);
}

} // namespace Libraries::Net
