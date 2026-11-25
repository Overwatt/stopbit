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

## Web pages

| Page | |
|---|---|
| Status | a verdict, then every part with its state; a port opens beside it as a sentence, ten minutes of traffic and its readings, with its settings one press away |
| Network | address |
| MQTT | Home Assistant and the broker |
| Hardware | chips found, processor temperature and supply, pin map, memory; device name, restart, password |
| Firmware | upload an `app.bin`, slots, starts |

`GET /api/history/1` to `/4` is that port's last ten minutes in 5 s steps (bytes heard and
sent, errors), kept in memory and lost on restart. Noise is not counted as an error.

The Hardware page reads `GET /api/hardware`, which like `/api/state` needs no password. The
temperature is the processor's own sensor, so it reads above room temperature.

## Updating from the web page

Choose `app.bin` on the Firmware page. The page checks the file, uploads it signed with
the web password's key, waits for the restart, and keeps the new firmware once it answers.
If it does not start, the loader goes back to the previous one. An image that starts but is
never kept is given up after three 3-minute trials. The upload goes into spare RAM and is
checked before flash is touched, and never replaces the running slot.

**So the web password can install firmware, which is full control of the gateway.** Loader
updates and flash dumps still need the update key. `POST /install` and `POST /confirm` are
the endpoints; `tests/test_install.py HOST` checks the refusals.

### Without Home Assistant

Turn off **Announce to Home Assistant** (`mqtt.announce=0`) to publish only the data. The
discovery entries are withdrawn once; everything else stays the same, retained:
`HOST/port1` to `HOST/port4` and `HOST/device` (JSON), and `HOST/status` (`online`, or
`offline` from the last will). With changes allowed, `HOST/port1/set/KEY` takes any port
setting except `listen_only`. Firmware older than this ignores the setting and announces.

### Being told when a bus goes quiet

Each port has an alert sensor in Home Assistant (`binary_sensor`, on = problem). The gateway
cannot report its own loss of power, network or broker: then the sensor becomes
**unavailable**, not on. A notification should watch for both. The wait skips restarts,
which take about ten seconds.

```yaml
triggers:
  - trigger: state
    entity_id: binary_sensor.n540_gateway_port_1_alert   # yours may differ
    to: "on"
  - trigger: state
    entity_id: binary_sensor.n540_gateway_port_1_alert
    to: "unavailable"
    for: "00:05:00"
actions:
  - action: notify.notify
    data:
      message: "Port 1: {{ 'problem' if trigger.to_state.state == 'on' else 'gateway unreachable' }}"
```

If Home Assistant or the broker loses power in the same outage, nothing can be sent until
it is back.

## Reload button

Hold 3 to 15 seconds and release: the network goes back to DHCP, the web password is
cleared, and the gateway restarts. Ports, names and MQTT stay. The work light blinks fast
while releasing would act; holding past 15 seconds cancels. Set a new password with
`tools/n540.py HOST passwd`.

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
| quiet_min | alert when nothing is heard, or no client is connected (TCP), for this many minutes: 0 (never), 1, 2, 3, 5, 10, 15, 20, 30, 60, 120, 180, 240, 360, 720 or 1440. Shown on the page, in `port N`, and as a problem sensor in Home Assistant. Firmware older than this ignores it |
| listen_only | never transmits: the transmitter is off and the direction pin stays low; data from the network is discarded, `send` and `wiretest` are refused. Not settable over MQTT |

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

- With controls: 84 entities, 64 enabled by default. Less common ones (packet timing,
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
cc -DD2_BSS= -Isrc/app tests/test_history.c src/app/history.c -o /tmp/h && /tmp/h
cc -Isrc/app tests/test_quiet.c -o /tmp/q && /tmp/q
cc -Isrc/app tests/test_button.c src/app/{button,cfg}.c -o /tmp/b && /tmp/b
python3 tests/test_http.py HOST
python3 tests/test_net_trial.py HOST GATEWAY
uv run --with paho-mqtt tests/test_mqtt.py HOST SSH_HOST BROKER_IP MOSQ_DIR out.json
uv run --python 3.14 --with homeassistant --with paho-mqtt tests/check_ha.py out.json
```

The device tests use on-chip loopback. On the real transceivers, each port was tested
with an FTDI USB-RS485 adapter through its TCP server: random data both ways and 50
request/reply rounds at 9600 8N1, 19200 8E1, 38400 8O1, 115200 8N1 and 8N2, 460800 and
1000000 8N1, with no errors. Not tested: several ports busy at once, long or terminated
buses, and how fast the gateway turns the bus around (the adapter's 16 ms latency hides it).
