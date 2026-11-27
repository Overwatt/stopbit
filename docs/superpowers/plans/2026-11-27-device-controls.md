# Device controls implementation plan (DRAFT: task outline and decisions; code steps to be written)

**Spec:** docs/superpowers/specs/2026-11-27-device-controls-design.md

**Global constraints:** loader byte-identical (only version text differs); no gateway install, port 2/3
change or Pi unit restart without the user present; port 1 never transmits; no private LAN names,
addresses or serials in commits; host tests (`tests/host.sh`) pass after every task.

## Decisions taken while reading the code (rulings)

- `modbus_t.broadcast` becomes `uint8_t` with `MB_BC_OFF/ALL/ANSWER`; `modbus_t.rtu` plus
  `modbus_set_rtu(m, rtu)` (a change clears clients' partial frames). `take()` is refactored to
  `take(m, c, tid, unit, pdu, plen)`. RTU replies go back as the frame heard (`m->rx`, `m->rxn`);
  gateway exceptions are RTU frames with CRC. An RTU client's bad CRC, unknown function or frame
  over 256 bytes drops its buffer and the rest of that TCP piece (no resync inside a piece);
  counted in `refused`. Unit 0 with ANSWER waits for a reply (MB_SENDING).
- Settings: `more_cfg_t` gains `modbus_rtu` and `modbus_answer` (bit per port) from `spare`
  (148 -> 146). `modbus_broadcast=0|1|answer`, `modbus_framing=tcp|rtu`; JSON gives them as strings
  (`"modbus_broadcast":"answer","modbus_framing":"rtu"`); helpers `cfg_modbus_unit0()`,
  `cfg_modbus_rtu()`. api.c `static char text[3200]` -> 3456 (settings text worst case is 2878 of
  3000 now; test_cfg bound `3456 - 200`). Page: two selects (`sel("modbus_framing", ...)`,
  `sel("modbus_broadcast", ...)`) in the Network grid; the checks div keeps only `modbus_tcp`
  (`data-f="modbus_tcp"`).
- Field kinds: controls go BEFORE PF_CALC, because pfdecode.c's calc() treats every kind >= PF_CALC
  as a calc: `PF_SENSOR, PF_BINARY, PF_ENUM, PF_BITS, PF_ASCII, PF_NUMBER, PF_SELECT, PF_SWITCH,
  PF_TIME, PF_CALC, PF_CALCBIN`; `PFF_HIDDEN = 4`; `pf_control(f)`. Reused field members: a
  number's min/max in `lo`/`hi`, step in `k` (default |scale|); a switch's on value in `k`, off in
  `lo`, bits in `mask`; a select's bits in `mask` (default 0xFFFF; values listed masked, unshifted).
- Parser: `number|select|switch|time` statements; a control needs a hold block; number types
  u16 s16 u32 s32 u32le s32le only; min and max required and must fit the register (raw =
  value/scale - offset); `min/max/step/hidden` handled in `mods()` under new allow flags
  `M_LIMITS`/`M_HIDDEN`, placed before the unit branch so a sensor's unit may still be `min`;
  select: mask before values, values inside the mask, choice names unique; `button` refused
  ("comes with a later firmware"). test_profile.c line 160's assertion becomes "no block 'reg'".
- Decode: select/switch masked like enum; a switch that reads neither on nor off is unknown; a time
  with hour > 23 or minute > 59 is unknown; pf_text gives a select's name and a time's "HH:MM";
  pf_prec treats PF_NUMBER as PF_SENSOR; device_value: select/time as text, switch as true/false.
- New pure module `src/app/pfwrite.c/.h`: `pf_write_t {addr, regs, mask[2], value[2]}`,
  `pf_command(p, i, payload, &w, why, max)` (number parsed by hand, rounded to the step, range
  checked; choice by exact name; ON/OFF; HH:MM -> minute << 8 | hour; 32-bit by word order) and
  `pf_merge(&w, have, out)` (true if it changes them). CMake globs src/app, so no list to edit.
  Test `tests/test_pfwrite.c` (host.sh: `t pfwrite -Isrc/app tests/test_pfwrite.c src/app/pfwrite.c
  src/app/profile.c -lm`); fuzz target `command` (built-in profiles, random field and payload).
- Engine (device.c): tag = op << 24 | gen << 16 | dev << 8 | n, op TAG_POLL/READ/WRITE/CHECK, n the
  block or the write slot; `s_wq[16]` {stage, dev, gen, field, seq, w, want[2], what[40]};
  `write_next(port)` before `submit(port)` (oldest queued for the port, 500 ms since the device's
  last write); READ (fn 3) -> pf_merge -> unchanged (event, block due now) or WRITE (fn 6, or 16 for
  two) -> CHECK (read the control's block, decoded as a poll, registers compared under mask).
  In-flight writes are freed with an event when the port's queue is re-inited or the 90 s
  backstop fires. `device_command(name, control, payload, retained)`; a newer command for a queued
  control replaces its value and keeps its place.
- Events: `EV_WRITE` after EV_DROPPED; port = device's port; a = device number | outcome << 8
  (0 ok, 1 unchanged, 2 refused, 3 failed, 4 reads back otherwise); s = "control=value" and detail
  ("out of range", "no reply", "error 3", "not read back", the value read). Page evText case 20,
  evBad for outcome >= 2. device.c now calls events_add: every host test that links device.c
  (device, devmqtt, devroom, devmqtt_long) needs a stub, e.g. `tests/stub/events_stub.c`.
- MQTT: `cb_topic` handles `BASE/dev/<name>/set/<control>` before its `mqtt.control` check and
  calls device_command from cb_data; second subscription `BASE/dev/+/set/+` (flag reset where
  `s_sub` is). devmqtt: comp() number/select/switch/text; command_topic `~/set/<id>`,
  `entity_category: config`, hidden -> `enabled_by_default: false`, text pattern, select options;
  with controls=0 a control's config is published empty. `BUF` 7680; lwipopts
  `MQTT_OUTPUT_RINGBUF_SIZE` 8192 (APP_IMAGE). test_devmqtt payload buffer 4096 -> 8192.
- `/api/devices`: parts of 40 values per device, three parts each (the last takes the rest):
  `device_json_part(i, k, ...)`, `device_json()` kept as their concatenation for tests. This
  changes the spec's "first 60, then the rest" (worst case for 60 values is too near 5,621):
  update the spec line when the plan is finished.
- EG4 profile: rename to `eg4-hybrid` (file, id, test_builtin, README, profiles/README), model
  "18kPV / FlexBOSS"; blocks hold 40/80/120/160 every 1m info, ident to 1m; 41 controls per the
  spec table; test_builtin decodes settings using the registers captured from the inverters
  (values only) and encodes `ac_charge ON` on 0xF355 -> 0xF3D5.
- Gateway test without the real broker: a local Mosquitto on the Mac (`/opt/homebrew/sbin/mosquitto`,
  no TLS) with the gateway's mqtt.host/port/tls pointed at it for the test, then restored; port 4 in
  loopback. Only with the user present.

## Tasks (in order)

1. modbus.c: RTU client framing, unit 0 answered; test_modbus.c and the modbus fuzz harness.
2. Settings, bridge, page and GATEWAY.md for `modbus_framing` and `modbus_broadcast=answer`;
   test_cfg, test_bridge (an RTU client through the bridge), test_store unchanged size.
   Checkpoint A (user present): install; switch ports 2 and 3; SolarAssistant still reads.
3. Control statements and their decoding (profile.c, pfdecode.c, device_value); tests; fuzz seed.
4. pfwrite.c and its test and fuzz target.
5. The write path in device.c, EV_WRITE, events stub, page event text; tests/test_devwrite.c with its
   own profile source (as test_devroom.c does).
6. MQTT and HA: commands, discovery, buffers, `/api/devices` parts; test_devmqtt.
7. The EG4 profile: settings and controls, rename; test_builtin; docs.
8. Gateway (user present): build, loader check, install; loopback + local Mosquitto; then the
   inverters: settings against the EG4 app, one write with the user watching.
