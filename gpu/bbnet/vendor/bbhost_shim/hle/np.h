// SPDX-License-Identifier: GPL-3.0-or-later
// Adapter for the vendored bbhost NpSignaling: what it offers the rest of the backend (hle_np_signaling_*)
// and what it asks of NpMatching2 (hle_np_matching2_*, in thd_matching2.cpp) and the NP id text
// (the glue).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
std::string hle_np_npid_text(const void* npid);
void hle_register_np_signaling();
// A player is a room member now: its transports and connections learn the peer-table entry.
void hle_np_signaling_peer_known(std::uint16_t member_id);
void hle_np_signaling_reset();
// The player left for good: its connections DEAD (terminated by peer).
void hle_np_signaling_peer_dead(const std::string& online_id);
// A kSignalMagic datagram from the P2P port (on the dispatcher).
void hle_np_signaling_control(const std::uint8_t* data, std::size_t len, std::uint32_t addr, std::uint16_t port);
// The game holds a connection to that player.
bool hle_np_signaling_live(const std::string& online_id);
// NpMatching2's connection to a member: starts the handshake (shadPS4's StartMatching2 / StopMatching2).
void hle_np_signaling_m2_connect(const std::string& online_id);
int hle_np_signaling_transport_status(const std::string& online_id); // 0 none, 1 in handshake, 2 up
void hle_np_signaling_m2_stop(const std::string& online_id);
// NpMatching2 hears from the signaling (thd_matching2.cpp).
void hle_np_matching2_transport_up(const std::string& online_id);
void hle_np_matching2_transport_dead(const std::string& online_id, int error);
void hle_np_matching2_peer_closed(const std::string& online_id);
