# Device profiles: RS485 devices to MQTT and back

Approved 2026-11-26, section by section. Today each of these devices has its own ESP32
running an ESPHome component; the gateway polls them itself instead and translates their
data and controls to MQTT, with Home Assistant discovery.

| Device | Protocol | Component it replaces |
|---|---|---|
| EG4 LifePower4 / LL BMS | Modbus RTU, function 3, master 0x10 and packs 0x01-0x0F | github.com/RAR/esphome-eg4-bms |
| SRNE inverter | Modbus RTU, functions 3 and 6 | github.com/RAR/esphome-srne-inverter |
| SmartGen HSC941 generator controller | Modbus RTU, functions 1, 3 and 5 (coil "on" is 0xFFFF) | github.com/RAR/esphome-smartgen-hsc941 |
| Ecoworthy BMS | JBD framing (0x78 read, 0x79 write, magic on writes) | github.com/RAR/esphome-ecoworthy-bms |

## Decisions

- **Profiles, plus drivers.** A device type is described by a text profile, not code. Modbus
  devices need nothing else; a non-Modbus protocol (JBD) gets a small C driver that frames
  requests and returns raw blocks, decoded by the same profile tables.
- **Built in and uploadable.** The profiles for the four devices are written from the
  ESPHome components, kept in `profiles/`, and built into the firmware. More, or changed
  ones, are uploaded at run time and need no firmware update.
- **Shared bus.** The poller is one more client of a port's Modbus queue (`modbus.c`):
  Modbus TCP clients keep working on the same port, with the same reply checks.
- **First device: EG4 BMS** (read only, many packs on one bus).
- **Home Assistant IDs:** the gateway's own by default (`n540_<name>_<field>`); per device,
  optionally the ESP32's (`sensor.eg4_bms_pack_voltage`) once the ESP32 is gone.
- Not chosen: profiles compiled at build time (every fix a firmware update); embedded
  scripting (Lua is about 150 KB of the 384 KB image, tens of KB of RAM per script, and
  could hold up the main loop).

## How it fits in

| Piece | Does |
|---|---|
| `profile.c` | Parses a profile into compact tables, or refuses it with line and reason. No I/O. Fuzzed. |
| `device.c` | The engine: per configured device, which block is due; queue its read; decode the reply; work out computed values; staleness; writes. |
| `devmqtt.c` | State, availability and discovery over the existing MQTT connection (`mqtt_ha.c`); commands to writes. |
| drivers | Modbus RTU through `modbus.c` first; JBD later behind the same small interface. |

- A polled port runs in Modbus gateway mode, with a local TCP port like any other: older
  firmware refuses the whole settings record if an enabled port has none, so a downgrade
  would lose every setting. Its TCP clients share the queue with the polling; an access
  list can turn them away, and `modbus_tcp=0` opens no listener at all
  ([design](2026-11-26-modbus-tcp-off-design.md)). The engine adds a local client to the port's queue (`modbus.c`
  gains one beside the 4 network clients, whose replies go to a callback, and a timeout
  per request).
- Everything runs in the main loop; no job stack. The engine keeps one request in each
  port's queue at most, so at most one reply per port is decoded per pass.
- **Settings** `devN.*`, N = 1..16, in a record of their own (`cfg_store.c`, like the access
  lists), so older firmware never reads them:

| Key | Values |
|---|---|
| `devN.profile` | a profile id; empty: no device |
| `devN.port`, `devN.address` | 1-4; 1-247 |
| `devN.name` | 1-24 of `a-z 0-9 _ -`: in topics and IDs |
| `devN.label` | shown in HA as the device's name; default the name |
| `devN.controls` | 0 (default) or 1 |
| `devN.ids`, `devN.prefix` | `own` (default) or `esphome`; the ESPHome name prefix, e.g. `eg4_bms` |

## Profile format

One statement a line; `#` starts a comment; names in double quotes (UTF-8). Numbers in
decimal or `0x` hex. An id is 1-24 of `a-z 0-9 _`. An excerpt, its addresses illustrative
(the real profile follows the component's REGISTER_MAP.md):

```
profile eg4-bms 1                       # profile id (a-z 0-9 -), format version (1)
name "EG4 LifePower4 BMS"               # HA model
maker "EG4"                             # HA manufacturer
line 9600 8N1                           # checked against the port: a mismatch is reported
stale 5                                 # misses before a block's values are unavailable (5)
timeout 1000ms                          # a reply may take this long (1000ms)
turnaround 50ms                         # quiet on the line before any request (0ms)

block live1  hold 0x0000 18 10s
block live2  hold 0x0012 21 10s
block model  hold 0x0069 11 15m info    # info: not needed for "online"

sensor pack_voltage "Pack voltage" live1 0x0000 u16 *0.01 V dc=voltage sc=measurement
sensor current      "Current"      live1 0x0001 s16 *0.01 A dc=current sc=measurement
sensor cell1..16    "Cell #"       live1 0x0002 u16 *0.001 V dc=voltage prec=3
sensor temp1        "Temperature 1" live2 0x0021 s8h °C dc=temperature
sensor cycles       "Cycles"       live2 0x0013 u32 sc=total_increasing
text   status       "Status"       live2 0x0016 enum mask=0x000F 0=Standby 1=Charging 2=Discharging 3=Protect 4="Charging limited"
binary heating      "Heating"      live2 0x0016 bit 15
text   warnings     "Warnings"     live2 0x0017 bits 0="Cell overvoltage" 1="Cell undervoltage"
text   model        "Model"        model 0x0069 ascii 11

calc power        "Power"            mul pack_voltage current W dc=power
calc cell_min     "Cell minimum"     min cell1..16 within 0.001..5 V
calc cell_delta   "Cell delta"       delta cell1..16 within 0.001..5 V
calc cell_low     "Lowest cell"      argmin cell1..16 within 0.001..5
calc may_charge   "Charging allowed" nobits protection 0x00F3

reject live1 if zero
reject live1 if pack_voltage == 0
```

**Statements**

| Statement | Form |
|---|---|
| `block` | `id kind address count period [info]`; kind `hold` (3), `input` (4), `coil` (1), `discrete` (2); period `Ns`, `Nm`, `Nh`; count up to 125 registers or 2000 bits |
| `sensor` | `id "name" block address type [value] [shown]`: a number |
| `binary` | `id "name" block address` and `bit N`, or a type and a comparison (`u16 == 5`, `!= 0`) |
| `text` | `id "name" block address` and `enum [mask=M] v=name ...`, `bits n=name ...` (set bits' names joined by "; "), `ascii N`, `asciile N` (added 2026-11-27: two characters a register, the low byte's first) or `asciilo N` registers |
| `calc` | `id "name" function arguments [within lo..hi] [shown]`; a comparison gives a binary, the rest a number |
| `reject` | `block if zero` (every byte 0) or `block if field op value`: the block's reply is not used |
| `number` | `id "name" reg\|regs address type [value] min M max M step S [state field] [shown] [hidden]` |
| `select` | `id "name" reg address v=name ... [state field] [hidden]` |
| `switch` | `id "name"` and `reg A on V off V`, `coil A [on V]`, or `reg A bit N` (read-modify-write); `[state field] [hidden]` |
| `button` | `id "name" reg\|coil address value V [hidden]` |

- **Types:** `u16 s16`; `u32 s32` (high word first), `u32le s32le` (low word first); `u8h u8l
  s8h s8l` (one byte of a register); `bit N`; `coil` (a bit of a coil or discrete block).
- **Value:** `+N` (offset) and `*N` (scale): value = (raw + offset) × scale. `nodata V ...`:
  raw values that mean "unavailable".
- **Shown:** one bare token is the unit (`V`, `°C`, `kWh` ...); `dc=` HA device class, `sc=`
  state class, `prec=` decimals shown, `diag` (diagnostic). A control with `hidden` is
  announced disabled: it must be enabled in HA on purpose.
- A `text` field (enum, bits) keeps its raw number too, which `calc` and `binary` can use.
- **Lists:** `cell1..16` declares cell1 to cell16 at consecutive addresses (by the type's
  size), `#` in the name replaced by the number; in `calc` it names the same fields.
- **Functions:** `sum min max avg delta argmin argmax count` (a list; argmin/argmax give the
  1-based position), `sub mul` (two), `pos neg abs` (one); binary: `eq ne gt lt` (field and
  constant), `anybits nobits` (field and mask). An unavailable input makes the result
  unavailable; `within` leaves out values outside the range (a missing cell reads 0).
- **Limits:** 16 KB of text, 16 blocks, 160 fields and controls, names 40 characters. A model
  that differs (SRNE HSI1500S) gets a profile of its own.
- A profile id may be built in and uploaded; the uploaded one is used. Deleting it brings
  back the built-in one.

## Polling

- **Turns.** Per port, at most one engine request in the queue. When it is answered or times
  out, the most overdue block of that port's devices goes next (round robin when equal).
  Network clients' requests take turns in between. At start every block is due.
- **Timing from the profile, per request.** `timeout` is the request's own (network clients
  keep the port's `modbus_timeout`); `turnaround` is quiet on the line before the next
  request, and the port uses its devices' longest for every request, network ones too, as
  they share the devices.
- **Replies** are matched by `modbus.c` (request in flight; unit, function, length) and
  decoded on arrival; the computed values using them are worked out again.
- **Misses:** a timeout, a device's exception or a `reject` rule. The last values are kept,
  the reason kept for the Devices page ("exception 2 at 0x0211"). After `stale` misses in a
  row, that block's values are unavailable; when every block not marked `info` is stale the
  device is offline.
- **Writes** go before any waiting poll; then the block with the control's `state` field is
  read at once.
- **Load:** an EG4 master and 15 packs at 9600 baud: 32 reads in 10 s, about 110 ms each
  (request, 47-byte reply, 50 ms turnaround): 35% of the line. The Devices page shows each
  port's use.

## MQTT and Home Assistant

As the gateway's own: `HOST` is the base topic, `HOST/status` the gateway's availability.

- **State:** `HOST/dev/<name>/state`, one JSON object of the device's values, e.g.
  `{"pack_voltage":53.12,"status":"Charging"}`; unavailable is `null`. Published after a
  block updates, at most once a second per device, and at least once a minute.
- **Availability:** `HOST/dev/<name>/status`, `online`/`offline`, retained. An entity is
  available while the gateway and its device are online and its own value is not `null`.
- **Discovery:** one retained config per entity, `homeassistant/<component>/HOST_<name>/<field>/config`,
  sent a few per pass after connecting (EG4 master and 15 packs: about 900). Each device is
  an HA device with the profile's maker and model, `via_device` the gateway. `unique_id`
  `HOST_<name>_<field>`, or with `devN.ids=esphome` `HOST_<name>_<prefix>_<field>` (a switch of
  naming, or a new prefix, is then a new entity to HA and takes the free entity ID rather than
  the registry restoring the old one). Templates read `value_json['<field>']`. The entity ID suggested: `n540_<name>_<field>`, or with
  `devN.ids=esphome` `<prefix>_<name as ESPHome makes it>` (lower case, other characters
  `_`). Profiles take their entity names from the component's example YAML, so these match
  an ESP32 set up from it; a renamed entity there will not match. Removing or renaming a device clears
  its retained configs and topics. With `mqtt.announce=0` there is no discovery; the rest stays.
- **Commands:** `HOST/dev/<name>/set/<control>`: a number for a number, a choice's name for a
  select, `ON`/`OFF`, `PRESS`. Out of range or unknown: refused. Retained messages are
  ignored. Only for devices with `devN.controls=1` (separate from `mqtt.control`, which
  covers the gateway's settings); without it, controls are not announced at all.

## Controls and safety

- Function 5 (coil; "on" 0xFF00 unless the profile says `on=`), 6 (register) or 16 (several
  registers, for 32-bit values).
- `reg A bit N` reads the register just before writing it, not the last poll.
- At most 2 writes a second per device.
- After a write the state is read back; if it does not match, HA shows what was read and the
  event log says so. Every write is logged: device, control, value, result.

## Profiles kept, and the page

- Built in: `profiles/*.txt`, embedded in the image. Uploaded: a 64 KB area of external
  flash, each up to 16 KB, signed with the web password as settings are; from the Devices
  page or `n540.py HOST profile put FILE`; refused with line and reason if it does not parse.
- **Devices page:** a row per device (online, last poll, misses and the last reason), each
  port's line use; a device's values and its form (profile, port, address, name, controls,
  HA naming); the profiles, with upload and delete. `GET /api/devices`; `/metrics` gains
  polls, misses and line use per device.

## Tests

- **Host:** the parser (every statement; every error, with its line); the engine with a fake
  line and clock (turns, turnaround, timeouts, staleness, `reject`, calc, writes and
  read-back, the bit switch's read-modify-write); MQTT JSON and discovery payloads
  (well-formed, sizes); commands.
- **Built-in profiles:** each parses, and decodes frames built from its REGISTER_MAP examples
  to the expected values.
- **Fuzzing:** the parser; random replies against each built-in profile.
- **CI:** the engine and MQTT side against Mosquitto (as `tests/mqtt_tls`): discovery and
  state as a subscriber sees them.
- **Gateway:** with the EG4 wired, beside its ESP32 (own IDs), values compared, then switched.

## Phases

1. Parser, engine, MQTT and HA, Devices page, EG4 profile: reading only.
2. Controls ([design](2026-11-27-device-controls-design.md)); SRNE and SmartGen profiles.
3. JBD driver; Ecoworthy profile.
