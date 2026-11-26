# MQTT over TLS

Approved 2026-11-26. The broker (`mqtt.example.net`, 10.20.1.219) listens on 1883 and
9001 only; it gets a TLS listener with a Let's Encrypt certificate for its name.

## Goal

The gateway's MQTT connection, password included, encrypted, to a broker whose certificate
chains to a root the gateway already trusts (`roots.c`: ISRG X1/X2, Amazon) and names the
host it was told to use. Plain MQTT stays available and stays the default.

## Design

**Where TLS goes.** lwIP's MQTT client already asks for `altcp_tls_new(config, ...)` when
`mqtt_connect_client_info_t.tls_config` is set. The gateway supplies that function
(`src/app/mqtt_tls.c`), so lwIP's MQTT code is unchanged. `LWIP_ALTCP` and
`LWIP_ALTCP_TLS` on, application only; the loader builds as before.

- The TLS layer wraps an `altcp_tcp` connection. While it is open the plain side is
  hidden from MQTT: `connected` is reported only after the handshake.
- **Handshake** on a third job (`WORKER_MQTT`, 6 KB stack in D3), in 1 ms slices like
  HTTPS, so the ports never wait. A job still running when MQTT gives up is told to stop,
  and the connection is freed once it has.
- **Records** in the main loop. A write is one record: `sndbuf` reports the room left
  after a record's overhead, and is 0 while a record is still going out, so a write
  never half succeeds. Received records are decrypted as they come and passed up as
  plaintext.
- **Trust:** `MBEDTLS_SSL_VERIFY_REQUIRED`, roots from `roots.c`, host name checked.
  ECDHE with AES-128-GCM, TLS 1.2, as the renewal client.
- **Resumption:** the session (ticket) is kept and offered on the next connection to the
  same host, so a dropped connection comes back in milliseconds.
- **Never in the clear:** a failed check is a failed connection, with the reason
  ("its certificate is for another name", "...has expired", "...is not trusted",
  "the time is not known yet"), shown on the MQTT page, logged once (EV_MQTT_DOWN),
  retried with the usual backoff.
- **Time:** the clock carried over a restart is enough (`clock_known`); after a power
  cut it waits for network time.

**Setting.** `mqtt.tls` = 0/1, kept in the extra settings record (`more_cfg_t`, from
`spare`), so older firmware never sees it. With TLS on the host must be a name, not an
address. Not settable over MQTT. The page offers 8883 when TLS is turned on and 1883
when it is turned off, if the port was the other default.

**Status.** `/api/state` `mqtt` gains `tls` (on), and when connected the certificate's
expiry; the MQTT page says "Encrypted (TLS 1.2), certificate valid until ...".

## Cost

About 22 KB of the TLS heap while connected (212 KB; most used so far 106 KB). A 6 KB job
stack in D3 (34 KB free). About 6 KB of code (image 274 of 384 KB).

## Tests

- **Host:** `tests/test_mqtt_tls.c`: lwIP's real MQTT client and altcp, the new layer, the
  real worker and mbedTLS, over a socket to Mosquitto (TLS, test CA made by the test).
  Connect, publish, subscribe and receive; wrong name, untrusted, expired, time unknown;
  broker restart (resumed session); a large retained message. In CI (Mosquitto from apt).
- **Settings:** `mqtt.tls` in the settings tests and the settings fuzzer.
- **Gateway:** after the broker has its listener: switch, check MQTT and Home Assistant,
  TigoBus untouched.

## Broker side

A certificate for `mqtt.example.net` by DNS check (certbot with its Route 53
plugin), using an AWS key limited to `_acme-challenge.mqtt.example.net` (policy shown
before it is made), and a Mosquitto listener on 8883 with it. 1883 stays until switched.
