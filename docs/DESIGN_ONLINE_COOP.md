# Design: online play and co-op

Status (2026-10-24): **plan.** Online play is PR #9 (noelmiller, open, needs a rebase); PR #11
holds the two co-op-exposed renderer stops. Nothing here is on master yet.

## The options

| Option | What it is | Licence | Fit and maturity |
|---|---|---|---|
| **The Hunter's Dream (THD)** via droogie/bbhost's backend (PR #9) | The console online: bells, signs, invasions, messages, bloodstains. Servers match and relay; game traffic is peer-to-peer UDP | GPL-3 (`gpu/bbnet/thd`) + GPL-2 | Already wired into `src/runtime_net.c` → `libbbnet`; byte-checked 1.09 addresses; the biggest player pool (Windows bbport/bbhost); Mac ↔ Windows verified by Noel |
| shadNet pool (PR #9) | Same, on shadPS4's server | GPL-2 | Weak: plain-TCP login, and since 2026-10-12 the server only takes shadPS4's own builds |
| **BBSC / Seamless Hunt** (bbport 0.6, `docs/BBSC.md` upstream) | Seamless co-op through servers (shadNet + seamlesshunt.com) | GPL-2.0-or-later | Plain HTTP game server; patches switched at run time (code kept writable) |
| **Nightwatch Co-op** (vladtepes22, from Lantern Engine + visy's co-op) | Seamless, serverless peer-to-peer, host's world, up to 4, party codes, encrypted, port 9307 | GPL-3 **plus Lantern Engine section 7 terms** (attribution in README/About/settings, marked as modified, no "Lantern Engine" name) | Windows build, ~190 files, x86 detours and callbacks into game code (runs in bb-probe under Rosetta), Vulkan name tags; only identical builds can play together. evill33t/bloodborne_mac vendored it on macOS but has not tested two Macs or the internet |

What "a lot of the PC versions have" is mostly THD (signs and summons with Windows players) and,
newer, Nightwatch's seamless co-op.

## Recommendation

1. **0.6: THD through PR #9.** Plays with the largest pool including Windows players; certificate
   checks on; licence (GPL-3 for online builds) already accepted.
2. **0.7: seamless co-op as a separate opt-in mode**, after deciding on Lantern's section 7
   terms (Nightwatch) or plain-HTTP servers (BBSC). Ask vladtepes22 first whether Mac builds may
   join Windows Nightwatch players (its build hash rejects any different build).

## State of PR #9 (2026-10-21)

- Done since the 2026-10-18 review: long polling (asked for by the THD operators), the launcher's
  Online tab (Keychain, Discord sign-in), online-module build failure is a warning (594bd47).
- Client identity: THD's operators were "glad to be asked" and want each client to send its own
  name in `User-Agent`; `run.sh` sends `bbport-mac <tag>`. Open: whether matchmaking only pairs
  known client names (an unknown name saw no hosts once), so `bbport-mac` may need adding on
  their side. Get their OK in writing and link it in `docs/ONLINE.md`.
- The IME dialog is already on master (ee25f38) and drops out at the rebase.
- Needs a rebase onto 0.5.5 and a fresh security review at merge time.

## Integration notes

- THD and BBSC run through `src/runtime_net.c` → `libbbnet.dylib` in the game process.
- Nightwatch hooks check function prologues; our loader rewrites `fs:[0]` accesses in the eboot,
  so any hook whose checked bytes include a rewritten instruction would refuse to install. Check
  every hook against our loaded image first.
- Patches switched while the game runs need the code writable, and under Rosetta a protection
  change after each write (CLAUDE.md "Rosetta + aliased memory").
- Nightwatch's Vulkan overlays (name tags, pings) would need a channel to the GPU host, or be
  dropped at first.

## Open questions

- THD: confirm `bbport-mac` is matched with Windows players; operators' written OK.
- Licences: Lantern section 7 (Nightwatch) is a separate decision from GPL-3; licence files for
  protobuf, OpenSSL, miniupnpc, nlohmann_json, httplib in online builds.
- Security: shadNet's password goes in clear (keep shadNet off by default or drop it); parsing
  peer UDP in the game process is attack surface; Nightwatch needs an inbound port.
- Packaging: confirm `package_app.sh` bundles `libbbnet.dylib` after the rebase.

## Milestones and tests

- **M1:** #9 rebased on 0.5.5; CI builds with and without the online module; security re-review.
- **M2:** #11 merged after a ≥ 1 h co-op session with no pass-mismatch or chunk-race warnings.
- **M3:** operators confirm the identity; packaging; 0.6 pre-release.
- **M4:** seamless co-op spike on a branch: Nightwatch hooks against our loaded image.
- Tests (test saves only): Mac ↔ Windows on THD (summon, be summoned, invade, messages,
  bloodstains, relay across NAT); Mac ↔ Mac on two networks; disconnect and reconnect, lamp
  travel, boss kill; an offline build makes no network calls.
