# HTTPS

The gateway serves the same pages and API on port 443, with a key that is made on it and
never leaves. Renewing a Let's Encrypt certificate by itself is not built yet.

## Use

Network page, or:

```sh
tools/n540.py HOST https on                          # self-signed until you install one
tools/n540.py HOST cert-request gateway.example.com request.pem
#   have request.pem signed, e.g. with an ACME client that takes a signing request
tools/n540.py HOST cert-install fullchain.pem
tools/n540.py HOST https                             # state, fingerprint, timings
```

- **Port 80 keeps working**, by address and by `.local` name, so a bad or expired
  certificate cannot lock anyone out. Only the certificate's own name is redirected, and
  with a 302. There is no HSTS. **So anyone on the network can still use plain HTTP**, and
  the web password's protection there is as before.
- The loader stays HTTP. The ports' data sockets and MQTT are not encrypted by this.
- Off by default; nothing is set up until it is turned on. `forget` discards the keys.
- A certificate that is for another key or another name is refused.

## How it works

mbedTLS 3.6.8, TLS 1.2 only, one suite: ECDHE-ECDSA P-256 with AES-128-GCM. Sessions resume
(tickets, keys changed every 12 hours and at every start). Up to 4 connections, kept open
between requests for a minute; idle ones give way to new ones.

- **Handshakes do not hold up the ports.** The arithmetic runs on its own stack in slices of
  1 ms between passes of the main loop (`src/app/worker.c`). It is cooperative: the calls it
  spends its time in are renamed at build time to ones that yield (`CMakeLists.txt`).
- **Records** are handled in the main loop, pieces of an upload as fast as flash takes them.
- **Storage:** keys and certificate in two copies at `0x0F4000` / `0x0F6000` of external
  flash 1. The flash is not encrypted: whoever holds the board has the key. `/flash` in the
  app blanks that range; the loader's does not, so treat loader dumps as secret.
- The data cache is on since this was added. Only the Ethernet controller uses DMA, and its
  memory is marked uncacheable.

| | Measured |
|---|---|
| Image | 223 KB, from 139 KB |
| Memory, 4 connections and a handshake | 87 KB of 112 KB |
| Emulated Cortex-M7: longest slice | 1.01 ms |
| On the gateway | **not yet**: see `https` and Longest pause on the Hardware page |

## Tests

`tests/sim/run.sh`, then `tests/test_tls.py 127.0.0.1:8541 8543`: versions and suites,
keep-alive, pipelining, resumption, more clients than places, a handshake cut at every
stage, garbage, uploads, certificates from an RSA and P-384 authority, restarts, and after
each that no memory or connection is left behind. `SIM_FLAGS=-DWORKER_SLICE_US=20u` makes
the worker switch fifty times as often.

## Not built: Let's Encrypt on the gateway

Checked 2026-11-25:

- The gateway is not reachable from the internet, so use **DNS-01**. On Route 53, IAM can
  limit a key to one record: allow `ChangeResourceRecordSets` with conditions on the record
  name (`_acme-challenge.<name>`), type `TXT` and action. Request signing is HMAC-SHA256,
  which we have.
- **DNS-PERSIST-01** would need no DNS key at all, one permanent TXT record instead. As of
  October 2026 Let's Encrypt had not announced a production date. Check again first.
- Lifetimes are 90 days, 64 from 2027-02-10 and 45 from 2028-02-16. Renew at two thirds of
  the lifetime, or follow ACME Renewal Information.
- Needed besides: network time (SNTP), a TLS client that checks certificates, and the ACME
  and Route 53 requests. The certificate's expiry should go to the Hardware page and Home
  Assistant.
- **RAM:** 375 of 384 KB is spoken for, so it needs room made, e.g. by fewer connections.
- Certificates are publicly logged, so the host name becomes public. The name needs a DNS
  record for the gateway's LAN address; some routers block public names that resolve to
  private addresses.

Open: the host name, and whether a restricted DNS key may live on the gateway. If not,
renewal runs on another computer, which only needs the signing request once.
