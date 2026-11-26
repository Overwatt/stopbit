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
| Hardware | chips found, processor temperature and supply, pin map, memory and shared buffers; device name, restart, password |
| Firmware | upload an `app.bin`, slots, starts |

`GET /api/history/1` to `/4` is that port's last ten minutes in 5 s steps (bytes heard and
sent, errors), kept in memory and lost on restart. Noise is not counted as an error.

The Hardware page reads `GET /api/hardware`, which like `/api/state` needs no password. The
temperature is the processor's own sensor, so it reads above room temperature. `loop` is the
main loop's longest pause, recently (10 to 20 s) and since the start: which step of the pass
took longest and how much of that was AES-GCM, and which API reply took longest to make.
`aes` says whether the crypto engine encrypts TLS records ([HTTPS.md](HTTPS.md)).

## Updating from the web page

Choose `app.bin` on the Firmware page. The page checks the file, uploads it signed with
the web password's key, waits for the restart, and keeps the new firmware once it answers.
If it does not start, the loader goes back to the previous one. An image that starts but is
never kept is given up after three 3-minute trials. The upload is checked in the library
before a slot is touched, and never replaces the running one.

**Kept firmware** lists up to seven images, and starts any of them the same way. Older
firmware ignores settings it does not know. See [NETWORK-UPDATE.md](NETWORK-UPDATE.md).

**So the web password can install firmware, which is full control of the gateway.** Loader
updates and flash dumps still need the update key. The endpoints are `POST /install`,
`/rollback` and `/confirm`; `tests/test_install.py HOST` checks the refusals.

### Without Home Assistant

Turn off **Announce to Home Assistant** (`mqtt.announce=0`) to publish only the data. The
discovery entries are withdrawn once; everything else stays the same, retained:
`HOST/port1` to `HOST/port4` and `HOST/device` (JSON), and `HOST/status` (`online`, or
`offline` from the last will). With changes allowed, `HOST/port1/set/KEY` takes any port
setting except `listen_only` and `access`. Firmware older than this ignores the setting and announces.

`HOST` is the host name unless **Base topic** (`mqtt.topic`) says otherwise, for example
`solar/gateway`. Changing it while connected clears what is retained under the old topic,
then reconnects, so the last will moves too; Home Assistant keeps the same entities, which
follow. Firmware older than this uses the host name again.

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

With announcing off there are no such sensors, and the automation above would silently stop
working. Define one by hand in `configuration.yaml` and use its name in the automation. (A
plain MQTT trigger would fire on every update.)

```yaml
mqtt:
  binary_sensor:
    - name: Port 1 alert
      state_topic: HOST/port1
      value_template: "{{ 'ON' if value_json.alert else 'OFF' }}"
      availability_topic: HOST/status
      device_class: problem
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
connections), a TCP client that reconnects, UDP, or a Modbus TCP gateway (below).
**Data ports have no password.**

| Setting | Values |
|---|---|
| baud | 300 to 2000000 |
| data_bits, parity, stop_bits | 7/8, none/even/odd, 1/2 (not 7N1) |
| gap_ms | silence that ends a packet; 0 = 3.5 characters, at least 1.75 ms |
| packet_len | 1 to 1400 bytes |
| echo_cancel | drop bytes received while sending |
| loopback | transmit to receive inside the chip (still drives the bus) |
| quiet_min | alert when nothing is heard, or no client is connected (TCP), for this many minutes: 0 (never), 1, 2, 3, 5, 10, 15, 20, 30, 60, 120, 180, 240, 360, 720 or 1440. Shown on the page, in `port N`, and as a problem sensor in Home Assistant. Firmware older than this ignores it |
| access | who may use the network side, up to 8: `10.0.0.5 read, 10.0.0.0/24 write`. Empty: anyone |
| listen_only | never transmits: the transmitter is off and the direction pin stays low; data from the network is discarded, `send` and `wiretest` are refused. Not settable over MQTT |
| modbus_timeout | Modbus gateway: ms to wait for a device's reply, 50 to 10000 (1000) |
| modbus_broadcast | Modbus gateway: 1 lets unit 0 reach every device; 0 (the default) refuses it |

A slow serial line pushes back on TCP senders. A network client that stops reading
loses packets (counted as "not delivered") without affecting the others. Bytes on their
way to a line wait in one of 6 buffers of 5.7 KB, shared by all ports, held only until
the line has taken them; readers and listen-only ports need none. If all 6 are in use,
another sender's data is offered again every quarter of a second until one is free:
nothing is lost. The
Hardware page shows how many have been in use at once.

### Modbus TCP gateway

`portN.mode=modbus` (502 is the usual port): Modbus TCP clients on the network, Modbus
RTU devices on the line. Each request goes out as an RTU frame (the unit, the request,
its CRC) and the device's reply goes back to the client that asked, with its transaction
ID. Up to 4 clients share the line: one request on it at a time, the rest waiting in
turn (up to 8). The gateway answers for itself only when the device cannot:

| | Answer |
|---|---|
| No valid reply within `modbus_timeout` | exception 0x0B (target device failed to respond) |
| 8 already waiting | 0x06 (busy) |
| A write from an address that may only read | 0x01 (illegal function); reads (1-4) and device identification (43/14) go through |
| Unit 0 without `modbus_broadcast`, or 248 to 255 | 0x0A (gateway path unavailable); nothing is sent |
| Not Modbus TCP | the connection is closed |

Unit 0 is a broadcast: every device on the line acts on it and none replies, so many
programs' default of 0 would write to all of them; hence it is off unless turned on. A
reply with a bad CRC, or from another unit, is ignored and the wait goes on. A Modbus
port never listens only, and drops what it hears while sending (its own echo).
`/api/state` gives each Modbus port `modbus`: requests, replies, exceptions (from
devices), timeouts, bad frames, busy, refused, broadcasts, waiting, and reply time last,
average and longest; the port page shows them. After going back to firmware older than
this, the port is taken as UDP, which Modbus clients do not use: nothing reaches the line.

## Home Assistant (MQTT)

Off by default. In the web UI enter the broker and tick **Connect to an MQTT broker**
(`.local` names work). The gateway then appears in Home Assistant as one device.
Tick **Let Home Assistant change settings** for controls as well as sensors.

```sh
tools/n540.py $H set mqtt.enabled=1 mqtt.host=homeassistant.local mqtt.user=n540 mqtt.control=1
tools/n540.py $H mqtt-passwd
```

- With controls: 88 entities, 68 enabled by default. Less common ones (packet timing,
  echo, loopback, port name) are there but disabled.
- A refused change shows under "Last rejected change" and the control goes back.
  Order matters: 7 data bits needs parity or 2 stop bits first.
- Commands reach port settings, the device name and restart. Network, MQTT and
  password settings are not reachable. Retained commands are ignored.
- **Anyone who can publish to the broker can use the controls.** Use a dedicated broker
  user. Without TLS the connection, broker password included, is not encrypted.
- Settings are written to flash (about 100k cycles per sector): don't automate
  settings changes every few seconds.
- Turning MQTT off removes the entities from HA.

Serial data does not go over MQTT; use the TCP/UDP ports for that.

### Over TLS

Tick **Encrypt the connection (TLS)** (`mqtt.tls=1`); the port moves to 8883. The broker
needs a certificate for the name the gateway is given (not an address) from Let's Encrypt
(either hierarchy: X1/X2 or the newer YE/YR) or Amazon, the roots the gateway carries
(`src/app/roots.c`). It is checked: name, chain
and dates. A failed check is a failed connection, with the reason on the MQTT page and in
the log; there is no falling back to plain MQTT.

- The handshake runs in slices beside the ports, as HTTPS does; reconnecting resumes the
  TLS session, so it costs milliseconds.
- About 23 KB of the TLS memory while connected (55 KB during the handshake).
- The clock must be known to check dates. After a restart it carries on; after a power
  cut MQTT waits for network time, a few seconds.
- A Mosquitto listener for it, with a certificate from certbot (Route 53 plugin):
  `listener 8883`, `certfile /etc/letsencrypt/live/NAME/fullchain.pem`,
  `keyfile /etc/letsencrypt/live/NAME/privkey.pem`. Restart Mosquitto after each renewal
  (certbot's `--deploy-hook`).
- Mosquitto 2.1 does not reload a certificate on SIGHUP: it must restart to serve a renewed one.
- Tested against Mosquitto with ECDSA and RSA certificates: `tests/mqtt_tls/run.sh`.
- **On the gateway (2026-11-26):** Mosquitto in Docker with certbot beside it; a Let's
  Encrypt certificate from the newer chain (YE1, Root YE, cross-signed by ISRG Root X2);
  TLS 1.2. The main loop's longest pause stayed 3.8 ms; TLS memory 44 KB at most.


New network settings run on trial for 2 minutes and revert unless kept (button in
the UI, or `n540.py $H keep`). The loader always uses DHCP.

## Security

Requests are signed with an HMAC over a single-use nonce, the path and the body.
The browser derives its key from the password (PBKDF2, 30000 rounds). The password
never crosses the wire, but on port 80 pages and data are not encrypted, so a listener could
attempt to guess a weak password offline. The web password can change settings, restart and
install firmware; it cannot read flash, replace the loader, or enter it.

## Access lists

A port's list names addresses or subnets and what each may do: `read`, or `write` (which
includes reading). The most specific entry decides; anyone else is turned away and counted.
An empty list allows anyone.

- A reader hears the bus; what it sends is accepted and dropped. Listen only still overrides
  everything.
- The list covers whoever is at the other end, in every mode: clients of a TCP server, the
  far end of a TCP client, senders and receivers of UDP.
- Whoever is turned away is shown for ten minutes in the station list (amber, "All normal"
  stays), and the last 8 addresses are on the port's report and in `GET /api/access/N`, in
  memory only. Home Assistant gets a count per port, with the last address as an attribute.
- **No client, and one unlisted address has tried 3 times or more in the last ten minutes:**
  the port alerts at once, whatever its Alert if quiet. Most likely the usual client has a
  new address. One or two tries, as from a scan, do not count.
- Changing a list drops only those no longer on it. The port is not restarted.
- It cannot be changed over MQTT, or bypassed by pointing a port elsewhere over MQTT.
- **It goes by IP address.** It stops mistakes and casual access, not someone on the network
  who takes over an address, and nothing is encrypted. Give listed clients fixed addresses.
- The console's `send` and the web page are governed by the web password, not by lists.
- The reload button does not clear lists. Firmware from before lists ignores them.
- If both stored copies are damaged, nobody is let in until a list is saved again.

Tested on the host: `tests/test_{acl,bridge,store}.c`. On hardware (2026-11-25): a listed
reader on a TCP server kept every byte while an unlisted address was turned away. Writers,
subnets, UDP and client mode are host-tested only.

## Storage

Saving erases flash, some 50 ms a sector. Data already on its way between ports and open
connections keeps moving meanwhile; new connections and replies wait.

Access lists have their own two copies at `0x0F2000` / `0x0F3000`, so older firmware still
reads the rest.


Settings: two alternating checksummed copies at `0x0F0000` / `0x0F1000` of external
flash 1 (blank on the stock unit). Schema 2 (MQTT) reads schema 1. Firmware from before
MQTT cannot read schema 2 and would start with defaults.

## Tests

```sh
cc -Isrc/app tests/test_cfg.c src/app/{cfg,acl}.c -o /tmp/t && /tmp/t   # host
cc -Isrc/app tests/test_knock.c src/app/knock.c -o /tmp/k && /tmp/k
cc -Isrc/app tests/test_acl.c src/app/{acl,cfg}.c -o /tmp/a && /tmp/a
cc -w -DD2= -DD2_BSS= -Itests/stub_net -Itests/stub -Isrc/app tests/test_bridge.c src/app/{bridge,cfg,acl,knock,traffic,history}.c -o /tmp/br && /tmp/br
cc -w -Itests/stub -Isrc/app -Isrc/common -DWDT_H '-Dwdt_feed()=(void)0' tests/test_store.c src/app/{cfg_store,cfg,acl}.c src/common/sha256.c -o /tmp/s && /tmp/s
cc -w -Itests/stub -Isrc/app -Isrc/common -DWDT_H '-Dwdt_feed()=(void)0' -DAXI_BSS= tests/test_fwlib.c src/app/fwlib.c src/common/{slots,image,sha256}.c -o /tmp/fl && /tmp/fl
tests/sim/run.sh                            # then, beside it:
python3 tests/test_install.py 127.0.0.1:8541
cc -DD2_BSS= -Isrc/app tests/test_cli.c src/app/{cli,cfg,acl,traffic}.c -o /tmp/c && /tmp/c
python3 tests/test_cli_device.py HOST       # nothing attached to the RS485 ports
python3 tests/test_device.py HOST           # nothing attached to the RS485 ports
cc -DD2_BSS= -Isrc/app tests/test_history.c src/app/history.c -o /tmp/h && /tmp/h
cc -Isrc/app tests/test_quiet.c -o /tmp/q && /tmp/q
cc -Isrc/app tests/test_button.c src/app/{button,cfg,acl}.c -o /tmp/b && /tmp/b
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

## Events

The Events page lists what happened, newest first, each with its time in your own time
zone: starts (and why: power-on, a restart, the watchdog), faults, firmware kept, settings
saved (and what), network trials kept or reverted, MQTT connecting and dropping, port
alerts raised and cleared, addresses turned away (once per address per ten minutes),
renewals, certificates installed, the clock being set, failed sign-ins (counted, at most
one entry a minute), restarts and who asked, and the reload button.

The log is kept in external flash (chip 1, from `0x0FC000`) through restarts and power cuts:
the last 200 or so, each record checked, so one cut short is skipped. Writing is rationed,
40 at once and then one every 90 seconds, so a flood cannot wipe it; what is not kept is
counted, and the count is kept. `GET /api/events` gives the newest 30,
`/api/events?before=SEQ` the 30 before that: `[seq, unix time or 0, seconds since that
start, start number, kind, port, number, note]`.

## Settings file

Hardware → Settings file: **Download settings** saves every setting as text, the same
`key=value` lines the console and `POST /api/config` take, with a note at the top of which
gateway, firmware and when. Passwords and keys are not in it. **Restore** sends a file's
lines back, signed like any change, so restored network settings go on trial for two
minutes. Lines starting `#` are notes. `GET /api/settings` gives the file as `{"text": …}`.

