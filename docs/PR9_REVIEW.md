# PR #9 local rebase and review, 2026-10-25

Reviewed [Wooloomooloo2/bloodborne_mac_port#9](https://github.com/Wooloomooloo2/bloodborne_mac_port/pull/9)
at `e98b7b90fb7042e63977107440d319f4355a55ee`, against master
`ca1bf166c862f510b57c64b642261626223105c2`. Both upstream refs were rechecked after validation.

The local branch is `codex/pr9-online-review`, in `.worktrees/pr9-online`. It was rebased with
`git rebase --rebase-merges origin/master`. Two launcher conflicts kept both the online state
and master's benchmark/mod initialization and tests. Before the fixes below, its tree differed
from the original PR only by master's 16:10 changes. Original PR commit authors and merge
history are retained. No remote branch, PR description or comments were changed.

## Fixes and improvements

- **Duplicate operations after a lost response:** the THD HTTP client retried every POST on a
  reused connection after a read/write error. A server could already have created or joined a
  room. Only GETs receive that transport retry now. A loopback fixture executes a POST and
  drops the response; it reproduces two executions on the original code and one on this branch.
- **Overlapping pollers after a context restart:** detached poll/keepalive workers shared a
  restartable flag. Stop now cancels requests and joins both workers before Start can run.
  Context teardown releases the matching mutex before waiting, since the event handler needs
  it. Client replacement/destruction is synchronized with cancellation, and cancellation is
  repeated while the worker exits to cover connection-setup races. Exit cleanup stops workers.
- **Empty-poll load:** an immediate empty response advertising long polling could create a
  tight request loop. Empty replies are capped at the legacy four requests per second; a
  server-held response is polled again immediately and events still drain immediately.
  `WaitMaxMs` is clamped in 64-bit width before converting to the timeout type.
- **Release loading and notices:** the packager includes the x86-64 `libbbnet.dylib` when built,
  validates its bundled/system dependencies, signs it, and collects GPL-3 text, cpp-httplib and
  the pinned online dependency notices. The runtime finds it in `Contents/Frameworks`; checkout
  and explicit `BB_NET_MODULE` paths still work. See [provenance](ONLINE_PROVENANCE.md).
- **Offline rebuilds:** `BB_NET=OFF` or missing dependencies remove a previously built module,
  so a stale file cannot silently enable it or enter a later package.
- **Release identity:** a packaged version file supplies `bbport-mac <release>` when Git is
  absent. `BB_CLIENT_VERSION` still overrides it.

## Validation

- `BB_LTO=OFF bash build_macos.sh`: x86-64 GPU library, online module and runtime, plus arm64
  GPU library and both native host executables, built successfully.
- Focused C runtime tests passed: pad, runtime, file overlays, semaphore, content, online import
  routing. The x86-64 P2P codec and new THD loopback session tests passed.
- The session fixture checks response-loss retries, timeout bounds, empty-poll pacing, waiting
  for a running handler, held-poll cancellation, five held-request restarts and twenty immediate
  start/stop races. It uses a loopback server, fake credentials, and no public service.
- Four x86-64 loader tests passed: checkout layout, app layout with spaces, explicit/missing
  override, offline and absent-module fallback.
- The launcher and its model tests built and passed; shell launch-settings tests passed.
- Python discovery: **95 tests, 87 passed, 8 failed**. The same seven `test_probe.py` failures
  reproduce with a separately compiled master runtime and the same native host. The eighth,
  `test_run_settings.py`'s 1080p restart expectation, reproduces with unchanged master scripts.
- Built `dist/Bloodborne.app` and `dist/Bloodborne-macOS-0.6.0-pr9-review.zip`. Packaging's
  dependency checks and `codesign --verify --deep --strict` passed. A runtime import test loaded
  the actual bundled module and verified online imports while trophies remained local.
- Built with `BB_NET=OFF`, checked that the module was removed, then rebuilt with `BB_NET=ON`.
- Shell syntax and `git -c core.whitespace=cr-at-eol diff --check` passed. The two edited THD
  source files retain their original CRLF line endings.

## Remaining verification

Live sign-in and Mac–Windows co-op with the distinct `bbport-mac` identity still need real
sessions. The historical intermittent HTTPS failure did not reproduce in a subsequent
36-request check through the actual x86-64 game HTTP module, including the custom-CA flag;
see [HTTPS verification](HTTPS_VERIFICATION.md) for results and limits. Authenticated gameplay
was not tested, so this is not a confirmed fix for the earlier report. The PR discussion records prior
co-op success, but that does not verify this local rebase. No gameplay or FPS improvement is
claimed from these network changes. No game files or player credentials were used.

The review app is a local artifact, built with LTO disabled. Its source is this local branch
until the changes are published; it is not a production release.
