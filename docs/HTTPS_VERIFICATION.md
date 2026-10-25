# HTTPS verification, 2026-10-25

The intermittent `SSL connection failed` report in [PR #9](https://github.com/Wooloomooloo2/bloodborne_mac_port/pull/9)
was **not reproduced** in this check. This is a current public-endpoint check, not a verified
fix for the earlier gameplay report.

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

## Limits

No signed-in game session, gameplay traffic, POST payloads or simultaneous request load was
tested. The earlier report may depend on those conditions, network/server state or the earlier
build. These results do not establish that the historical failure is fixed, or identify its
cause. Authenticated gameplay needs a separate reproduction before making either claim.
