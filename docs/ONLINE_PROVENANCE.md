# Online module sources and redistribution notices

`libbbnet.dylib` is optional and runs in the x86-64 game process. It is loaded only with
`BB_ONLINE=1`; the arm64 GPU host does not load it. The release packager includes the module
when built, checks its library dependencies, and copies the notices below into the app.

| Component | Source used by PR #9 | License notice |
|---|---|---|
| Network, HTTP/SSL, NP and shadNet libraries | [shadPS4 v0.20.0](https://github.com/shadps4-emu/shadPS4/tree/529ceb32), with local adaptations marked `bbport` | GPL-2.0-or-later; source file headers and repository `LICENSE` |
| The Hunter's Dream backend (`gpu/bbnet/thd`) | Adapted in PR #9 from [droogie/bbhost](https://github.com/droogie/bbhost/tree/566184a), through Supermedo's Windows port; individual files identify the original paths | GPL-3.0-or-later; `LICENSES/GPL-3.0-or-later.txt` |
| cpp-httplib 0.45.0 | [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib); vendored header and license in `gpu/third_party/httplib` | MIT |
| protobuf 21.12 | `f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c` | `deps/src/protobuf/LICENSE` |
| OpenSSL 3.5.9 | `45e844fa2a14ec92d146bd8f5778ac130b6625fb` | `deps/src/openssl/LICENSE.txt` |
| miniupnpc 2.3.3 | `bf4215a7574f88aa55859db9db00e3ae58cf42d6` | `deps/src/miniupnp/miniupnpc/LICENSE` |
| nlohmann/json 3.12.0 | `55f93686c01528224f448c19128836e7df245f72` | `deps/src/json/LICENSE.MIT` |

The dependency revisions above are pinned by `scripts/macos/bootstrap_deps.sh`. Protobuf,
OpenSSL and miniupnpc are linked statically; json and cpp-httplib are headers compiled into
the module. zlib and Apple's Security/CoreFoundation frameworks are system dependencies.
The module also links `libbbgpu.dylib`; its existing notices are collected by the packager.

The combined online module includes GPL-3.0-or-later code. Preserve that notice and the
corresponding source, including local changes and build scripts, when distributing it.
This file records source provenance and the notices packaged with this implementation.

The upstream PR remains [Wooloomooloo2/bloodborne_mac_port#9](https://github.com/Wooloomooloo2/bloodborne_mac_port/pull/9).
