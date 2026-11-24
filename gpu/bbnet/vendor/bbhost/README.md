# bbhost's NpSignaling

`np_signaling.cpp` is droogie/bbhost's `src/hle/np_signaling.cpp` at commit
3f9c4c05e447c8447aee60b6a4223a875187bd39 (the same file in v0.2.31 to v0.2.34), unchanged
(SHA-256 2048305d1029a524f4ff86435d5ab8bdb80a1e2fbdfa09b5d0e3f78d44a00c8c). Copyright the bbhost contributors,
GPL-3.0-or-later (`LICENSES/GPL-3.0-or-later.txt`).

It is the connection handshake The Hunter's Dream's server has required for co-op, invasions and
PvP since bbhost v0.2.31 (2026-11-11, bbhost PR #217): Offer, Accept, Check and CheckAck datagrams
on the P2P port, an ActivationRequest and its Ack, 500 ms resends, 30 s to give up, and
ESTABLISHED, MUTUAL_ACTIVATED, PEER_ACTIVATED and DEAD events as the game's SocketState handler
expects them. It is built with the adapter headers in `../bbhost_shim` (the include paths it was
written against) over `thd/thd_bbhost_glue.cpp`, which maps its needs onto the backend's peer
table, dispatcher, session service and P2P port. The vendoring follows Lantern Engine 0.9.42's,
which carries the same file the same way.

To update: copy the file from bbhost, refresh the commit and hash here, and check the adapter
headers against its includes and the `hle_*` / `net::*` names it uses.
