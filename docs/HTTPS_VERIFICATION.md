# HTTPS verification, 2026-10-25 and 2026-10-26

The intermittent `SSL connection failed` report in [PR #9](https://github.com/Wooloomooloo2/bloodborne_mac_port/pull/9)
was **not reproduced** in the checks below. These include public-endpoint probes and a prior
authenticated gameplay smoke; they do not establish a fix for the reported failure.

## Method

Tested source revision `1f3bf420e16d0ec2b2320ed1f1adf60e5f7a9fcd` on macOS 27.0, using the
built x86-64 `libbbnet.dylib`, cpp-httplib 0.45.0 and pinned OpenSSL 3.5.9.

A standalone C probe linked the real runtime, initialized the online module and called its
registered `sceHttp` imports: context/template creation, connection/request creation,
`sceHttpSendRequest`, response status and last-error retrieval, and object teardown. Each
request ran through the module's normal asynchronous worker and created a fresh TLS client,
as the game HTTP implementation currently does. No GPU host or game was launched.

The probe requested only `GET /`, with redirects disabled, through the normal
`*.scej-network.jp` host override to `thehuntersdream.com`. It used the distinct User-Agent
`bbport-mac/0.6.0-https-check`, removed account IDs/tokens/passwords, kept HTTPS verification
enabled and named `/etc/ssl/cert.pem` as the certificate bundle, as `run.sh` does. Requests were
serial, with a 750 ms pause after each completed request and 8-second timeouts.

The custom-CA cases called `sceHttpsLoadCert` to exercise the flag that the game sets. That
import currently records the flag rather than parsing the supplied certificates. The request
worker explicitly logged verification against system roots for those cases.

## Results

| Endpoint | Custom-CA flag | Requests | HTTP result | Transport/TLS errors |
|---|---|---:|---|---:|
| `https://thehuntersdream.com:18671/` | Set | 18 | 307 | 0 |
| `https://thehuntersdream.com:18671/` | Unset | 12 | 307 | 0 |
| `https://thehuntersdream.com/` | Set | 6 | 200 | 0 |

All 36 requests completed with zero game HTTP transport error. Request times were
0.531–0.773 seconds, median 0.566 seconds. The 307 responses are HTTP redirects received after
a successful TLS handshake; the probe deliberately did not follow them. Independent curl
requests to both ports also completed certificate verification with result 0.

The probe source, per-request logs and JSON summary are retained locally under
`out/https-investigation/`. They contain no player credentials or game data.

## Limits of the serial probe

No signed-in game session, gameplay traffic, POST payloads or simultaneous request load was
tested. The earlier report may depend on those conditions, network/server state or the earlier
build. These results do not establish that the historical failure is fixed, or identify its
cause. Authenticated gameplay needs a separate reproduction before making either claim.

## Prior authenticated gameplay smoke, 2026-10-25

A later run at `9a23eaf73e8d8684198179b3a3e68911450cb6ce` signed in to The Hunter's Dream,
entered fresh-character gameplay, returned to the title, reconnected, and loaded that character
online. The retained redacted log records 37 HTTP 200 responses, including two
`POST /basic_utils/login` calls, with zero observed TLS or game HTTP failures. Certificate
verification was enabled. Evidence is retained under `out/ingame-online-2026-10-25/`.

This is evidence from the earlier revision, not a new gameplay test of the 2026-10-26 changes.
It did not measure peak concurrent requests or exercise a second-player summon, co-op or
invasion. A successful bounded run does not rule out an intermittent failure.

## Parallel request follow-up, 2026-10-26

The [reviewer's reproduction details](https://github.com/Wooloomooloo2/bloodborne_mac_port/pull/78#issuecomment-6404613501)
identify a different load: signed-in gameplay with several game HTTP workers active, roughly
one failure in six calls (`cpp-httplib error 8`, `SSL connection failed`), including two
`basic_utils/login` calls. The serial unauthenticated probe above does not reproduce those
conditions.

The follow-up probe used the rebuilt x86-64 `libbbnet.dylib` with the session-client changes in
this review. It dispatched four `sceHttpSendRequest` calls before awaiting any response through
`sceHttpGetStatusCode`, exercising the module's concurrent request workers. Each batch used one
HTTP context/template and separate connection/request objects. Settings otherwise matched the
serial probe: unauthenticated `GET /`, host override to port 18671, redirects off, eight-second
timeouts, certificate verification enabled, and a 750 ms pause between batches.

| Endpoint | Custom-CA flag | Requests | Concurrent workers | HTTP result | Transport/TLS errors |
|---|---|---:|---:|---|---:|
| `https://thehuntersdream.com:18671/` | Set | 24 | 4 | 307 | 0 |
| `https://thehuntersdream.com:18671/` | Unset | 24 | 4 | 307 | 0 |

All 48 requests received an HTTP response without a transport/TLS error. The probe source and
logs are retained under `out/https-investigation/parallel-probe.c` and
`out/https-investigation/game-http-parallel-*.log`. Account credentials were removed before
module initialization; no game was launched for this follow-up.

Concurrent TLS handshakes alone did not reproduce the failure in this sample. Signed-in
parallel gameplay traffic, including the actual login payloads and timing, still needs a
reproduction. This does not identify the cause or establish a fix. The THD action-client
keep-alive change affects a separate HTTP path from the game's `sceHttp` workers.
