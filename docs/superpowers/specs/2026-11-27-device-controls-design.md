# Device controls: settings written over MQTT

Approved 2026-11-27, section by section. Phase 2 of the [device profiles
design](2026-11-26-device-profiles-design.md), which it extends; where the two differ, this
one holds. The first device with controls is the EG4 inverter. Buttons, the SRNE and
SmartGen profiles and the JBD driver come later.

## What the inverters showed

Two EG4 FlexBOSS inverters, one on each of ports 2 and 3, were listened to on 2026-11-27
while SolarAssistant read them through server-mode ports; the gateway sent nothing.

- Plain Modbus RTU at 19200 8N1. The serial number in every message, in EG4's document, is
  the WiFi dongle's framing, not RS485's.
- Each answers unit 0, which SolarAssistant uses. Its own address (holding register 15) is
  1, which the engine uses; that it answers there is the first thing checked on the gateway.
- SolarAssistant reads 40 registers about every 1.5 s (input 0-199, holding 0-199 and one
  group beyond): 3% of the line.
- The live values and totals decode as the profile has them. Of two bytes in one register,
  the item the document lists first is the low byte: the clock at 12-14, and a time window
  at 77 that reads 0x3B17, 23:59.
- Some settings differ from the 18kPV document. The charge and discharge current limits
  (101, 102) read 250 A, where it gives 0-140: the FlexBOSS's limit, in amps. AC charge,
  charge-first and forced-discharge power (66, 74, 82) read 120, where it gives 0-100 %:
  tenths of a kW, 12.0 kW. That is from the Monitor-My-Solar dongle's register schema (66
  and 82 in kW, 0-25, step 0.1) and its vendor's Home Assistant integration (all three
  scaled by 10); no write has confirmed it yet.

## Profile format

A control is a field that can be written: it names its block and address as a sensor
does, shows what is read there, and writes to the same address. The design's `state field`
and `reg|regs` are not used. A control on an `input` or `discrete` block is refused.

| Statement | Form |
|---|---|
| `number` | `id "name" block address type [value] min M max M [step S] [shown] [hidden]`; type `u16 s16`, or `u32 s32 u32le s32le` (two registers) |
| `select` | `id "name" block address [mask=M] v=name ...`; each value as it is in the register, masked, as `text ... enum` reads it |
| `switch` | `id "name" block address` and `bit N`, or `on V off V` |
| `time` | `id "name" block address`; `HH:MM`, the hour in the low byte and the minute in the high byte |

- **Value:** a number's register value is value ÷ scale − offset, rounded. The step is the
  scale unless given.
- **Writes:** one register by function 6, two (the 32-bit types) by function 16. `bit N` and
  `mask=` read the register just before writing it and change only their own bits:
  (read & ~mask) | value.
- `hidden`: announced disabled in HA, to be enabled there on purpose.
- The limit stays at 160 fields and controls a profile.

## Writes

1. A command is checked against its control (see Commands) and queued. The queue has 16
   places for all devices; a newer command for a control already queued replaces its value.
2. A port's queued writes go before its next poll, at most 2 a second per device.
3. The register is read first if the control's `bit` or `mask` needs it; then written.
4. The control's block is read at once, and HA shows what was read.

Every write is in the event log with its outcome ("inverter ac_charge_soc_limit 80: ok",
"exception 3", "timeout", "read back 75"), and so is each refused command, with the reason.

## MQTT and Home Assistant

- **Buffers** (the application only): lwIP MQTT's output ring from 4 to 8 KB, and the device
  state message's buffer from 3.5 to 7.5 KB; about 8 KB of DTCM. A device may have about
  110 values, settings in its one state message with its live values. `tests/test_devmqtt.c`
  holds every built-in profile to that at its worst case.
- **Commands:** `HOST/dev/<name>/set/<control>`, subscribed as `HOST/dev/+/set/+` once
  connected. A number in decimal (rounded to the step), a choice's name, `ON` or `OFF`, or
  `HH:MM`. Refused, with an event: retained messages, an unknown device or control,
  `devN.controls=0`, a number out of range, a name not among the choices. A command is
  trusted as far as the broker is (TLS and its login), as `mqtt.control` is.
- **Discovery:** `number` (min, max, step, unit), `select` (options), `switch`, and `text`
  for a `time` (pattern `^([01][0-9]|2[0-3]):[0-5][0-9]$`, 5 characters). Each has
  `command_topic` `~/set/<id>` and `entity_category` `config`; its state, availability and
  `unique_id` are as a sensor's. `hidden` gives `enabled_by_default` false. With
  `devN.controls=0` controls are not announced and their configs are emptied; their values
  are still in the state message and on the Devices page.
- **`/api/devices`:** a device goes in two parts, its first 60 values and then the rest, so
  no reply buffer grows (they are in ITCM, which has 9.6 KB free). The page is unchanged:
  settings are among the values.

## Modbus gateway mode: RTU over TCP

SolarAssistant sends RTU frames over TCP (socat from a serial device) to unit 0. A Modbus
gateway port drops it, and a polled port must be one; so that both can use the inverters:

- **`portN.modbus_framing`:** `tcp` (the default: Modbus TCP, as now) or `rtu`: clients send
  RTU frames and are answered in RTU frames. A frame's length comes from its function:
  1 to 6, 8 bytes; 15 and 16, 9 and the byte count. Other functions are refused (exception
  1). A bad CRC empties that client's buffer.
- **`portN.modbus_broadcast`:** 0 and 1 as now, and `answer`: unit 0 is passed on and its
  reply waited for, as EG4 inverters answer it.
- SolarAssistant's requests and the engine's share the port's queue, with the profile's
  turnaround between requests. With 1 s, a reply to SolarAssistant may come up to about 1 s
  later; SolarAssistant's own 1.5 s suggests the inverters need less, which is measured
  before the turnaround is lowered. Access lists apply.
- Older firmware reads such a port as Modbus TCP, and drops SolarAssistant.
- Ports 2 and 3 are switched (`mode=modbus`, `modbus_framing=rtu`, `modbus_broadcast=answer`)
  when the user says; SolarAssistant's connections drop for a moment.

## The EG4 profile

- The profile becomes `eg4-hybrid`, model "18kPV / FlexBOSS": both use this register map.
- Settings are read from holding 40-79, 80-119, 120-159 and 160-199 every minute, `info`;
  `ident` (0-39, which holds register 21) goes from every 15 minutes to every minute. With
  SolarAssistant's reads, the line is about 45% in use.
- 41 controls, which take the profile to 95 values:

| Group | Controls |
|---|---|
| AC charging | `ac_charge` switch (21 bit 7); power (66, 0-25 kW); SOC limit (67, 0-100 %); start SOC (160, 0-90 %) and stop SOC (161, 20-100 %); current (168, 0-250 A); `ac_charge_by` select (120, mask 0x000E: 0 Off, 2 Time, 4 Voltage, 6 SOC); three windows, each a start and an end (68-73) |
| Charge first | `charge_first` switch (21 bit 11); power (74, 0-25 kW); SOC limit (75, 0-100 %); three windows (76-81) |
| Forced discharge | `forced_discharge` switch (21 bit 10); power (82, 0-25 kW); SOC limit (83, 0-100 %); three windows (84-89) |
| Battery | charge and discharge current limits (101, 102, 0-250 A); on-grid cut-off SOC (105, 10-90 %); off-grid cut-off SOC (125, 0-90 %); `discharge_control` select (120, mask 0x0030: 0 Voltage, 16 SOC, 32 Both) |
| Grid and mode | `export` switch (21 bit 15); export limit (103, 0-100 %); `off_grid_output` switch (21 bit 0, hidden); `power` switch (21 bit 9: off is standby; hidden); `output_priority` select (145: 0 Battery first, 1 PV first, 2 AC first) |

- The three powers (66, 74, 82) are `u16 *0.1 kW`, step 0.1. The 18kPV document has them in
  percent; that the 18kPV reads them as the FlexBOSS does is not known.
- Left out: register 11 (restore defaults, clear records, reboot), grid protection (25-63),
  the clock, PV input mode, parallel and lead-acid settings, the 0.1 % settings (138-143),
  AC-first windows (152-157) and generator charging (194-198).
- In a parallel pair a setting that must match is written to each inverter; the gateway
  does not copy one to the other.

## Safety

- Controls are announced and obeyed only for a device with `devN.controls=1`.
- The two controls that can cut power to loads, `off_grid_output` and `power`, are hidden.
- Ranges are the document's, widened where the inverters read beyond it. The inverter's own
  exception and the read-back stand behind them.

## Tests

- **Host:** the parser (every control form; every error, with its line); the engine with a
  fake line and clock (a write before a poll, read-modify-write, a read-back that matches
  and one that does not, an exception, a timeout, a queued command replaced, a full queue,
  2 a second, `controls=0`); commands of each kind; discovery payloads; the EG4 profile at
  its worst case (its state under 7.5 KB, each `/api/devices` part under 5,621 bytes); its
  settings decoded from registers read from the inverters (values only, no serial numbers),
  and writes encoded (`ac_charge` ON with register 21 at 0xF355 writes 0xF3D5).
- **`modbus.c`:** RTU client frames (lengths, CRC, refused functions, a frame split across
  TCP segments), `answer` for unit 0, replies framed back.
- **Fuzzing:** RTU client bytes into `modbus.c`; commands against the built-in profiles;
  profile seeds with controls.
- **Gateway:** an install (the loader byte-identical, port 1 untouched); port 4 in loopback
  with MQTT parked, where a command sends a write and logs a timeout. Then, with the user,
  ports 2 and 3 switched: SolarAssistant still reads, the engine reads both inverters, their
  settings agree with the EG4 app (the three powers in kW among them), and one write is made
  with the user watching (AC charge SOC limit up 1, then back).

## Order

1. RTU over TCP in Modbus mode: the gateway can then read the inverters beside SolarAssistant.
2. The control statements, the write path, MQTT and HA.
3. The EG4 profile's settings and controls.

## Not in this phase

Buttons; the SRNE and SmartGen profiles; changing settings from the page; copying a
setting across a parallel pair; a view or log of a port's traffic, which gets its own design.
