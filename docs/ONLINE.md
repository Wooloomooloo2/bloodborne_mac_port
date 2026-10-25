# Online play on the Mac: basic setup

Bloodborne's online features on this port: messages, bloodstains and wandering ghosts come from
the community game server (The Hunter's Dream), and bells, summons and invasions go through a
co-op pool. Two pools exist:

| Pool | Who is on it | Account | Needs |
|---|---|---|---|
| **The Hunter's Dream** (`online_pool=huntersdream`, the default) | bbhost players and the Windows port (Supermedo/bloodborne_pc) | Discord-linked, free | nothing on the router: the pool's server relays and punches through NATs |
| **shadNet** (`online_pool=shadnet`) | shadPS4 players and the Windows port's shadNet setting | shadNet account from shadnet.shadps4.net | a router whose NAT lets UDP through (UPnP is asked for the port) |

Players only meet other players on the same pool, same game version (1.09) and, for bells,
the same password set under **Network Settings** in the game.

Everything below is opt-in: without `online=1` the game never contacts the network.

## 1. Build with the online module

The module (`out/gpu/libbbnet.dylib`) needs four extra libraries, built once by the same script
as the rest of the dependencies. If you built the port before the module existed, run the
bootstrap again; it only builds what is missing.

```sh
cd bloodborne_mac_port
bash scripts/macos/bootstrap_deps.sh        # protobuf, OpenSSL, miniupnpc, nlohmann_json
bash build_macos.sh                         # prints "Online module: .../libbbnet.dylib"
```

Without those libraries the build still succeeds and the port plays offline.
`BB_NET=OFF bash build_macos.sh` also removes an older module from the build output.
Release packaging includes the built x86-64 module in `Contents/Frameworks`, together with
its [source and license notices](ONLINE_PROVENANCE.md). A package built without it stays
offline. The release version is carried into the online client identity without needing Git.

## 2. Turn online play on

Open `Bloodborne.app`, switch to its **Online** tab and turn **Play online** on. The launcher
writes the settings below into `~/Library/Application Support/Bloodborne/bbport.ini` when you
press Play or quit it (the in-game menu writes the same file; close the launcher before editing
it by hand). Without the launcher, add the line yourself:

```ini
online=1
```

## 3. Pick a pool and sign in

### The Hunter's Dream (recommended: works through any router)

In the **Online** tab, with the pool set to **The Hunter's Dream**, press **Sign in with
Discord…**. The window shows a web address and a code: open the page (**Open Page**), sign in
with Discord there and enter the code. Within a few seconds the window says *Signed in as
\<your name\>*; **Sign Out** forgets the account again. The token is kept in your login keychain
(item `bbport-huntersdream`), and bbport.ini gets `online_pool=huntersdream`,
`thd_online_id=<your name>` and `thd_token=keychain`.

The same from Terminal, the way the Windows port's launcher does it (the command prints the
address and the code and waits for the approval):

```sh
python3 scripts/online.py signin --keychain --ini ~/Library/Application\ Support/Bloodborne/bbport.ini
```

Without `--keychain` the token is written into the file, which then becomes readable by you
alone.

### shadNet

Register at [shadnet.shadps4.net](https://shadnet.shadps4.net). In the **Online** tab set the
pool to **shadNet server** and fill in the account name and the password (and another server
address, if not shadPS4's public one). The password goes to your login keychain (item
`bbport-shadnet`, account = the name) when the settings are saved; the field then reads
*Stored in your keychain* and stays empty. Mind that the shadNet protocol itself is unencrypted
(see *What online play exposes* in the README): use a password you use nowhere else.

From Terminal instead (the command prompts for the password; nothing is echoed):

```sh
python3 scripts/online.py keychain shadnet --npid YourOnlineId --ini ~/Library/Application\ Support/Bloodborne/bbport.ini
```

That writes `shadnet_npid=YourOnlineId` and `shadnet_password=keychain`. Set
`online_pool=shadnet` (a file with only a shadNet account and no pool line is taken as shadNet
too).

### The connection

The **Connection** group of the same tab has the UPnP switch (the module asks the router to
forward the co-op port while the game runs) and the co-op UDP port (3658, the PS4's, by
default; 0 picks any free port).

## 4. Launch and check

Start the game as usual (the app, or from Terminal):

```sh
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

The log (the Terminal, or `~/Library/Logs/Bloodborne/game.log` from the app) shows, early on:

- `Co-op: The Hunter's Dream as <name>` or `Online: module .../libbbnet.dylib` and
  `Logged in npid=<name>` for shadNet;
- `Co-op: CUSA03173, NP communication id NPWR05818_00; online play on`.

At the title screen choose to play online. The Hunter's Dream welcome message means the game
server accepted you; notes and bloodstains appear once your character has the Notebook.

For bells: set the same **Network Settings → password** as the people you play with, stand in
the same area, and ring the Beckoning Bell (host) or the Small Resonant Bell (guest). On The
Hunter's Dream pool a host picks up a sign within a minute or so; bbhost notes that about one
in four first summons fails and has to be rung again.

## 5. If something does not work

Run with the network libraries' log and keep it in a file:

```sh
BB_GPU_LOG=debug BB_GAME_DIR=/path/to/CUSA03173 bash run.sh 2>&1 | tee ~/Library/Logs/Bloodborne/online.log
```

| In the log | Meaning | What to do |
|---|---|---|
| `Online: not signed in to The Hunter's Dream` | no `thd_online_id` / `thd_token` | run the `signin` command above |
| `Online: no keychain item bbport-... for <name>` | the ini says `keychain` but nothing is stored | run the `keychain` or `signin --keychain` command again |
| `Online: ... bbport.ini holds a password or token other users of this Mac can read` | a secret in clear, file not owner-only | `chmod 600` the file, or move the secret to the keychain |
| `HandleLoginReply: Login rejected error code 8` | wrong shadNet password (7: unknown account) | store it again |
| `the shadNet server rejected this client` | the server refuses this build's version string | `BB_SHADNET_CLIENT_VERSION` in `launch.conf`; see the README |
| sign posted (`summon_messenger/create ... status=200`) but never requested | the host cannot see it: other pool, other password, or far away | compare pool and password with the host |
| `Handshake[...] SEND kind=1` repeating, then `DEAD (errorCode=0x80552711)` (shadNet pool) | your offers never get an answer: a router in the way | switch to The Hunter's Dream pool |
| `SSL connection failed` now and then | a historical report from the game's HTTPS calls; not reproduced in the current public-endpoint check | preserve the error log for a gameplay reproduction; see [verification results and limits](HTTPS_VERIFICATION.md) |

`online=0` (or removing the line, or **Play online** off in the launcher) returns the game to
offline play; `upnp=0` stops the port mapping; `BB_DISABLE_HTTPS=1` turns certificate checks
off (only for debugging).

## Settings reference

The launcher's **Online** tab writes the first eight rows; the rest are for `bbport.ini` or
`launch.conf` by hand.

| bbport.ini | Environment | Meaning |
|---|---|---|
| `online=1` | `BB_ONLINE=1` | load the online module |
| `online_pool=huntersdream` / `shadnet` | `BB_NET_BACKEND=thd` | which co-op pool (The Hunter's Dream by default) |
| `thd_online_id=`, `thd_token=` | `BB_THD_ONLINE_ID`, `BB_THD_TOKEN` | The Hunter's Dream account (`keychain` = from the login keychain) |
| `shadnet_npid=`, `shadnet_password=` | `BB_SHADNET_NPID`, `BB_SHADNET_PASSWORD` | shadNet account (`keychain` as above) |
| `shadnet_server=host:port` | `BB_SHADNET_SERVER` | another shadNet server (default `srv.shadps4.net:31313`) |
| `community_server=url` | `BB_COMMUNITY_SERVER` | another game server (default `https://thehuntersdream.com`) |
| `p2p_port=3658` | `BB_P2P_PORT` | the UDP port for co-op traffic (`0` = any free port) |
| `upnp=0` | `BB_UPNP=0` | do not ask the router to forward the port |
| | `BB_CLIENT_VERSION` | the client name shown to The Hunter's Dream (`bbport-mac <latest mac-v tag>` by default) |
| | `BB_GPU_LOG=debug` | the network libraries' log |

Values set in the environment (`launch.conf`, or on the command line) win over the file.
