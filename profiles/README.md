# Device profiles

A profile is a text file describing an RS485 Modbus device's registers: the gateway polls them and publishes them to MQTT (and Home Assistant).


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

## Statements

| Statement | Form |
|---|---|
| `block` | `id kind address count period [info]`; kind `hold` (3), `input` (4), `coil` (1), `discrete` (2); period `Ns`, `Nm`, `Nh`; count up to 125 registers or 2000 bits |
| `sensor` | `id "name" block address type [value] [shown]`: a number |
| `binary` | `id "name" block address` and `bit N`, or a type and a comparison (`u16 == 5`, `!= 0`) |
| `text` | `id "name" block address` and `enum [mask=M] v=name ...`, `bits n=name ...` (set bits' names joined by "; "), or `ascii N`, `asciilo N` or `asciile N` registers of text: two characters a register with the high byte's first, one in the low byte, or two with the low byte's first. `enum` and `bits` may follow a type (`u32le bits ...` for 32 named bits) |
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
- **Limits:** 16 KB of text, lines of 1024 characters, 16 blocks, 160 fields and controls,
  names 40 characters. A model that differs (SRNE HSI1500S) gets a profile of its own.
- **What a device may show:** its state is one MQTT message of up to 7.5 KB (about 110
  values at their longest), and /api/devices sends a device in three parts of up to 40
  values, each at most 5.6 KB; `tests/test_devmqtt.c` holds every built-in profile to both.
- Uploads come in phase 2. Then a profile id may be built in and uploaded: the uploaded
  one is used, and deleting it brings back the built-in one.

## Controls

Settings a device keeps, which Home Assistant can change when the device has
`controls=1`. Each is read from a `hold` block, as a sensor is, and written to the same
address: one register by function 6, two by function 16. A write reads the control's block
first (as its poll does), changes only the control's own bits, and is not made if nothing
would change.

| Statement | Form |
|---|---|
| `number` | `id "name" block address type [*scale] [+offset] min M max M [step S] [unit] [dc=] [hidden]`; type `u16 s16 u32 s32 u32le s32le`; min and max must fit the register; the step is the scale unless given |
| `select` | `id "name" block address [mask=M] v=name ... [hidden]`; each value as it is in the register under the mask; names unique |
| `switch` | `id "name" block address bit N [hidden]`, or `on V off V` for a whole register |
| `time` | `id "name" block address [hidden]`: HH:MM, the hour in the low byte, the minute in the high |

`hidden`: announced disabled, so it must be enabled in Home Assistant on purpose.

## Phase 1 notes

- Built-in profiles only (below): profiles cannot be uploaded yet.
- `button` comes with a later firmware.
- `reject B if zero f1 f2 ...`: the block's reply is not used when every listed field is 0
  (for example `reject live2 if zero remaining_capacity state_of_health full_capacity`).

## How to add a built-in profile

A `profiles/<id>.txt` whose first statement is `profile <id> 1`; it is embedded at the next
build, and `tests/test_builtin.c` checks it parses.

## Built in

| Profile | Device | From | Tried on the device |
|---|---|---|---|
| `eg4-bms` | EG4 LifePower4 / LL battery | github.com/RAR/esphome-eg4-bms | yes |
| `eg4-hybrid` | EG4 18kPV and FlexBOSS hybrid inverters | EG4's "18KPV-12LV Modbus Protocol", read on two FlexBOSS | yes, settings too |

## eg4-hybrid

19200 8N1, unit 1. 95 values from seven reads of 40 registers: what the inverter is doing
(input 0-39, every 10 s), its totals, faults, warnings and temperatures (input 40-79, every
minute), and its serial number, firmware and settings (holding 0-39, 40-79, 80-119,
120-159, 160-199, every 2 minutes): about 9.5 requests a minute. Requests are a second
apart, the document's minimum; with SolarAssistant's reads on the same port the line is
then near full, so the turnaround is to be set from the inverter's measured reply time. It
was read on two FlexBOSS; the 18kPV is the same document.

- EG4's document frames each message with the inverter's serial number in it, as the Wi-Fi
  dongle does. Over RS485 the inverters answer plain Modbus RTU, to unit 0 and to their own
  address (holding 15, 1 by default), which is what the profile asks.
- The FlexBOSS reads some settings beyond the document's ranges: charge and discharge current
  to 250 A, and the three powers (holding 66, 74, 82) in tenths of a kW where it says percent.
- The settings are controls (AC charge, charge first, forced discharge, with their powers,
  SOC limits and three time windows each; currents, export limit, cut-off SOCs, charge and
  discharge control, output priority, export and the hidden off-grid output and power
  switches).
- Faults and warnings are text: the names of the set bits, each with EG4's code (`E012 UPS
  short`), or `None`.
- Battery power is charge less discharge (positive: charging). Grid power is import less
  export (positive: importing).
- Left out, for the room a device has: the battery's own figures as its BMS reports them
  (input 80-119), the generator and each EPS leg (120-138), arc fault detection (140-152),
  the power factor (19: two ranges in one number, which the format cannot turn into one),
  and the settings listed at the end of the profile. The firmware text (holding 7-8) is not in EG4's document; it is where
  the monitor named above reads it.

## Differences from the ESPHome component (eg4-bms)

`eg4-bms` follows github.com/RAR/esphome-eg4-bms: the same registers, entity ids, names,
label text and display attributes. Three differences remain because the format cannot
remove them: set bits are joined by "; " (the component uses ";"); the component prefixes
status with "Heating+" when bit 15 is set, where the profile has heating as its own binary
sensor; and an unknown status reads "Unknown (N)" where the component says "Unknown". The
state_of_charge icon and the component's online status, bitmask and rejected-frame sensors
are not carried over.
