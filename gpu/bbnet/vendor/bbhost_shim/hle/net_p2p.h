// SPDX-License-Identifier: GPL-3.0-or-later
// Adapter for the vendored bbhost NpSignaling: its datagrams on the P2P port. The magic is the
// wire format every bbhost since v0.2.31 speaks; the reader (thd_net.cpp) hands such datagrams to
// the signaling instead of the game, and the send goes out through the port (and the relay).
#pragma once
#include <cstddef>
#include <cstdint>
constexpr std::uint8_t kSignalMagic[4] = {0xfe, 'b', 'b', 's'};
void hle_net_p2p_signal(std::uint32_t addr, std::uint16_t port, const void* data, std::size_t len);
