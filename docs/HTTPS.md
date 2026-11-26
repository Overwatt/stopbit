# HTTPS

The gateway serves the same pages and API on port 443, with a key that is made on it and
never leaves, and can renew a Let's Encrypt certificate by itself (see Renewing).

## Use

Network page, or:

```sh
tools/n540.py HOST https on                          # self-signed until you install one
tools/n540.py HOST cert-request gateway.example.com request.pem
#   have request.pem signed, e.g. with an ACME client that takes a signing request
tools/n540.py HOST cert-install fullchain.pem
tools/n540.py HOST https                             # state, fingerprint, timings
```

Or a Let's Encrypt certificate made on another computer, with Route 53 credentials in the
usual places, `--staging` first. Then repeat it before it expires.

```sh
uvx --from certbot --with certbot-dns-route53 certbot certonly --dns-route53 --csr request.pem \
  --config-dir le/c --work-dir le/w --logs-dir le/l --fullchain-path fullchain.pem \
  --cert-path cert.pem --chain-path chain.pem
```

Asking the gateway for another request makes another key: install the certificate first.

- **Port 80 keeps working**, by address and by `.local` name, so a bad or expired
  certificate cannot lock anyone out. Only the certificate's own name is redirected, and
  with a 302. There is no HSTS. **So anyone on the network can still use plain HTTP**, and
  the web password's protection there is as before.
- The loader stays HTTP. The ports' data sockets and MQTT are not encrypted by this.
- Off by default; nothing is set up until it is turned on. `forget` discards the keys.
- A certificate that is for another key or another name is refused.

## How it works

mbedTLS 4.1.2 (supported to March 2029), TLS 1.2 only, one suite: ECDHE-ECDSA P-256 with
AES-128-GCM. Sessions resume (tickets, keys changed every 12 hours and at every start). Up to 4 connections, kept open
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
| Image | 232 KB, from 139 KB without HTTPS |
| Memory, 4 connections and a handshake | 101 KB of 124 KB |
| Emulated Cortex-M7: longest slice | 1.01 ms |
| Handshake arithmetic, emulated | 28.4 million instructions (26.3 with mbedTLS 3.6) |
| On the gateway: full handshake, resumed, four at once | 91 ms in 67 slices, 4.8 ms, 364 ms (mbedTLS 3.6: 77, 2.2, 305) |
| On the gateway: page (25 KB), status, on an open connection | 13 ms, 1.6 ms |
| On the gateway: longest pause of the main loop meanwhile | 3.9 ms; saves no longer pause the ports |
| On the gateway: longest slice, arithmetic stack | 2.2 ms, 2.2 KB of 12 KB |
| Let's Encrypt's chain, November 2026 | 4 certificates, 3.4 KB; up to 4 KB can be stored and sent |

**On the gateway (2026-11-25):** a Let's Encrypt certificate, curl, Python and Chromium, and
a live bus that lost nothing meanwhile. Not tested there: uploads over HTTPS, expiry, a
certificate replaced while in use, and connections left open for days.

## Tests

`tests/sim/run.sh`, then `tests/test_tls.py 127.0.0.1:8541 8543`: versions and suites,
keep-alive, pipelining, resumption, more clients than places, a handshake cut at every
stage, garbage, uploads, certificates from an RSA and P-384 authority, restarts, and after
each that no memory or connection is left behind. `SIM_FLAGS=-DWORKER_SLICE_US=20u` makes
the worker switch fifty times as often.

## Renewing on the gateway

Network page, Renewal: the Route 53 hosted zone ID, an access key and its secret, and
whether to renew by itself. "Test on staging" goes through every step with Let's Encrypt's
staging service and installs nothing. Due once a third of the certificate's life is left;
checked every 12 hours, and after a failure in an hour, then longer, up to a day.

1. A Let's Encrypt account key, made once and kept (`0x0F8000` / `0x0F9000`, two copies,
   blanked in flash dumps like the secret beside it).
2. An order for the certificate's name; the TXT record `_acme-challenge.<name>` through
   Route 53 (AWS Signature Version 4), until every Route 53 server has it; Let's Encrypt
   checks it; the record is deleted, whatever happened.
3. A new private key on the gateway, its signing request, and the certificate, installed as
   if uploaded.

It runs as a second worker job, so the bus and HTTPS carry on. The time comes from SNTP:
the time server set on the Network page (`net.ntp`), else the DHCP server's, then
`pool.ntp.org`, every hour. A restart carries the time over in backup registers, to within a
few seconds, but renewal waits for a time server: AWS refuses a clock more than 5 minutes
out. The board has no watch crystal, so the RTC could do no better. Outgoing connections check the server
against six roots compiled in (`src/app/roots.c`): ISRG Root X1 and X2, ISRG Root YE and YR
(Let's Encrypt's newer hierarchy, so a server sending only YE1 or YR1 and the like still
verifies), Amazon Root CA 1 and 3. **A root that is withdrawn needs new firmware.**

The AWS key should be able to do nothing else. This is the policy, for one name:

```json
{ "Version": "2012-10-17", "Statement": [
  { "Effect": "Allow", "Action": "route53:ChangeResourceRecordSets",
    "Resource": "arn:aws:route53:::hostedzone/ZONE_ID",
    "Condition": { "ForAllValues:StringEquals": {
      "route53:ChangeResourceRecordSetsNormalizedRecordNames": ["_acme-challenge.NAME"],
      "route53:ChangeResourceRecordSetsRecordTypes": ["TXT"],
      "route53:ChangeResourceRecordSetsActions": ["UPSERT", "DELETE"] } } },
  { "Effect": "Allow", "Action": "route53:GetChange", "Resource": "arn:aws:route53:::change/*" } ] }
```

Checked 2026-11-25: with it, a key cannot list zones, read records, or change any other
name, type or action. Whoever has the board can still read the key from its flash and use
it for that one record, which is enough to get certificates for that name.

**Tested:** full staging runs from the simulator (31 s) and on the gateway (41 s, 2026-11-25),
with TigoBus live: nothing lost. The main loop paused up to 18 ms meanwhile, saving the new
key: the external flash is clocked by hand, and the record was written and read back with
nothing else running. Long writes and reads now let the ports run between pieces: 2.9 ms. Let's Encrypt's lifetimes are getting shorter (64 days from
2027-02-10, 45 from 2028-02-16); renewing at a third of the life follows them.

## Not built

- **DNS-PERSIST-01** would need no DNS key at all, one permanent TXT record instead. As of
  October 2026 Let's Encrypt had not announced a production date. Worth moving to when it is.
- ACME Renewal Information (ARI), which lets Let's Encrypt ask for an early renewal.
- The certificate's expiry on the Hardware page and in Home Assistant.
- Other DNS providers.

Certificates are publicly logged, so the host name becomes public. The name needs a DNS
record for the gateway's LAN address; some routers block public names that resolve to
private addresses.
