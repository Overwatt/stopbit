# Device profiles: follow-ups from phase 1

Recorded 2026-11-26 from the phase 1 reviews. None of these blocked the merge. The
first group is to settle before or during phase 2.

## Before or with phase 2

- Any device edit (label, controls) re-announces every entity of every device.
- A profile with 160 fields cannot publish: its state message exceeds the MQTT
  buffer. Settle the limit before uploads.
- An offline device is polled at its full rate; there is no backoff.
- LOST_MS can let a request be queued twice at extreme timeouts (8 queued × 10 s).
- A restore can fail at a batch boundary when two devices swap names or addresses.
- Fuzzing covers profile text, not random replies, `device_json` or the state
  message.
- LOADED=3 holds about 42 KB of D2 for two profile slots phase 1 never uses (D2
  has about 29 KB free).
- Line use is per port (`n540_port_line_use_ratio`); the spec asked for per device.
- json_str passes well-formed overlong and surrogate UTF-8 sequences, so they could
  reach discovery JSON. Only profile text carries them, which matters once profiles
  can be uploaded.

## MQTT and Home Assistant

- Pending removals (16) can fill while MQTT is down; a further removal is dropped.
- A removal accepted by lwIP but lost with the connection (QoS 0) is not redone.
- An oversized config or state message is skipped with no event or page reason.
- unique_id collisions are possible: device `last` with field `error` matches the
  gateway's `HOST_last_error`; `a_b` + `c` matches `a` + `b_c`.
- Turning MQTT off leaves the devices' entities in HA. The gateway's own entities
  are withdrawn.
- A change to `device.name` or `mqtt.control` re-sends every config;
  `announce=0` empties every config on each reconnect.

## Profiles and decoding

- A 0 mV cell shows 0.000 V where the ESPHome component leaves it unknown.
- One missing source makes a whole list calc unknown, including `count`.
- `pf_text` truncation can cut a bits text mid-UTF-8; a first name that does not
  fit gives "Unknown (N)" or "None".
- The parser accepts `c1..+5` as `1..5`. A partly resolvable list word counts as a
  single unit. `ps_t` is static, so the parser is not reentrant.
- `embed_profiles.py` does not check that an id matches its file name, that ids are
  unique, or the 16 KB limit.

## Web page and tools

- Device tables have no `<thead>`. Remove asks no confirmation, and after a failed
  remove its message stays styled as an error.
- After a successful Save and a failed refresh, the form stays open. Save
  recreates a device that was removed elsewhere while its form was open.
- Two `/api/settings` or `/api/devices` replies at once share one snapshot. JSON
  stays valid but may mix snapshots, except in one `/api/devices` race that can
  give `}{`.
- `n540.py`: a cut-short `/api/settings` gives a traceback; an empty file prints
  "loaded".

## Code and tests

- DTCM: 9 static `cfg_t` copies of 2976 B each; about 26 KB is left.
- `t[512]` and `v[520]` add about 1 KB to the main-loop stack and could be static.
- A device-only change logs no settings event.
- `fuzz/modbus.c` never calls `modbus_local`.
- Tests still missing:
  - parser limits (pool, blocks, nodata, ascii, rejects);
  - inf into `pf_num`;
  - calc operators other than eq;
  - removing a device, or changing its profile;
  - the order configs → status → state.
