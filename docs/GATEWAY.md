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

## Console

```sh
tools/n540.py $H                     # interactive: history, tab completion
tools/n540.py $H ports               # or one command at a time
```

Set `N540_HOST` to leave the host out. `help` lists the commands. The console runs on
the gateway and is reached through signed requests, so there is no telnet port.

| Command | |
|---|---|
| `status`, `ports`, `port 1`, `resets` | look |
| `get port1`, `set port1.baud=19200 port1.parity=even` | settings; a `set` is all or nothing |
| `enable 1`, `disable 1`, `keep` | |
| `send 1 "AT\r"`, `send 1 hex 01 03 00 00` | send on the line |
| `log 1` | last 16 packets on a port, first 24 bytes of each |
| `wiretest yes` | send on each port, report who hears it (disconnect equipment first) |
| `watch 1`, `connect 1` | follow traffic live; talk to a TCP-server port |
| `save FILE`, `load FILE` | all settings except passwords |

`watch` polls twice a second and can miss packets on a busy line.

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

## Home Assistant (MQTT)

Off by default. In the web UI enter the broker and tick **Connect to an MQTT broker**
(`.local` names work). The gateway then appears in Home Assistant as one device.
Tick **Let Home Assistant change settings** for controls as well as sensors.

```sh
tools/n540.py $H set mqtt.enabled=1 mqtt.host=homeassistant.local mqtt.user=n540 mqtt.control=1
tools/n540.py $H mqtt-passwd
```

- With controls: 80 entities, 60 enabled by default. Less common ones (packet timing,
  echo, loopback, port name) are there but disabled.
- A refused change shows under "Last rejected change" and the control goes back.
  Order matters: 7 data bits needs parity or 2 stop bits first.
- Commands reach port settings, the device name and restart. Network, MQTT and
  password settings are not reachable. Retained commands are ignored.
- **Anyone who can publish to the broker can use the controls.** The connection,
  including the broker password, is not encrypted. Use a dedicated broker user.
- Settings are written to flash (about 100k cycles per sector): don't automate
  settings changes every few seconds.
- Turning MQTT off removes the entities from HA.

Serial data does not go over MQTT; use the TCP/UDP ports for that.


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
flash 1 (blank on the stock unit). Schema 2 (MQTT) reads schema 1. Firmware from before
MQTT cannot read schema 2 and would start with defaults.

## Tests

```sh
cc -Isrc/app tests/test_cfg.c src/app/cfg.c -o /tmp/t && /tmp/t   # host
cc -DD2_BSS= -Isrc/app tests/test_cli.c src/app/{cli,cfg,traffic}.c -o /tmp/c && /tmp/c
python3 tests/test_cli_device.py HOST       # nothing attached to the RS485 ports
python3 tests/test_device.py HOST           # nothing attached to the RS485 ports
python3 tests/test_http.py HOST
python3 tests/test_net_trial.py HOST GATEWAY
uv run --with paho-mqtt tests/test_mqtt.py HOST SSH_HOST BROKER_IP MOSQ_DIR out.json
uv run --python 3.14 --with homeassistant --with paho-mqtt tests/check_ha.py out.json
```

The device tests use on-chip loopback. **Nothing has been tested through the RS485
transceivers yet**: port order, the UART mapping and direction control are unproven.
With loopback off, transmitting on each port is heard by none of the four receivers,
which fits automatic direction control but also fits several wrong mappings.
