# Gateway: web UI, RS485 bridge, configuration

Open `http://n540-a1b2c3.local/`. Anyone can view it; changing settings needs the
web password, which is unset at first:

```sh
tools/n540.py n540-a1b2c3.local passwd
```

The same settings from the command line (signed with the update key):

```sh
tools/n540.py $H config
tools/n540.py $H set port1.enabled=1 port1.baud=19200 port1.parity=even
tools/n540.py $H state
```

## Ports

All four ship disabled. Each one is a TCP server (default 4001-4004, up to 4
connections), a TCP client that reconnects, or UDP. **Data ports have no password.**

| Setting | Values |
|---|---|
| baud | 300 to 2000000 |
| data_bits, parity, stop_bits | 7/8, none/even/odd, 1/2 (not 7N1) |
| gap_ms | silence that ends a packet; 0 = 3.5 characters, at least 1.75 ms |
| packet_len | 1 to 1400 bytes |
| echo_cancel | drop bytes received while sending |
| loopback | transmit to receive inside the chip (still drives the bus) |

A slow serial line pushes back on TCP senders. A network client that stops reading
loses packets (counted as "not delivered") without affecting the others.

## Network changes

New network settings run on trial for 2 minutes and revert unless kept (button in
the UI, or `n540.py $H keep`). The loader always uses DHCP.

## Security

Requests are signed with an HMAC over a single-use nonce, the path and the body.
The browser derives its key from the password (PBKDF2, 30000 rounds). The password
never crosses the wire, but pages and data are not encrypted, so a listener could
attempt to guess a weak password offline. The web password can change settings and restart; it
cannot install firmware, read flash, or enter the loader.

## Storage

Settings: two alternating checksummed copies at `0x0F0000` / `0x0F1000` of external
flash 1 (blank on the stock unit).

## Tests

```sh
cc -Isrc/app tests/test_cfg.c src/app/cfg.c -o /tmp/t && /tmp/t   # host
python3 tests/test_device.py HOST           # nothing attached to the RS485 ports
python3 tests/test_http.py HOST
python3 tests/test_net_trial.py HOST GATEWAY
```

The device tests use on-chip loopback. **Nothing has been tested through the RS485
transceivers yet**: port order, the UART mapping and direction control are unproven.
With loopback off, transmitting on each port is heard by none of the four receivers,
which fits automatic direction control but also fits several wrong mappings.
