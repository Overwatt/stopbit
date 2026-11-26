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
| `text` | `id "name" block address` and `enum [mask=M] v=name ...`, `bits n=name ...` (set bits' names joined by "; "), `ascii N` or `asciilo N` registers |
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

## Phase 1 notes

- Controls (`number`, `select`, `switch`, `button`) are refused until phase 2.
- `reject B if zero f1 f2 ...`: the block's reply is not used when every listed field is 0
  (for example `reject live2 if zero remaining_capacity state_of_health full_capacity`).

## How to add a built-in profile

A `profiles/<id>.txt` whose first statement is `profile <id> 1`; it is embedded at the next
build, and `tests/test_builtin.c` checks it parses.

## Differences from the ESPHome component (eg4-bms)

`eg4-bms` follows github.com/RAR/esphome-eg4-bms: the same registers, entity ids, names,
label text and display attributes. Three differences remain because the format cannot
remove them: set bits are joined by "; " (the component uses ";"); the component prefixes
status with "Heating+" when bit 15 is set, where the profile has heating as its own binary
sensor; and an unknown status reads "Unknown (N)" where the component says "Unknown". The
state_of_charge icon and the component's online status, bitmask and rejected-frame sensors
are not carried over.
