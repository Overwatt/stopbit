# HTTPS with a Let's Encrypt certificate (stretch goal)

Not built. This records the design and what was checked on 2026-11-25, so it can be
picked up later. Goal: the gateway serves HTTPS itself, with a certificate browsers
trust, and renews it unattended. No reverse proxy.

## Does it fit?

| | Have | Need |
|---|---|---|
| AXI SRAM (code and data) | 384 KB, about 270 KB free | estimate: mbedTLS 100-150 KB of code, about 40 KB per connection |
| DTCM, D2 | about 28 KB and 16 KB free, part of DTCM being the stack | little; TLS buffers belong in AXI |
| Random numbers | hardware RNG, already used | same |
| Clock | none | network time (SNTP), only for renewal and expiry |

So two connections should fit and three is tight. **Handshake time is unknown**: no
published figure for this core was found, and the D-cache is off. Measure first.

## Design

1. **Private key made on the device** (P-256) and never sent over the network; only a
   signing request leaves. Do not copy a wildcard key onto it: the flash is not
   encrypted, so whoever holds the board could impersonate every host it covers. Keep
   the key out of `/flash` dumps or treat dumps as secret.
2. **HTTPS on 443**; the web page redirects there once a certificate is installed.
3. **Signed updates stay on port 80**, and the loader stays HTTP. A bad or expired
   certificate must never lock anyone out.
4. **Keep-alive first.** The web server closes after every response, so each poll of the
   page would cost a handshake. Add keep-alive and TLS session resumption.
5. The certificate's expiry goes on the Hardware page and to Home Assistant.

## Getting and renewing the certificate

- The gateway is not reachable from the internet, so HTTP-01 and TLS-ALPN-01 would mean
  opening ports. Use **DNS-01**.
- On Route 53, IAM can limit a key to one record: allow `ChangeResourceRecordSets` with
  conditions on the record name (`_acme-challenge.<name>`), type `TXT` and action. Request
  signing is HMAC-SHA256, which we already have.
- **DNS-PERSIST-01** would need no DNS key at all, one permanent TXT record instead. As of
  October 2026 Let's Encrypt had not announced a production date. Check again first.
- Lifetimes are 90 days, 64 from 2027-02-10 and 45 from 2028-02-16, so renewal must be
  automatic. Renew at two thirds of the lifetime, or follow ACME Renewal Information.
- Certificates are publicly logged, so the host name becomes public.
- The name needs a DNS record for the gateway's LAN address. Some routers block public
  names that resolve to private addresses.

## Steps, each usable alone

1. Trial build: mbedTLS in the app, measure handshake time and memory.
2. HTTPS with a certificate requested from a computer (`tools/n540.py HOST cert ...`).
3. Renewal on the device: SNTP, ACME client, Route 53 (or DNS-PERSIST-01).

Open decisions: the host name, and whether a restricted DNS key may live on the gateway.
If not, renewal runs from another computer and only pushes the certificate.
