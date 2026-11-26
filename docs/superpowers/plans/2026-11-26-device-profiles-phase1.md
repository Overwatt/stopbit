# Device Profiles, Phase 1 (EG4 BMS, reading only) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The gateway polls RS485 devices described by text profiles and publishes their values to MQTT with Home Assistant discovery; phase 1 ships the EG4 BMS profile and reads only.

**Architecture:** `profile.c` parses a profile into fixed tables; `pfdecode.c` turns a block's reply into values and computed values; `device.c` schedules block reads per port through `modbus.c`'s queue as a new local client; `devmqtt.c` publishes state, availability and discovery over `mqtt_ha.c`'s connection. Settings `devN.*` live in a record of their own. Large replies (`/api/devices`, `/api/settings`) go out part by part with `http.c`'s `do_parts`.

**Tech Stack:** C11 on STM32H750 (newlib nano: no float or 64-bit `printf`), lwIP 2.2 raw API and MQTT client, host tests with clang/gcc and ASan/UBSan (`tests/host.sh`), the gateway's own fuzzer (`tests/fuzz/run.sh`), Python 3 for build tools.

**Spec:** `docs/superpowers/specs/2026-11-26-device-profiles-design.md` (read it first).

## Global Constraints

- Never transmit on port 1 (TigoBus, listen-only); check it before and after every install (`serial_tx` stays 0, both clients back).
- No `%ll`, `%f`, `%g` or `%e` in firmware sources: `tests/host.sh` refuses `%ll`; numbers with decimals are written by `pf_num()`.
- Firmware installs: `cmake -B build && cmake --build build`, then `python3 tools/n540.py 10.20.1.210 update build/app.bin`; after 30 s: kept, crashes 0, TigoBus clients back, `serial_tx` 0, MQTT connected.
- Never commit `secrets/`, `dumps/`; grep diffs for `AKIA[A-Z0-9]{16}` before pushing.
- Commit trailers on every commit: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK`.
- Limits from the spec: profile text 16 KB, 16 blocks, 160 fields, names 40 characters, ids 24 of `a-z 0-9 _` (profile ids `a-z 0-9 -`), 16 devices.
- Phase 1 is reading only: no `number`/`select`/`switch`/`button` statements yet (the parser refuses them with "controls come in phase 2"), no commands over MQTT, no uploaded profiles (built-in only).
- Main loop: no job stacks; at most one reply decoded per port per pass.

## Deviations from the spec, to raise with the user at hand-off

1. **Uploaded profiles move to phase 2** (the spec's phase list leaves them out of phase 1; its "Profiles kept" section describes them). Phase 1 uses built-in profiles only.
2. **CI tests the MQTT payloads, not a live broker.** The transport is `mqtt_ha.c`'s lwIP client, already tested against Mosquitto; `tests/test_devmqtt.c` checks every topic and payload (well-formed JSON, required keys, sizes under the 4096-byte MQTT buffer).
3. **`reject B if zero f1 f2 ...`** (all listed fields zero) is added: EG4's second block needs it.
4. A `bits` text with no bit set reads `None`, as the ESPHome component shows it.
5. Changing the MQTT base topic does not clear devices' retained topics under the old base (the gateway's own are cleared as before).

## File Structure

| File | Responsibility |
|---|---|
| `src/app/modbus.{h,c}` (modify) | A local client: `modbus_local()`, replies to `io.local`, per-request timeout, `turn_us` quiet before requests |
| `src/app/profile.{h,c}` (new) | Profile text to `profile_t` tables; errors with line numbers |
| `src/app/pfdecode.{h,c}` (new) | Block replies to values (`pf_state_t`), calc, staleness drop, text of enum/bits/ascii, `pf_num()` number formatting |
| `profiles/eg4-bms.txt` (new) | The EG4 profile |
| `tools/embed_profiles.py` (new), `src/app/profiles.{h,c}` (new) | Built-in profiles embedded as text; lookup by id |
| `src/app/device.{h,c}` (new) | Devices from settings; per-port scheduling; replies; misses, staleness, online; JSON for `/api/devices` |
| `src/app/devmqtt.{h,c}` (new) | MQTT topics and payloads for devices: discovery, state, status, clearing |
| `src/app/cfg.{h,c}`, `src/app/cfg_store.c` (modify) | `dev_cfg_t`, `devN.*` keys, validation, a fourth side record at `0x0EE000`/`0x0EF000` |
| `src/app/bridge.{h,c}` (modify) | Modbus ports without a TCP listener; `bridge_modbus_port()`, `bridge_on_local()` |
| `src/app/mqtt_ha.c`, `src/app/api.{h,c}`, `src/app/main.c`, `src/http/http.c`, `src/app/metrics.{h,c}` (modify) | Wiring: device steps in the main loop, MQTT, `/api/devices`, `/api/settings` in parts, device metrics |
| `web/index.html` (modify) | Devices page |
| `tests/test_modbus.c`, `tests/test_profile.c`, `tests/test_pfdecode.c`, `tests/test_device.c`, `tests/test_devmqtt.c`, `tests/test_cfg.c`, `tests/test_store.c`, `tests/test_bridge.c`, `tests/fuzz/profile.c`, `tests/host.sh`, `tests/fuzz/run.sh` | Tests |
| `docs/GATEWAY.md`, `profiles/README.md` | User docs, profile format reference |

---

### Task 1: The local client in `modbus.c`

The engine queues its own requests in a port's Modbus queue. Their replies (or timeouts) go to a callback instead of a TCP client; each may carry its own timeout; a port may need extra quiet on the line before any request (EG4 packs: 50 ms).

**Files:**
- Modify: `src/app/modbus.h`, `src/app/modbus.c`, `src/app/bridge.c` (the `mb_io_t` initializer), `tests/fuzz/modbus.c` (the `mb_io_t` initializer)
- Test: `tests/test_modbus.c`

**Interfaces:**
- Produces:
  - `#define MB_LOCAL MB_CLIENTS` (client number of local requests)
  - `mb_io_t.local`: `void (*local)(void *ctx, uint32_t tag, int status, const uint8_t *pdu, size_t n)`; `status` 0 with the reply's PDU (function byte, then data; an exception is `fn | 0x80` and its code), or -1 for no valid reply in time
  - `bool modbus_local(modbus_t *m, uint8_t unit, const uint8_t *pdu, uint16_t len, uint16_t timeout_ms, uint32_t tag)` (false: queue full or bad length; timeout 0 = the port's)
  - `modbus_t.turn_us` (quiet before any request, on top of `gap_us`; set by `device.c`)

- [ ] **Step 1: Write the failing tests** — in `tests/test_modbus.c`, change the I/O to include a local callback and add the cases below just before the final `printf("modbus: ok, ...`:

```c
/* At the top, after reply(): */
static uint32_t Ltag[16]; static int Lst[16], Lk; static uint8_t Lpdu[16][260]; static size_t Lpn[16];
static bool again;                                          /* queue another from inside the callback */
static void local(void *ctx, uint32_t tag, int st, const uint8_t *pdu, size_t n)
{
    (void)ctx;
    Ltag[Lk] = tag; Lst[Lk] = st; Lpn[Lk] = n; if (n) memcpy(Lpdu[Lk], pdu, n); Lk++;
    if (again) { again = false; assert(modbus_local(&M, 2, (const uint8_t[]){ 3, 0, 1, 0, 1 }, 5, 0, 77)); }
}
static const mb_io_t IO = { line, reply, local, NULL };      /* replaces the old IO */
```

```c
    /* The gateway's own requests: replies to the callback, not to a client. */
    M.broadcast = false;
    Lk = 0;
    assert(modbus_local(&M, 0x10, (const uint8_t[]){ 3, 0x00, 0x00, 0x00, 0x02 }, 5, 300, 0x0102));
    poll(GAP); assert(on_linec("10 03 00 00 00 02")); poll(GAP);
    hearc("10 03 04 14 c0 ff 85");
    assert(Lk == 1 && Lst[0] == 0 && Ltag[0] == 0x0102 && Lpn[0] == 6 && !memcmp(Lpdu[0], (const uint8_t[]){ 3, 4, 0x14, 0xc0, 0xff, 0x85 }, 6));
    assert(An[0] == 0 && An[1] == 0);                       /* no client heard of it */
    /* Its own timeout: 300 ms, not the port's 1000. */
    assert(modbus_local(&M, 0x10, (const uint8_t[]){ 3, 0, 0, 0, 2 }, 5, 300, 9));
    poll(GAP); Ln = 0; poll(GAP);
    now += 299; poll(GAP); assert(Lk == 1);
    now += 1; poll(GAP); assert(Lk == 2 && Lst[1] == -1 && Ltag[1] == 9 && An[0] == 0);
    /* An exception reaches the callback as it is. */
    assert(modbus_local(&M, 0x10, (const uint8_t[]){ 3, 0, 0x99, 0, 1 }, 5, 0, 10));
    poll(GAP); Ln = 0; poll(GAP);
    hearc("10 83 02");
    assert(Lk == 3 && Lst[2] == 0 && Lpn[2] == 2 && Lpdu[2][0] == 0x83 && Lpdu[2][1] == 2);
    /* Turnaround: quiet for turn_us before any request, a client's too. */
    M.turn_us = 50000;
    assert(ask(0, "00 50 00 00 00 06 01 03 00 00 00 01"));
    poll(GAP); assert(Ln == 0);
    poll(49999); assert(Ln == 0);
    poll(50000); assert(on_linec("01 03 00 00 00 01"));
    poll(GAP); hearc("01 03 02 00 2a");
    assert(to(0, "00 50 00 00 00 05 01 03 02 00 2a"));
    M.turn_us = 0;
    /* A callback may queue the next one. */
    again = true;
    assert(modbus_local(&M, 1, (const uint8_t[]){ 3, 0, 0, 0, 1 }, 5, 0, 76));
    poll(GAP); Ln = 0; poll(GAP);
    hearc("01 03 02 00 2a");
    assert(Lk == 4 && Ltag[3] == 76 && M.qn == 1 && M.q[0].tag == 77);
    poll(GAP); assert(on_linec("02 03 00 01 00 01"));
    poll(GAP); hearc("02 03 02 00 01");
    assert(Lk == 5 && Ltag[4] == 77);
    /* Full: refused. A bad length: refused. */
    for (int i = 0; i < MB_QUEUE; i++) assert(modbus_local(&M, 1, (const uint8_t[]){ 3, 0, 0, 0, 1 }, 5, 0, 0));
    assert(!modbus_local(&M, 1, (const uint8_t[]){ 3, 0, 0, 0, 1 }, 5, 0, 0));
    assert(!modbus_local(&M, 1, (const uint8_t[]){ 3 }, 0, 0, 0) && !modbus_local(&M, 1, (const uint8_t[]){ 3 }, MB_PDU + 1, 0, 0));
    modbus_init(&M, &IO, 1000, GAP);                        /* empty again */
```

- [ ] **Step 2: Run to verify it fails**

Run: `cc -g -fsanitize=address,undefined -Isrc/app tests/test_modbus.c src/app/modbus.c -o /tmp/mb && /tmp/mb`
Expected: build errors: `mb_io_t` has no member `local`, `modbus_local` undeclared, no member `turn_us`/`tag`.

- [ ] **Step 3: Implement** — in `src/app/modbus.h`:

```c
#define MB_LOCAL    MB_CLIENTS  /* the gateway's own requests (device.c): replies go to io.local */
```

Replace `mb_io_t` with:

```c
typedef struct {
    size_t (*line)(void *ctx, const uint8_t *d, size_t n);          /* all of it, or 0: not now */
    void   (*reply)(void *ctx, int client, const uint8_t *d, size_t n);
    /* A local request ended: status 0 and the reply's PDU (function, then data; an
     * exception is function | 0x80 and its code), or -1: no valid reply in time. */
    void   (*local)(void *ctx, uint32_t tag, int status, const uint8_t *pdu, size_t n);
    void   *ctx;
} mb_io_t;
```

In `mb_req_t`, after `uint16_t tid, len;` add:

```c
    uint16_t timeout_ms;        /* 0: the port's */
    uint32_t tag;               /* a local request's, given back with its reply */
```

In `modbus_t`, after `bool broadcast;` add:

```c
    uint32_t turn_us;           /* quiet before any request, beyond gap_us (device.c sets it) */
```

And declare, after `modbus_from_client`:

```c
/* The gateway's own request: unit, PDU (function and data). Its reply or timeout goes to
 * io.local with `tag`. False if 8 wait already, or the PDU is empty or too long. */
bool modbus_local(modbus_t *m, uint8_t unit, const uint8_t *pdu, uint16_t len, uint16_t timeout_ms, uint32_t tag);
```

In `src/app/modbus.c`: `exception()` must ignore the local client — change its guard to `if (c >= 0 && c < MB_CLIENTS && m->cl[c].open)`. In `take()`, after `r->len = plen;` add `r->timeout_ms = 0; r->tag = 0;`. Add after `modbus_from_client()`:

```c
bool modbus_local(modbus_t *m, uint8_t unit, const uint8_t *pdu, uint16_t len, uint16_t timeout_ms, uint32_t tag)
{
    if (!len || len > MB_PDU || m->qn == MB_QUEUE) return false;
    mb_req_t *r = &m->q[m->qn++];
    r->client = MB_LOCAL;
    r->unit = unit;
    r->tid = 0;
    r->len = len;
    r->timeout_ms = timeout_ms;
    r->tag = tag;
    memcpy(r->pdu, pdu, len);
    m->s.requests++;
    return true;
}
```

In `modbus_poll()`, `case MB_IDLE`: replace `if (!m->qn || !tx_idle || quiet_us < m->gap_us) return;` with:

```c
        uint32_t quiet = m->turn_us > m->gap_us ? m->turn_us : m->gap_us;
        if (!m->qn || !tx_idle || quiet_us < quiet) return;
```

In `case MB_WAITING`, the reply branch — replace from `m->s.replies++;` to `return;` with (the callback runs after `done()`, so it may queue the next request):

```c
                m->s.replies++;
                if (m->rx[1] & 0x80) m->s.exceptions++;
                m->s.last_ms = ms;
                if (ms > m->s.max_ms) m->s.max_ms = ms;
                m->s.total_ms += ms;
                static const uint32_t B[MB_HIST] = MB_BOUNDS_MS;
                int b = 0;
                while (b < MB_HIST && ms > B[b]) b++;
                m->s.hist[b]++;
                if (r->client == MB_LOCAL) {                /* a[7..]: function and data, plen bytes */
                    uint32_t tag = r->tag;
                    done(m);
                    if (m->io.local) m->io.local(m->io.ctx, tag, 0, a + 7, plen);
                    return;
                }
                if (r->client >= 0 && m->cl[r->client].open) m->io.reply(m->io.ctx, r->client, a, 7u + plen);
                done(m);
                return;
```

(`a` is the MBAP reply already built above: `a[7]` is the function byte, `plen` the length of function and data. `done()` clears `rx`, not `a`, so `a` is still valid for the callback.)

And the timeout — replace the block starting `if (now - m->at >= m->timeout_ms) {` with:

```c
        if (now - m->at >= (r->timeout_ms ? r->timeout_ms : m->timeout_ms)) {
            m->s.timeouts++;
            if (r->client == MB_LOCAL) {
                uint32_t tag = r->tag;
                done(m);
                if (m->io.local) m->io.local(m->io.ctx, tag, -1, NULL, 0);
                return;
            }
            exception(m, r->client, r->tid, r->unit, r->pdu[0], EX_NO_RESPONSE);
            done(m);
        }
```

In `src/app/bridge.c` `mb_start()`: `const mb_io_t io = { mb_line, mb_reply, NULL, &s_p[i] };` (Task 7 fills the third). In `tests/fuzz/modbus.c`: `static const mb_io_t IO = { line, reply, NULL, NULL };`.

- [ ] **Step 4: Run the tests**

Run: `bash tests/host.sh 2>&1 | grep -v ': ok'` and `CC=clang FUZZ_OUT=/tmp/fz tests/fuzz/run.sh 30 modbus`
Expected: only the informational `cfg:`/`cli:`/`history:`/`modbus:` lines; the fuzzer `0 failures`.

- [ ] **Step 5: Commit**

```bash
git add src/app/modbus.h src/app/modbus.c src/app/bridge.c tests/test_modbus.c tests/fuzz/modbus.c
git commit -m "Modbus: a local client for the gateway's own requests

Replies (or timeouts) go to io.local with the request's tag; each may
have its own timeout; turn_us keeps the line quiet before any request.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK"
```

### Task 2: The profile parser

**Files:**
- Create: `src/app/profile.h`, `src/app/profile.c`, `tests/test_profile.c`, `tests/fuzz/profile.c`, `tests/fuzz/seeds/profile/minimal`
- Modify: `tests/host.sh` (one `t` line), `tests/fuzz/run.sh` (a target)

**Interfaces:**
- Produces (exactly as in `profile.h` below): `profile_t`, `pf_block_t`, `pf_field_t`, `pf_reject_t`, `pf_vname_t`; enums `PF_SENSOR..PF_CALCBIN`, `T_U16..T_ASCIILO`, `OP_EQ..OP_LT`, `F_SUM..F_NOBITS`, `RJ_ZERO..RJ_CMP`; `bool profile_parse(profile_t *p, const char *text, size_t n, char *err, size_t errlen)`; `int profile_field(const profile_t *p, const char *id)`; `int profile_block(const profile_t *p, const char *id)`; `const char *pf_str(const profile_t *p, uint16_t off)`.

- [ ] **Step 1: Create `src/app/profile.h`**

```c
/*
 * profile.h — A device profile: what to read from a device on an RS485 port, and how to
 * show it. profile_parse() turns the text (docs/superpowers/specs/
 * 2026-11-26-device-profiles-design.md, "Profile format") into these tables;
 * pfdecode.c uses them. Strings live in `pool`, referred to by offset (0 is "").
 */
#ifndef PROFILE_H
#define PROFILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PF_TEXT_MAX  16384u
#define PF_BLOCKS    16
#define PF_FIELDS    160
#define PF_REJECTS   16
#define PF_VNAMES    256         /* enum values' and bits' names */
#define PF_ARGS      512         /* calc arguments */
#define PF_POOL      6144        /* ids, names, units, HA classes */
#define PF_STRS      8           /* ascii texts */
#define PF_STR_LEN   49          /* up to 24 registers of text, and the NUL */
#define PF_NAME_MAX  40
#define PF_ID_MAX    24

enum { PF_SENSOR, PF_BINARY, PF_ENUM, PF_BITS, PF_ASCII, PF_CALC, PF_CALCBIN };
enum { T_U16, T_S16, T_U32, T_S32, T_U32LE, T_S32LE, T_U8H, T_U8L, T_S8H, T_S8L, T_BIT, T_COIL, T_ASCII, T_ASCIILO };
enum { OP_EQ, OP_NE, OP_GT, OP_LT };
enum { F_SUM, F_MIN, F_MAX, F_AVG, F_DELTA, F_ARGMIN, F_ARGMAX, F_COUNT, F_SUB, F_MUL, F_POS, F_NEG, F_ABS,
       F_EQ, F_NE, F_GT, F_LT, F_ANYBITS, F_NOBITS };        /* F_EQ..F_LT in OP_ order */
enum { RJ_ZERO, RJ_ZEROS, RJ_CMP };                        /* the block all 0; listed fields all 0; a comparison */
enum { PFF_DIAG = 1, PFF_WITHIN = 2 };

typedef struct {
    uint16_t id;                 /* pool */
    uint8_t  fn;                 /* Modbus function: 1 coils, 2 discrete inputs, 3 holding, 4 input registers */
    bool     info;               /* not needed for "online" */
    uint16_t addr, count;        /* registers, or bits for 1 and 2 */
    uint32_t period_ms;
} pf_block_t;

typedef struct {
    uint16_t id, name, unit, dc, sc;   /* pool; 0: none */
    uint8_t  kind, type, block, bit;   /* block 0xFF for calc; bit: T_BIT's bit, or T_ASCII*'s registers */
    uint16_t addr;
    int8_t   prec;                     /* decimals shown; -1: from the scale */
    uint8_t  flags, op, fn;            /* op: a binary's comparison; fn: a calc's function */
    uint8_t  nnodata, str;             /* str: PF_ASCII's slot in pf_state_t */
    uint16_t vname, nvname;            /* PF_ENUM, PF_BITS: their value names in vname[] */
    uint16_t arg, narg;                /* PF_CALC*: field indexes in arg[] */
    int32_t  offset;
    uint32_t mask, nodata[4];          /* mask: an enum's, or anybits/nobits' */
    float    scale, k, lo, hi;         /* k: comparison constant; lo, hi: within */
} pf_field_t;

typedef struct { uint8_t block, kind, op, nfield; uint16_t field[4]; float k; } pf_reject_t;
typedef struct { uint32_t value; uint16_t name; } pf_vname_t;

typedef struct {
    uint16_t id, name, maker;          /* pool */
    uint32_t baud;
    char     frame[4];                 /* "8N1" */
    uint8_t  stale;
    uint16_t timeout_ms, turn_ms;
    uint8_t  nblock, nreject, nstr;
    uint16_t nfield, nvname, narg, npool;
    pf_block_t  block[PF_BLOCKS];
    pf_field_t  field[PF_FIELDS];
    pf_reject_t reject[PF_REJECTS];
    pf_vname_t  vname[PF_VNAMES];
    uint16_t    arg[PF_ARGS];
    char        pool[PF_POOL];
} profile_t;

static inline const char *pf_str(const profile_t *p, uint16_t off) { return p->pool + off; }

/* The text into *p. False, with "line N: why" in err (or just "why"), if it will not do. */
bool profile_parse(profile_t *p, const char *text, size_t n, char *err, size_t errlen);
int  profile_field(const profile_t *p, const char *id);    /* index, or -1 */
int  profile_block(const profile_t *p, const char *id);    /* index, or -1 */

#endif
```

- [ ] **Step 2: Write the failing test `tests/test_profile.c`**

```c
/* test_profile.c — profile text to tables (src/app/profile.c): what each statement makes,
 * and that each mistake is refused with its line.
 *   cc -Isrc/app tests/test_profile.c src/app/profile.c -o /tmp/pf && /tmp/pf */
#include "profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static profile_t P;
static char err[160];

static bool parse(const char *t) { return profile_parse(&P, t, strlen(t), err, sizeof err); }
static void bad(const char *t, const char *want)
{
    if (parse(t) || !strstr(err, want)) { printf("text:\n%s\nerror: \"%s\"\nwanted: \"%s\"\n", t, err, want); assert(0); }
}
static const pf_field_t *F(const char *id) { int i = profile_field(&P, id); assert(i >= 0); return &P.field[i]; }

#define HEAD "profile t 1\nblock b hold 0x10 20 10s\n"

int main(void)
{
    /* Everything once. */
    const char *ok =
        "# a test device\n"
        "profile test-dev 1\n"
        "name \"Test device\"\n"
        "maker \"ACME\"\n"
        "line 19200 8E1\n"
        "stale 3\n"
        "timeout 500ms\n"
        "turnaround 50ms\n"
        "block live hold 0x0000 18 10s\n"
        "block more input 100 4 2m\n"
        "block flags coil 0 16 1h info\n"
        "sensor volts \"Total voltage\" live 0x0000 u16 *0.01 V dc=voltage sc=measurement prec=2\n"
        "sensor amps \"Current\" live 1 s16 *0.01 +-5 A\n"
        "sensor cell1..16 \"Cell #\" live 2 u16 *0.001 V nodata 0xFFFF\n"
        "sensor big \"Energy\" more 100 u32le kWh sc=total_increasing diag\n"
        "binary heat \"Heating\" live 0x11 bit 15\n"
        "binary busy \"Busy\" more 102 u16 == 5 dc=running\n"
        "binary coil3 \"Coil 3\" flags 3 coil\n"
        "text state \"Status\" live 0x11 enum mask=0x000F 0=Standby 1=Charging 8=\"Charging limited\"\n"
        "text warn \"Warnings\" more 103 bits 0=\"Pack OV\" 13=\"Float stopped\" diag\n"
        "text model \"Model\" more 100 ascii 4\n"
        "calc power \"Power\" mul volts amps W dc=power\n"
        "calc low \"Lowest cell\" argmin cell1..16 within 0.001..4.999\n"
        "calc ok \"Charging allowed\" nobits warn 0x2001\n"
        "reject live if zero\n"
        "reject live if zero volts amps\n"
        "reject more if big > 100\n";
    if (!parse(ok)) { puts(err); assert(0); }
    assert(!strcmp(pf_str(&P, P.id), "test-dev") && !strcmp(pf_str(&P, P.name), "Test device") && !strcmp(pf_str(&P, P.maker), "ACME"));
    assert(P.baud == 19200 && !strcmp(P.frame, "8E1") && P.stale == 3 && P.timeout_ms == 500 && P.turn_ms == 50);
    assert(P.nblock == 3 && P.block[0].fn == 3 && P.block[0].count == 18 && P.block[0].period_ms == 10000);
    assert(P.block[1].fn == 4 && P.block[1].addr == 100 && P.block[1].period_ms == 120000 && !P.block[1].info);
    assert(P.block[2].fn == 1 && P.block[2].period_ms == 3600000 && P.block[2].info);
    const pf_field_t *f = F("volts");
    assert(f->kind == PF_SENSOR && f->type == T_U16 && f->block == 0 && f->addr == 0 && f->scale == 0.01f && f->prec == 2);
    assert(!strcmp(pf_str(&P, f->unit), "V") && !strcmp(pf_str(&P, f->dc), "voltage") && !strcmp(pf_str(&P, f->sc), "measurement"));
    assert(F("amps")->offset == -5 && F("amps")->type == T_S16 && F("amps")->prec == -1);
    for (int k = 1; k <= 16; k++) {
        char id[16], nm[16]; snprintf(id, sizeof id, "cell%d", k); snprintf(nm, sizeof nm, "Cell %d", k);
        f = F(id);
        assert(f->addr == 1 + k && !strcmp(pf_str(&P, f->name), nm) && f->nnodata == 1 && f->nodata[0] == 0xFFFF);
    }
    assert(F("big")->type == T_U32LE && (F("big")->flags & PFF_DIAG));
    assert(F("heat")->kind == PF_BINARY && F("heat")->type == T_BIT && F("heat")->bit == 15);
    assert(F("busy")->op == OP_EQ && F("busy")->k == 5.0f && F("coil3")->type == T_COIL && F("coil3")->addr == 3);
    f = F("state");
    assert(f->kind == PF_ENUM && f->mask == 0xF && f->nvname == 3 && P.vname[f->vname + 2].value == 8 &&
           !strcmp(pf_str(&P, P.vname[f->vname + 2].name), "Charging limited"));
    assert(F("warn")->kind == PF_BITS && F("warn")->nvname == 2 && P.vname[F("warn")->vname + 1].value == 13);
    assert(F("model")->kind == PF_ASCII && F("model")->type == T_ASCII && F("model")->bit == 4 && F("model")->str == 0);
    f = F("power");
    assert(f->kind == PF_CALC && f->fn == F_MUL && f->narg == 2 && P.arg[f->arg] == profile_field(&P, "volts") && f->block == 0xFF);
    f = F("low");
    assert(f->fn == F_ARGMIN && f->narg == 16 && (f->flags & PFF_WITHIN) && f->lo == 0.001f && f->hi == 4.999f);
    f = F("ok");
    assert(f->kind == PF_CALCBIN && f->fn == F_NOBITS && f->mask == 0x2001);
    assert(P.nreject == 3 && P.reject[0].kind == RJ_ZERO && P.reject[1].kind == RJ_ZEROS && P.reject[1].nfield == 2 &&
           P.reject[2].kind == RJ_CMP && P.reject[2].op == OP_GT && P.reject[2].k == 100.0f && P.reject[2].block == 1);

    /* Mistakes, each with its line. */
    bad("", "the profile is empty");
    bad("# nothing\n\n", "the profile is empty");
    bad("name \"x\"\n", "line 1: the first statement must be: profile id 1");
    bad("profile EG4 1\n", "line 1: bad profile id 'EG4'");
    bad("profile eg4 2\n", "line 1: format version 2 is not known; this firmware reads 1");
    bad("profile t 1\n", "no blocks");
    bad("profile t 1\nprofile t 1\n", "line 2: profile is given twice");
    bad("profile t 1\nname x\n", "line 2: the name must be \"quoted\", 1 to 40 characters");
    bad("profile t 1\nline 9600 8X1\n", "line 2: bad line settings '8X1'");
    bad("profile t 1\ntimeout 10ms\n", "line 2: timeout must be 50ms to 10000ms");
    bad("profile t 1\nblock b hold 0 0 10s\n", "line 2: a block of holding or input registers counts 1 to 125");
    bad("profile t 1\nblock b hold 0 126 10s\n", "line 2: a block of holding or input registers counts 1 to 125");
    bad("profile t 1\nblock b fifo 0 2 10s\n", "line 2: unknown block kind 'fifo'");
    bad("profile t 1\nblock b hold 0 2 10x\n", "line 2: bad period '10x'");
    bad(HEAD "block b hold 0 2 10s\n", "line 3: 'b' is declared twice");
    bad(HEAD "sensor x \"X\" nope 0x10 u16\n", "line 3: no block 'nope'");
    bad(HEAD "sensor x \"X\" b 0x10 u17\n", "line 3: unknown type 'u17'");
    bad(HEAD "sensor x \"X\" b 0x0F u16\n", "line 3: 0x000f is outside block b");
    bad(HEAD "sensor x \"X\" b 0x23 u32\n", "line 3: 0x0023 is outside block b");
    bad(HEAD "sensor x X b 0x10 u16\n", "line 3: the name must be \"quoted\"");
    bad(HEAD "sensor x \"12345678901234567890123456789012345678901\" b 0x10 u16\n", "line 3: the name must be \"quoted\", 1 to 40");
    bad(HEAD "sensor X \"X\" b 0x10 u16\n", "line 3: bad id 'X': 1 to 24 of a-z 0-9 _");
    bad(HEAD "sensor x \"X\" b 0x10 u16\nsensor x \"Y\" b 0x11 u16\n", "line 4: 'x' is declared twice");
    bad(HEAD "sensor x \"X\" b 0x10 u16 *zz\n", "line 3: bad scale '*zz'");
    bad(HEAD "sensor x \"X\" b 0x10 u16 V A\n", "line 3: unknown word 'A'");
    bad(HEAD "sensor c1..4 \"Cell\" b 0x10 u16\n", "line 3: a list's name needs # for its number");
    bad(HEAD "sensor x \"X\" b 0x10 coil\n", "line 3: coils are read from a coil or discrete block");
    bad(HEAD "binary x \"X\" b 0x10 bit 16\n", "line 3: bit must be 0 to 15");
    bad(HEAD "binary x \"X\" b 0x10 u16 >= 2\n", "line 3: unknown comparison '>='");
    bad(HEAD "text x \"X\" b 0x10 enum\n", "line 3: enum needs at least one value=name");
    bad(HEAD "text x \"X\" b 0x10 bits 32=\"Y\"\n", "line 3: bad value '32'");
    bad(HEAD "text x \"X\" b 0x10 ascii 25\n", "line 3: ascii needs a length of 1 to 24 registers");
    bad(HEAD "text x \"X\" b 0x10 thing\n", "line 3: text needs enum, bits, ascii or asciilo");
    bad(HEAD "sensor x \"X\" b 0x10 u16\ncalc y \"Y\" frob x\n", "line 4: unknown function 'frob'");
    bad(HEAD "calc y \"Y\" pos nope\n", "line 3: no field 'nope'");
    bad(HEAD "sensor x \"X\" b 0x10 u16\ncalc y \"Y\" sub x\n", "line 4: sub needs 2 fields");
    bad(HEAD "sensor x \"X\" b 0x10 u16\ncalc y \"Y\" pos x within 0..1\n", "line 4: unknown word 'within'");
    bad("profile t 1\nblock b hold 0 2 10s\nblock c hold 2 2 10s\nsensor x \"X\" c 2 u16\nreject b if x == 0\n",
        "line 5: 'x' is not in block b");
    bad(HEAD "reject b when zero\n", "line 3: reject needs: block if zero [fields], or block if field op value");
    bad(HEAD "number n \"N\" reg 0x10 u16 min 0 max 9 step 1\n", "line 3: controls come in phase 2");
    bad(HEAD "sensor x \"X b 0x10 u16\n", "line 3: a quote is not closed");
    bad(HEAD "frobnicate\n", "line 3: unknown statement 'frobnicate'");
    {
        static char t[2048];
        snprintf(t, sizeof t, HEAD "sensor x \"%0600d\" b 0x10 u16\n", 0);
        bad(t, "line 3: longer than 512 characters");
        snprintf(t, sizeof t, HEAD "sensor x \"\x01\" b 0x10 u16\n");
        bad(t, "line 3: a control character");
    }
    {   /* Too many fields: 161. */
        static char t[16384];
        size_t n = (size_t)snprintf(t, sizeof t, "profile t 1\nblock b hold 0 125 10s\nblock c hold 125 125 10s\n");
        for (int k = 0; k <= PF_FIELDS; k++)
            n += (size_t)snprintf(t + n, sizeof t - n, "sensor f%d \"F\" %s %d u16\n", k, k < 125 ? "b" : "c", k);
        bad(t, "line 164: too many fields (160)");
    }
    {   /* Longer than 16 KB. */
        static char t[PF_TEXT_MAX + 2];
        memset(t, '#', sizeof t - 1);
        t[sizeof t - 1] = 0;
        bad(t, "the profile is longer than 16384 bytes");
    }
    puts("profile: ok");
    return 0;
}
```

- [ ] **Step 3: Run it to see it fail**

Run: `cc -g -Isrc/app tests/test_profile.c src/app/profile.c -o /tmp/pf`
Expected: fails to link: `profile.c` does not exist.

- [ ] **Step 4: Create `src/app/profile.c`**

```c
/*
 * profile.c — A device profile's text into tables (profile.h). One statement a line;
 * `#` starts a comment; words are split by spaces, "quoted" ones kept whole. Errors name
 * the line. No I/O, no allocation: fuzzed (tests/fuzz/profile.c).
 */
#include "profile.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX 512
#define TOK_MAX  96

typedef struct {
    profile_t *p;
    char *err;
    size_t errlen;
    int line;
    int ntok;
    char *tok[TOK_MAX];
    char buf[LINE_MAX + 1];
} ps_t;

static bool fail(ps_t *s, const char *fmt, ...)
{
    va_list a;
    int k = 0;
    if (!s->errlen) return false;
    if (s->line) k = snprintf(s->err, s->errlen, "line %d: ", s->line);
    va_start(a, fmt);
    if (k >= 0 && (size_t)k < s->errlen) vsnprintf(s->err + k, s->errlen - (size_t)k, fmt, a);
    va_end(a);
    return false;
}

/* s->buf into words. A "quoted" word keeps its quotes, and k="v w" is one word. */
static bool tokens(ps_t *s)
{
    char *c = s->buf;
    s->ntok = 0;
    for (;;) {
        while (*c == ' ' || *c == '\t' || *c == '\r') c++;
        if (!*c || *c == '#') return true;
        if (s->ntok == TOK_MAX) return fail(s, "too many words");
        s->tok[s->ntok++] = c;
        bool q = false;
        while (*c && (q || (*c != ' ' && *c != '\t' && *c != '\r' && *c != '#'))) {
            if (*c == '"') q = !q;
            c++;
        }
        if (q) return fail(s, "a quote is not closed");
        if (*c == '#') { *c = 0; return true; }
        if (*c) *c++ = 0;
    }
}

/* The word without its quotes; NULL if quoted badly. */
static char *unq(char *t)
{
    size_t n = strlen(t);
    if (t[0] != '"') return strchr(t, '"') ? NULL : t;
    if (n < 2 || t[n - 1] != '"' || memchr(t + 1, '"', n - 2)) return NULL;
    t[n - 1] = 0;
    return t + 1;
}

/* Decimal or 0x hex, with a sign; within [lo, hi]. */
static bool num_i(const char *t, int64_t lo, int64_t hi, int64_t *out)
{
    const char *d = t + (*t == '-' || *t == '+');
    char *e;
    if (!*d) return false;
    long long v = strtoll(t, &e, d[0] == '0' && (d[1] == 'x' || d[1] == 'X') ? 16 : 10);
    if (*e || e == t || v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* [-]digits[.digits] */
static bool num_f(const char *t, float *out)
{
    double v = 0, f = 1;
    bool neg = *t == '-', any = false;
    if (*t == '-' || *t == '+') t++;
    while (isdigit((unsigned char)*t)) { v = v * 10 + (*t++ - '0'); any = true; }
    if (*t == '.') for (t++; isdigit((unsigned char)*t); t++) { f /= 10; v += (*t - '0') * f; any = true; }
    if (!any || *t) return false;
    *out = (float)(neg ? -v : v);
    return true;
}

/* A string into the pool, shared with an equal one already there. */
static bool pool_add(ps_t *s, const char *str, uint16_t *off)
{
    profile_t *p = s->p;
    size_t n = strlen(str) + 1;
    if (n == 1) { *off = 0; return true; }
    for (size_t i = 1; i + n <= p->npool; i += strlen(p->pool + i) + 1)
        if (!strcmp(p->pool + i, str)) { *off = (uint16_t)i; return true; }
    if (p->npool + n > PF_POOL) return fail(s, "the profile has too much text (%d bytes)", PF_POOL);
    memcpy(p->pool + p->npool, str, n);
    *off = p->npool;
    p->npool = (uint16_t)(p->npool + n);
    return true;
}

static bool id_ok(const char *t)
{
    size_t n = strlen(t);
    if (!n || n > PF_ID_MAX) return false;
    for (; *t; t++) if (!((*t >= 'a' && *t <= 'z') || (*t >= '0' && *t <= '9') || *t == '_')) return false;
    return true;
}

static bool word_ok(const char *t)       /* an HA class: a-z and _ */
{
    size_t n = strlen(t);
    if (!n || n > 32) return false;
    for (; *t; t++) if (!((*t >= 'a' && *t <= 'z') || *t == '_')) return false;
    return true;
}

int profile_block(const profile_t *p, const char *id)
{
    for (int i = 0; i < p->nblock; i++) if (!strcmp(pf_str(p, p->block[i].id), id)) return i;
    return -1;
}

int profile_field(const profile_t *p, const char *id)
{
    for (int i = 0; i < p->nfield; i++) if (!strcmp(pf_str(p, p->field[i].id), id)) return i;
    return -1;
}

static const char *name_of(ps_t *s, int i)
{
    char *n = i < s->ntok && s->tok[i][0] == '"' ? unq(s->tok[i]) : NULL;
    if (!n || !*n || strlen(n) > PF_NAME_MAX) { fail(s, "the name must be \"quoted\", 1 to %d characters", PF_NAME_MAX); return NULL; }
    return n;
}

static const struct { const char *name; uint8_t type, regs; } TYPES[] = {
    { "u16", T_U16, 1 }, { "s16", T_S16, 1 }, { "u32", T_U32, 2 }, { "s32", T_S32, 2 },
    { "u32le", T_U32LE, 2 }, { "s32le", T_S32LE, 2 }, { "u8h", T_U8H, 1 }, { "u8l", T_U8L, 1 },
    { "s8h", T_S8H, 1 }, { "s8l", T_S8L, 1 }, { "coil", T_COIL, 0 },
};

static int type_of(const char *t)
{
    for (unsigned i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) if (!strcmp(TYPES[i].name, t)) return (int)i;
    return -1;
}

static int op_of(const char *t)
{
    return !strcmp(t, "==") ? OP_EQ : !strcmp(t, "!=") ? OP_NE : !strcmp(t, ">") ? OP_GT : !strcmp(t, "<") ? OP_LT : -1;
}

/* "cell1..16": base "cell", from 1, to 16. False if not a list. */
static bool range(const char *t, char *base, size_t bmax, int *from, int *to)
{
    const char *dd = strstr(t, ".."), *d;
    char *e;
    if (!dd) return false;
    for (d = dd; d > t && isdigit((unsigned char)d[-1]); d--) { }
    if (d == dd || d == t || (size_t)(d - t) >= bmax) return false;
    memcpy(base, t, (size_t)(d - t));
    base[d - t] = 0;
    *from = (int)strtol(d, NULL, 10);
    *to = (int)strtol(dd + 2, &e, 10);
    return !*e && e != dd + 2 && *to >= *from && *to - *from < 64;
}

static pf_field_t *field_new(ps_t *s, const char *id, const char *name)
{
    profile_t *p = s->p;
    if (!id_ok(id)) { fail(s, "bad id '%s': 1 to %d of a-z 0-9 _", id, PF_ID_MAX); return NULL; }
    if (profile_field(p, id) >= 0) { fail(s, "'%s' is declared twice", id); return NULL; }
    if (p->nfield == PF_FIELDS) { fail(s, "too many fields (%d)", PF_FIELDS); return NULL; }
    pf_field_t *f = &p->field[p->nfield];
    memset(f, 0, sizeof *f);
    f->scale = 1;
    f->prec = -1;
    f->mask = 0xFFFFFFFFu;
    f->block = 0xFF;
    if (!pool_add(s, id, &f->id) || !pool_add(s, name, &f->name)) return NULL;
    p->nfield++;
    return f;
}

/* Is the field inside its block? regs: registers it spans; 0 for a coil. */
static bool place(ps_t *s, const pf_field_t *f, int regs)
{
    const pf_block_t *b = &s->p->block[f->block];
    bool bits = b->fn <= 2;
    if (bits && regs) return fail(s, "block %s holds coils only", pf_str(s->p, b->id));
    if (!bits && !regs) return fail(s, "coils are read from a coil or discrete block");
    if (f->addr < b->addr || (uint32_t)f->addr + (uint32_t)(regs ? regs : 1) > (uint32_t)b->addr + b->count)
        return fail(s, "0x%04x is outside block %s", f->addr, pf_str(s->p, b->id));
    return true;
}

enum { M_SCALE = 1, M_UNIT = 2, M_HA = 4, M_DIAG = 8, M_WITHIN = 16, M_NODATA = 32 };

/* A word with a meaning of its own: never taken for a unit where it is not allowed. */
static bool keyword(const char *t)
{
    return t[0] == '*' || t[0] == '+' || !strcmp(t, "nodata") || !strcmp(t, "diag") || !strcmp(t, "within") ||
           !strncmp(t, "dc=", 3) || !strncmp(t, "sc=", 3) || !strncmp(t, "prec=", 5);
}

/* The words after a field's own: scale, offset, unit, HA hints, ... */
static bool mods(ps_t *s, pf_field_t *f, int from, unsigned allow)
{
    bool unit = false;
    for (int i = from; i < s->ntok; i++) {
        char *t = s->tok[i];
        int64_t l;
        float x;
        if (t[0] == '*' && (allow & M_SCALE)) {
            if (!num_f(t + 1, &x) || x == 0) return fail(s, "bad scale '%s'", t);
            f->scale = x;
        } else if (t[0] == '+' && (allow & M_SCALE)) {
            if (!num_i(t + 1, -2000000000LL, 2000000000LL, &l)) return fail(s, "bad offset '%s'", t);
            f->offset = (int32_t)l;
        } else if (!strcmp(t, "nodata") && (allow & M_NODATA)) {
            while (i + 1 < s->ntok && num_i(s->tok[i + 1], 0, 0xFFFFFFFFLL, &l)) {
                if (f->nnodata == 4) return fail(s, "at most 4 nodata values");
                f->nodata[f->nnodata++] = (uint32_t)l;
                i++;
            }
            if (!f->nnodata) return fail(s, "nodata needs values");
        } else if (!strcmp(t, "diag") && (allow & M_DIAG)) {
            f->flags |= PFF_DIAG;
        } else if (!strncmp(t, "dc=", 3) && (allow & M_HA)) {
            if (!word_ok(t + 3)) return fail(s, "bad device class '%s'", t + 3);
            if (!pool_add(s, t + 3, &f->dc)) return false;
        } else if (!strncmp(t, "sc=", 3) && (allow & M_HA)) {
            if (strcmp(t + 3, "measurement") && strcmp(t + 3, "total") && strcmp(t + 3, "total_increasing"))
                return fail(s, "state class is measurement, total or total_increasing");
            if (!pool_add(s, t + 3, &f->sc)) return false;
        } else if (!strncmp(t, "prec=", 5) && (allow & M_HA)) {
            if (!num_i(t + 5, 0, 6, &l)) return fail(s, "prec is 0 to 6");
            f->prec = (int8_t)l;
        } else if (!strcmp(t, "within") && (allow & M_WITHIN)) {
            char *dd = i + 1 < s->ntok ? strstr(s->tok[i + 1], "..") : NULL;
            if (!dd) return fail(s, "within needs lo..hi");
            *dd = 0;
            if (!num_f(s->tok[i + 1], &f->lo) || !num_f(dd + 2, &f->hi) || f->hi < f->lo) return fail(s, "within needs lo..hi");
            f->flags |= PFF_WITHIN;
            i++;
        } else if ((allow & M_UNIT) && !unit && !keyword(t) && t[0] != '"' && !strchr(t, '=') && strlen(t) <= 15) {
            unit = true;
            if (!pool_add(s, t, &f->unit)) return false;
        } else {
            return fail(s, "unknown word '%s'", t);
        }
    }
    return true;
}

/* One field, or one for each number of a list (id "cell1..16", name "Cell #"), from a
 * template: addresses step by `regs` (a coil: by 1). */
static bool each(ps_t *s, const char *idt, const char *name, const pf_field_t *tpl, int regs, int modfrom, unsigned allow)
{
    char base[PF_ID_MAX + 1], id[PF_ID_MAX + 8], nm[PF_NAME_MAX + 8];
    int from = 0, to = 0;
    bool list = range(idt, base, sizeof base, &from, &to);
    const char *hash = strchr(name, '#');
    if (list && !hash) return fail(s, "a list's name needs # for its number");
    for (int k = from; k <= to; k++) {
        if (list) {
            snprintf(id, sizeof id, "%s%d", base, k);
            snprintf(nm, sizeof nm, "%.*s%d%s", (int)(hash - name), name, k, hash + 1);
            if (strlen(nm) > PF_NAME_MAX) return fail(s, "the name must be \"quoted\", 1 to %d characters", PF_NAME_MAX);
        } else {
            snprintf(id, sizeof id, "%s", idt);
            snprintf(nm, sizeof nm, "%s", name);
        }
        uint16_t idoff, nmoff;
        pf_field_t *f = field_new(s, id, nm);
        if (!f) return false;
        idoff = f->id;
        nmoff = f->name;
        *f = *tpl;
        f->id = idoff;
        f->name = nmoff;
        f->addr = (uint16_t)(tpl->addr + (k - from) * (regs ? regs : 1));
        if (f->block != 0xFF && !place(s, f, regs)) return false;
        if (!mods(s, f, modfrom, allow)) return false;
    }
    return true;
}

static void tpl_init(pf_field_t *t)
{
    memset(t, 0, sizeof *t);
    t->scale = 1;
    t->prec = -1;
    t->mask = 0xFFFFFFFFu;
    t->block = 0xFF;
}

/* id "name" block address: the start every read field shares. */
static bool where(ps_t *s, pf_field_t *t, const char **name)
{
    int64_t a;
    if (!(*name = name_of(s, 2))) return false;
    int b = profile_block(s->p, s->tok[3]);
    if (b < 0) return fail(s, "no block '%s'", s->tok[3]);
    if (!num_i(s->tok[4], 0, 0xFFFF, &a)) return fail(s, "bad address '%s'", s->tok[4]);
    t->block = (uint8_t)b;
    t->addr = (uint16_t)a;
    return true;
}

/* sensor id "name" block address type [...] */
static bool st_sensor(ps_t *s)
{
    pf_field_t t;
    const char *name;
    tpl_init(&t);
    if (s->ntok < 6) return fail(s, "sensor needs: id \"name\" block address type");
    if (!where(s, &t, &name)) return false;
    int ti = type_of(s->tok[5]);
    if (ti < 0) return fail(s, "unknown type '%s'", s->tok[5]);
    t.kind = PF_SENSOR;
    t.type = TYPES[ti].type;
    return each(s, s->tok[1], name, &t, TYPES[ti].regs, 6, M_SCALE | M_UNIT | M_HA | M_DIAG | M_NODATA);
}

/* binary id "name" block address bit N | type op value */
static bool st_binary(ps_t *s)
{
    pf_field_t t;
    const char *name;
    int64_t l;
    tpl_init(&t);
    if (s->ntok < 6) return fail(s, "binary needs: id \"name\" block address, then bit N, coil, or type op value");
    if (!where(s, &t, &name)) return false;
    t.kind = PF_BINARY;
    if (!strcmp(s->tok[5], "bit")) {
        if (s->ntok < 7 || !num_i(s->tok[6], 0, 15, &l)) return fail(s, "bit must be 0 to 15");
        t.type = T_BIT;
        t.bit = (uint8_t)l;
        return each(s, s->tok[1], name, &t, 1, 7, M_HA | M_DIAG);
    }
    if (!strcmp(s->tok[5], "coil")) {
        t.type = T_COIL;
        return each(s, s->tok[1], name, &t, 0, 6, M_HA | M_DIAG);
    }
    int ti = type_of(s->tok[5]);
    if (ti < 0 || !TYPES[ti].regs) return fail(s, "unknown type '%s'", s->tok[5]);
    if (s->ntok < 8) return fail(s, "binary needs: type op value");
    int op = op_of(s->tok[6]);
    if (op < 0) return fail(s, "unknown comparison '%s'", s->tok[6]);
    if (!num_f(s->tok[7], &t.k)) return fail(s, "bad value '%s'", s->tok[7]);
    t.type = TYPES[ti].type;
    t.op = (uint8_t)op;
    return each(s, s->tok[1], name, &t, TYPES[ti].regs, 8, M_SCALE | M_HA | M_DIAG);
}
```

The `coil3` line in the test is `binary coil3 "Coil 3" flags 3 coil`: six words, the `coil` branch above.

```c
/* text id "name" block address [type] enum [mask=M] v=name... | bits n=name... | ascii N | asciilo N */
static bool st_text(ps_t *s)
{
    profile_t *p = s->p;
    pf_field_t t;
    const char *name;
    int64_t l;
    int i = 5, regs = 1;
    tpl_init(&t);
    t.type = T_U16;
    if (s->ntok < 6) return fail(s, "text needs: id \"name\" block address, then enum, bits, ascii or asciilo");
    if (!where(s, &t, &name)) return false;
    int ti = type_of(s->tok[5]);
    if (ti >= 0 && TYPES[ti].regs) { t.type = TYPES[ti].type; regs = TYPES[ti].regs; i = 6; }
    const char *how = i < s->ntok ? s->tok[i++] : "";
    if (!strcmp(how, "ascii") || !strcmp(how, "asciilo")) {
        char base[PF_ID_MAX + 1];
        int a, b;
        if (i >= s->ntok || !num_i(s->tok[i], 1, 24, &l)) return fail(s, "ascii needs a length of 1 to 24 registers");
        if (range(s->tok[1], base, sizeof base, &a, &b)) return fail(s, "ascii cannot be a list");
        if (p->nstr == PF_STRS) return fail(s, "too many ascii texts (%d)", PF_STRS);
        t.kind = PF_ASCII;
        t.type = how[5] ? T_ASCIILO : T_ASCII;
        t.bit = (uint8_t)l;
        t.str = p->nstr++;
        return each(s, s->tok[1], name, &t, (int)l, i + 1, M_DIAG);
    }
    if (strcmp(how, "enum") && strcmp(how, "bits")) return fail(s, "text needs enum, bits, ascii or asciilo");
    t.kind = how[0] == 'e' ? PF_ENUM : PF_BITS;
    t.vname = p->nvname;
    for (; i < s->ntok; i++) {
        char *w = s->tok[i], *eq = strchr(w, '=');
        if (!strcmp(w, "diag")) { t.flags |= PFF_DIAG; continue; }
        if (!eq) return fail(s, "expected value=name, not '%s'", w);
        *eq = 0;
        if (!strcmp(w, "mask") && t.kind == PF_ENUM) {
            if (!num_i(eq + 1, 1, 0xFFFFFFFFLL, &l)) return fail(s, "bad mask '%s'", eq + 1);
            t.mask = (uint32_t)l;
            continue;
        }
        if (!num_i(w, 0, t.kind == PF_BITS ? 31 : 0xFFFFFFFFLL, &l)) return fail(s, "bad value '%s'", w);
        char *vn = unq(eq + 1);
        if (!vn || !*vn || strlen(vn) > PF_NAME_MAX) return fail(s, "the name must be \"quoted\", 1 to %d characters", PF_NAME_MAX);
        if (p->nvname == PF_VNAMES) return fail(s, "too many value names (%d)", PF_VNAMES);
        p->vname[p->nvname].value = (uint32_t)l;
        if (!pool_add(s, vn, &p->vname[p->nvname].name)) return false;
        p->nvname++;
    }
    t.nvname = (uint16_t)(p->nvname - t.vname);
    if (!t.nvname) return fail(s, "%s needs at least one value=name", how);
    return each(s, s->tok[1], name, &t, regs, s->ntok, 0);
}

static const struct { const char *name; uint8_t fn, args; } FNS[] = {    /* args: 0 a list, 1, 2 fields, 3 a field and a number */
    { "sum", F_SUM, 0 }, { "min", F_MIN, 0 }, { "max", F_MAX, 0 }, { "avg", F_AVG, 0 }, { "delta", F_DELTA, 0 },
    { "argmin", F_ARGMIN, 0 }, { "argmax", F_ARGMAX, 0 }, { "count", F_COUNT, 0 },
    { "sub", F_SUB, 2 }, { "mul", F_MUL, 2 }, { "pos", F_POS, 1 }, { "neg", F_NEG, 1 }, { "abs", F_ABS, 1 },
    { "eq", F_EQ, 3 }, { "ne", F_NE, 3 }, { "gt", F_GT, 3 }, { "lt", F_LT, 3 }, { "anybits", F_ANYBITS, 3 }, { "nobits", F_NOBITS, 3 },
};

/* A field, or each of a list, into arg[]. 0 added if the word is not a field. */
static int args_of(ps_t *s, const char *w)
{
    profile_t *p = s->p;
    char base[PF_ID_MAX + 1], id[PF_ID_MAX + 8];
    int from, to, n = 0;
    if (!range(w, base, sizeof base, &from, &to)) { from = to = 0; base[0] = 0; }
    for (int k = from; k <= to; k++) {
        if (base[0]) snprintf(id, sizeof id, "%s%d", base, k); else snprintf(id, sizeof id, "%s", w);
        int f = profile_field(p, id);
        if (f < 0) return n ? -1 : 0;
        if (p->field[f].kind == PF_ASCII) return -2;
        if (p->narg == PF_ARGS) return -3;
        p->arg[p->narg++] = (uint16_t)f;
        n++;
    }
    return n;
}

/* calc id "name" function arguments [...] */
static bool st_calc(ps_t *s)
{
    profile_t *p = s->p;
    pf_field_t t;
    const char *name;
    int fi = -1, i = 4;
    tpl_init(&t);
    if (s->ntok < 5) return fail(s, "calc needs: id \"name\" function arguments");
    if (!(name = name_of(s, 2))) return false;
    for (unsigned k = 0; k < sizeof FNS / sizeof FNS[0]; k++) if (!strcmp(FNS[k].name, s->tok[3])) fi = (int)k;
    if (fi < 0) return fail(s, "unknown function '%s'", s->tok[3]);
    t.fn = FNS[fi].fn;
    t.kind = t.fn >= F_EQ ? PF_CALCBIN : PF_CALC;
    t.arg = p->narg;
    int want = FNS[fi].args == 3 ? 1 : FNS[fi].args;
    while (i < s->ntok && (want == 0 || t.narg < want)) {
        int n = args_of(s, s->tok[i]);
        if (n == -2) return fail(s, "'%s' is text", s->tok[i]);
        if (n == -3) return fail(s, "too many calc arguments (%d)", PF_ARGS);
        if (n <= 0) {
            if (want || t.narg) break;
            return fail(s, "no field '%s'", s->tok[i]);
        }
        t.narg = (uint16_t)(t.narg + n);
        i++;
    }
    if (!t.narg && i < s->ntok) return fail(s, "no field '%s'", s->tok[i]);
    if (want && t.narg != want) return fail(s, "%s needs %d field%s", FNS[fi].name, want, want > 1 ? "s" : "");
    if (FNS[fi].args == 3) {
        int64_t l;
        if (i >= s->ntok) return fail(s, "%s needs a field and a number", FNS[fi].name);
        if (t.fn >= F_ANYBITS) {
            if (!num_i(s->tok[i], 1, 0xFFFFFFFFLL, &l)) return fail(s, "bad mask '%s'", s->tok[i]);
            t.mask = (uint32_t)l;
        } else if (!num_f(s->tok[i], &t.k)) {
            return fail(s, "bad value '%s'", s->tok[i]);
        }
        i++;
    }
    return each(s, s->tok[1], name, &t, 0, i, FNS[fi].args == 0 ? M_UNIT | M_HA | M_DIAG | M_WITHIN : M_UNIT | M_HA | M_DIAG);
}

/* reject block if zero [fields] | reject block if field op value */
static bool st_reject(ps_t *s)
{
    profile_t *p = s->p;
    if (s->ntok < 4 || strcmp(s->tok[2], "if"))
        return fail(s, "reject needs: block if zero [fields], or block if field op value");
    int b = profile_block(p, s->tok[1]);
    if (b < 0) return fail(s, "no block '%s'", s->tok[1]);
    if (p->nreject == PF_REJECTS) return fail(s, "too many reject rules (%d)", PF_REJECTS);
    pf_reject_t *j = &p->reject[p->nreject];
    memset(j, 0, sizeof *j);
    j->block = (uint8_t)b;
    int first = 3, last = s->ntok;
    if (!strcmp(s->tok[3], "zero")) {
        j->kind = s->ntok == 4 ? RJ_ZERO : RJ_ZEROS;
        first = 4;
        if (last - first > 4) return fail(s, "at most 4 fields");
    } else {
        if (s->ntok != 6) return fail(s, "reject needs: block if zero [fields], or block if field op value");
        int op = op_of(s->tok[4]);
        if (op < 0) return fail(s, "unknown comparison '%s'", s->tok[4]);
        if (!num_f(s->tok[5], &j->k)) return fail(s, "bad value '%s'", s->tok[5]);
        j->kind = RJ_CMP;
        j->op = (uint8_t)op;
        last = 4;
    }
    for (int i = first; i < last; i++) {
        int f = profile_field(p, s->tok[i]);
        if (f < 0) return fail(s, "no field '%s'", s->tok[i]);
        if (p->field[f].block != b) return fail(s, "'%s' is not in block %s", s->tok[i], s->tok[1]);
        if (p->field[f].kind == PF_ASCII || p->field[f].kind >= PF_CALC) return fail(s, "'%s' is not a number read from the block", s->tok[i]);
        j->field[j->nfield++] = (uint16_t)f;
    }
    p->nreject++;
    return true;
}

/* block id kind address count period [info] */
static bool st_block(ps_t *s)
{
    profile_t *p = s->p;
    static const struct { const char *name; uint8_t fn; } KINDS[] = { { "coil", 1 }, { "discrete", 2 }, { "hold", 3 }, { "input", 4 } };
    int64_t a, n, v;
    if (s->ntok < 6 || s->ntok > 7) return fail(s, "block needs: id kind address count period [info]");
    if (!id_ok(s->tok[1])) return fail(s, "bad id '%s': 1 to %d of a-z 0-9 _", s->tok[1], PF_ID_MAX);
    if (profile_block(p, s->tok[1]) >= 0) return fail(s, "'%s' is declared twice", s->tok[1]);
    if (p->nblock == PF_BLOCKS) return fail(s, "too many blocks (%d)", PF_BLOCKS);
    pf_block_t *b = &p->block[p->nblock];
    memset(b, 0, sizeof *b);
    for (unsigned k = 0; k < 4; k++) if (!strcmp(KINDS[k].name, s->tok[2])) b->fn = KINDS[k].fn;
    if (!b->fn) return fail(s, "unknown block kind '%s'", s->tok[2]);
    if (!num_i(s->tok[3], 0, 0xFFFF, &a)) return fail(s, "bad address '%s'", s->tok[3]);
    if (b->fn >= 3 && !num_i(s->tok[4], 1, 125, &n)) return fail(s, "a block of holding or input registers counts 1 to 125");
    if (b->fn <= 2 && !num_i(s->tok[4], 1, 2000, &n)) return fail(s, "a block of coils or discrete inputs counts 1 to 2000");
    if (a + n > 0x10000) return fail(s, "the block runs past 0xFFFF");
    char *t = s->tok[5], unit = t[0] ? t[strlen(t) - 1] : 0;
    uint32_t mul = unit == 's' ? 1000u : unit == 'm' ? 60000u : unit == 'h' ? 3600000u : 0;
    if (mul) t[strlen(t) - 1] = 0;
    if (!mul || !num_i(t, 1, 86400, &v) || v * mul > 86400000LL) { if (mul) t[strlen(t)] = (char)unit; return fail(s, "bad period '%s'", s->tok[5]); }
    if (s->ntok == 7 && strcmp(s->tok[6], "info")) return fail(s, "unknown word '%s'", s->tok[6]);
    if (!pool_add(s, s->tok[1], &b->id)) return false;
    b->addr = (uint16_t)a;
    b->count = (uint16_t)n;
    b->period_ms = (uint32_t)(v * mul);
    b->info = s->ntok == 7;
    p->nblock++;
    return true;
}

static bool ms_of(const char *t, int64_t lo, int64_t hi, uint16_t *out)
{
    char b[16];
    int64_t v;
    size_t n = strlen(t);
    if (n < 3 || n >= sizeof b || strcmp(t + n - 2, "ms")) return false;
    memcpy(b, t, n - 2);
    b[n - 2] = 0;
    if (!num_i(b, lo, hi, &v)) return false;
    *out = (uint16_t)v;
    return true;
}

/* profile, name, maker, line, stale, timeout, turnaround */
static bool st_head(ps_t *s, const char *w)
{
    profile_t *p = s->p;
    int64_t v;
    if (!strcmp(w, "profile")) {
        const char *id = s->tok[1];
        if (s->ntok != 3) return fail(s, "profile needs: id version");
        size_t n = strlen(id);
        bool ok = n && n <= PF_ID_MAX;
        for (const char *c = id; *c; c++) ok = ok && ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-');
        if (!ok) return fail(s, "bad profile id '%s': 1 to %d of a-z 0-9 -", id, PF_ID_MAX);
        if (!num_i(s->tok[2], 0, 1000, &v) || v != 1) return fail(s, "format version %s is not known; this firmware reads 1", s->tok[2]);
        return pool_add(s, id, &p->id);
    }
    if (s->ntok != 2 && strcmp(w, "line")) return fail(s, "%s needs one value", w);
    if (!strcmp(w, "name") || !strcmp(w, "maker")) {
        const char *n = name_of(s, 1);
        return n && pool_add(s, n, w[0] == 'n' ? &p->name : &p->maker);
    }
    if (!strcmp(w, "line")) {
        const char *f = s->ntok == 3 ? s->tok[2] : "";
        if (s->ntok != 3 || !num_i(s->tok[1], 300, 2000000, &v)) return fail(s, "line needs: baud frame, e.g. 9600 8N1");
        if (strlen(f) != 3 || (f[0] != '7' && f[0] != '8') || !strchr("NEO", f[1]) || (f[2] != '1' && f[2] != '2'))
            return fail(s, "bad line settings '%s'", f);
        p->baud = (uint32_t)v;
        memcpy(p->frame, f, 4);
        return true;
    }
    if (!strcmp(w, "stale")) {
        if (!num_i(s->tok[1], 1, 255, &v)) return fail(s, "stale must be 1 to 255");
        p->stale = (uint8_t)v;
        return true;
    }
    if (!strcmp(w, "timeout")) return ms_of(s->tok[1], 50, 10000, &p->timeout_ms) || fail(s, "timeout must be 50ms to 10000ms");
    return ms_of(s->tok[1], 0, 1000, &p->turn_ms) || fail(s, "turnaround must be 0ms to 1000ms");
}

bool profile_parse(profile_t *p, const char *text, size_t n, char *err, size_t errlen)
{
    static ps_t s;
    memset(p, 0, sizeof *p);
    p->npool = 1;
    p->stale = 5;
    p->timeout_ms = 1000;
    memset(&s, 0, sizeof s);
    s.p = p;
    s.err = err;
    s.errlen = errlen;
    if (errlen) err[0] = 0;
    if (n > PF_TEXT_MAX) return fail(&s, "the profile is longer than %u bytes", PF_TEXT_MAX);
    bool first = true;
    for (size_t at = 0; at < n;) {
        size_t e = at;
        while (e < n && text[e] != '\n') e++;
        s.line++;
        if (e - at > LINE_MAX) return fail(&s, "longer than %d characters", LINE_MAX);
        for (size_t k = at; k < e; k++)
            if ((unsigned char)text[k] < 0x20 && text[k] != '\t' && text[k] != '\r') return fail(&s, "a control character");
        memcpy(s.buf, text + at, e - at);
        s.buf[e - at] = 0;
        at = e + 1;
        if (!tokens(&s)) return false;
        if (!s.ntok) continue;
        const char *w = s.tok[0];
        bool ok;
        if (first && strcmp(w, "profile")) return fail(&s, "the first statement must be: profile id 1");
        if (!first && !strcmp(w, "profile")) return fail(&s, "profile is given twice");
        if (!strcmp(w, "profile") || !strcmp(w, "name") || !strcmp(w, "maker") || !strcmp(w, "line") ||
            !strcmp(w, "stale") || !strcmp(w, "timeout") || !strcmp(w, "turnaround")) ok = st_head(&s, w);
        else if (!strcmp(w, "block")) ok = st_block(&s);
        else if (!strcmp(w, "sensor")) ok = st_sensor(&s);
        else if (!strcmp(w, "binary")) ok = st_binary(&s);
        else if (!strcmp(w, "text")) ok = st_text(&s);
        else if (!strcmp(w, "calc")) ok = st_calc(&s);
        else if (!strcmp(w, "reject")) ok = st_reject(&s);
        else if (!strcmp(w, "number") || !strcmp(w, "select") || !strcmp(w, "switch") || !strcmp(w, "button"))
            ok = fail(&s, "controls come in phase 2");
        else ok = fail(&s, "unknown statement '%s'", w);
        if (!ok) return false;
        first = false;
    }
    s.line = 0;
    if (first) return fail(&s, "the profile is empty");
    if (!p->nblock) return fail(&s, "no blocks");
    return true;
}
```

The `block` period error path restores the trimmed character only for the message; keep it as written (the test checks `bad period '10x'`: `x` is not s/m/h, so `mul` is 0 and nothing was trimmed).

- [ ] **Step 5: Add the test to `tests/host.sh`** — after the `t modbus` line:

```sh
t profile -Isrc/app tests/test_profile.c src/app/profile.c
```

- [ ] **Step 6: Run the tests**

Run: `bash tests/host.sh 2>&1 | grep -v ': ok'`
Expected: no `FAILED` line; `profile: ok` among the `ok` lines (check with `bash tests/host.sh | grep profile`).

- [ ] **Step 7: The fuzz target `tests/fuzz/profile.c`**

```c
/* profile.c — Any text as a profile (src/app/profile.c). What parses must hold together:
 * every index inside its table, every string inside the pool; and decoding any reply of
 * the right size for each block (pfdecode.c, from Task 3) must not fail. */
#include "profile.h"
#include "pfdecode.h"
#include <assert.h>
#include <string.h>

static profile_t P;
static pf_state_t S;

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static char err[160];
    static uint8_t data[250], txt[200];
    if (!profile_parse(&P, (const char *)d, n, err, sizeof err)) { assert(err[0]); return 0; }
    assert(P.nblock >= 1 && P.nblock <= PF_BLOCKS && P.nfield <= PF_FIELDS && P.npool <= PF_POOL && !P.pool[0]);
    for (int i = 0; i < P.nfield; i++) {
        const pf_field_t *f = &P.field[i];
        assert(f->id < P.npool && f->name < P.npool && f->unit < P.npool && f->dc < P.npool && f->sc < P.npool);
        assert(f->kind >= PF_CALC ? f->block == 0xFF : f->block < P.nblock);
        assert((uint32_t)f->vname + f->nvname <= P.nvname && (uint32_t)f->arg + f->narg <= P.narg);
        for (int k = 0; k < f->narg; k++) assert(P.arg[f->arg + k] < i);
    }
    pf_state_init(&S);
    for (int b = 0; b < P.nblock; b++) {
        size_t len = P.block[b].fn <= 2 ? (P.block[b].count + 7u) / 8u : P.block[b].count * 2u;
        for (size_t k = 0; k < len; k++) data[k] = (uint8_t)(n + k * 37 + b);
        (void)pf_decode(&P, b, data, len, &S);
        (void)pf_decode(&P, b, data, len ? len - 1 : 1, &S);   /* the wrong size: refused */
        pf_drop(&P, b, &S);
        (void)pf_decode(&P, b, data, len, &S);
    }
    for (int i = 0; i < P.nfield; i++) { (void)pf_text(&P, i, &S, (char *)txt, sizeof txt); (void)pf_num((char *)txt, sizeof txt, S.val[i].v, pf_prec(&P.field[i])); }
    return 0;
}
```

In `tests/fuzz/run.sh`, after the `modbus)` line (this target needs Task 3's `pfdecode.c`; add it with Task 3):

```sh
  profile) build profile -Isrc/app tests/fuzz/profile.c src/app/profile.c src/app/pfdecode.c ;;
```

and make the default list `cfg http hclient modbus profile` in both `for t in` lines; in `.github/workflows/tests.yml` append `profile` to the fuzz step's targets. Seed: `tests/fuzz/seeds/profile/minimal` containing the test's `ok` profile text (copy it from Step 2, without the C quoting).

- [ ] **Step 8: Commit** (the fuzz target is committed with Task 3, which it needs)

```bash
git add src/app/profile.h src/app/profile.c tests/test_profile.c tests/host.sh
git commit -m "Device profiles: the parser

profile.c turns a profile's text into fixed tables: blocks, fields
(sensor, binary, text: enum, bits, ascii; calc), reject rules. Lists
(cell1..16), scale, offset, nodata, HA hints. Mistakes are refused with
their line; controls are refused until phase 2.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK"
```

### Task 3: Replies into values (`pfdecode.c`)

**Files:**
- Create: `src/app/pfdecode.h`, `src/app/pfdecode.c`, `tests/test_pfdecode.c`
- Modify: `tests/host.sh`; and commit Task 2's fuzz target (`tests/fuzz/profile.c`, `tests/fuzz/run.sh`, `tests/fuzz/seeds/profile/minimal`, `.github/workflows/tests.yml`)

**Interfaces:**
- Consumes: `profile.h` (Task 2).
- Produces: `pf_val_t {float v; uint32_t raw;}`, `pf_state_t {pf_val_t val[PF_FIELDS]; uint8_t ok[]; char str[PF_STRS][PF_STR_LEN];}`, `bool pf_ok(const pf_state_t *, int)`, `void pf_state_init(pf_state_t *)`, `bool pf_decode(const profile_t *p, int block, const uint8_t *d, size_t n, pf_state_t *s)`, `void pf_drop(const profile_t *p, int block, pf_state_t *s)`, `size_t pf_text(const profile_t *p, int i, const pf_state_t *s, char *out, size_t max)`, `int pf_prec(const pf_field_t *f)`, `size_t pf_num(char *out, size_t max, float v, int prec)`.

- [ ] **Step 1: Create `src/app/pfdecode.h`**

```c
/*
 * pfdecode.h — A device's values, from its blocks' replies, by its profile (profile.h).
 * Each field has a number (v; for a binary 0 or 1), its raw reading, and whether it is
 * known now; an ascii field's text is in str[field.str].
 */
#ifndef PFDECODE_H
#define PFDECODE_H
#include "profile.h"

typedef struct { float v; uint32_t raw; } pf_val_t;
typedef struct {
    pf_val_t val[PF_FIELDS];
    uint8_t  ok[(PF_FIELDS + 7) / 8];
    char     str[PF_STRS][PF_STR_LEN];
} pf_state_t;

static inline bool pf_ok(const pf_state_t *s, int i) { return s->ok[i >> 3] >> (i & 7) & 1u; }

void   pf_state_init(pf_state_t *s);       /* nothing known */
/* A block's reply: register bytes (2 a register, high byte first) or coil bits (as Modbus
 * packs them). False, nothing changed, if the size is not the block's or a reject rule
 * says no. Then every calc is worked out again. */
bool   pf_decode(const profile_t *p, int block, const uint8_t *d, size_t n, pf_state_t *s);
/* A block gone stale: its fields unknown, and the calcs again. */
void   pf_drop(const profile_t *p, int block, pf_state_t *s);
/* A text field: an enum's name ("Unknown (5)" if none), the set bits' names joined by
 * "; " ("None" if none), or the ascii. "" if not known. */
size_t pf_text(const profile_t *p, int i, const pf_state_t *s, char *out, size_t max);
int    pf_prec(const pf_field_t *f);       /* decimals to show */
/* v with `prec` decimals, as JSON has it; "null" if not a finite number that fits.
 * 0 if out is too small. */
size_t pf_num(char *out, size_t max, float v, int prec);

#endif
```

- [ ] **Step 2: Write the failing test `tests/test_pfdecode.c`**

```c
/* test_pfdecode.c — replies into values (src/app/pfdecode.c).
 *   cc -Isrc/app tests/test_pfdecode.c src/app/pfdecode.c src/app/profile.c -o /tmp/pd && /tmp/pd */
#include "pfdecode.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static profile_t P;
static pf_state_t S;
static const char *T =
    "profile t 1\n"
    "block a hold 0 12 10s\n"
    "block cells hold 100 4 10s\n"
    "block txt hold 200 6 1m info\n"
    "block c coil 0 10 10s\n"
    "sensor v \"V\" a 0 u16 *0.01 V\n"
    "sensor i \"I\" a 1 s16 *0.01 A\n"
    "sensor big \"Big\" a 2 u32\n"
    "sensor le \"Le\" a 4 s32le\n"
    "sensor th \"Th\" a 6 s8h\n"
    "sensor tl \"Tl\" a 6 s8l\n"
    "sensor off \"Off\" a 7 u16 +-500 *0.1\n"
    "sensor nd \"Nd\" a 8 u16 nodata 0xFFFF\n"
    "text st \"St\" a 9 enum mask=0x0F 0=Idle 1=Run 8=\"Run limited\"\n"
    "binary hot \"Hot\" a 9 bit 15\n"
    "text w \"W\" a 10 bits 0=\"A\" 3=\"B\" 15=\"C\"\n"
    "binary high \"High\" a 0 u16 > 5000\n"
    "sensor cell1..4 \"Cell #\" cells 100 u16 *0.001 V\n"
    "text model \"Model\" txt 200 ascii 4\n"
    "text lo \"Low\" txt 204 asciilo 2\n"
    "binary c3 \"C3\" c 3 coil\n"
    "calc p \"P\" mul v i W\n"
    "calc pp \"Pp\" pos p\n"
    "calc np \"Np\" neg p\n"
    "calc ab \"Ab\" abs i\n"
    "calc df \"Df\" sub v off\n"
    "calc cmin \"Cmin\" min cell1..4 within 0.001..4.999\n"
    "calc cmax \"Cmax\" max cell1..4 within 0.001..4.999\n"
    "calc cavg \"Cavg\" avg cell1..4 within 0.001..4.999\n"
    "calc cdel \"Cdel\" delta cell1..4 within 0.001..4.999\n"
    "calc clow \"Clow\" argmin cell1..4 within 0.001..4.999\n"
    "calc chigh \"Chigh\" argmax cell1..4 within 0.001..4.999\n"
    "calc ccnt \"Ccnt\" count cell1..4 within 0.001..4.999\n"
    "calc csum \"Csum\" sum cell1..4\n"
    "calc running \"Running\" eq st 1\n"
    "calc anyw \"Anyw\" anybits w 0x0008\n"
    "calc now \"Now\" nobits w 0x0009\n"
    "reject a if zero\n"
    "reject a if v == 0\n"
    "reject cells if zero cell1 cell2\n";

static int F(const char *id) { int i = profile_field(&P, id); assert(i >= 0); return i; }
static float V(const char *id) { int i = F(id); assert(pf_ok(&S, i)); return S.val[i].v; }
static bool near(float a, float b) { return fabsf(a - b) < 1e-4f * (fabsf(b) > 1 ? fabsf(b) : 1); }
static size_t regs(uint8_t *d, const uint16_t *r, int n) { for (int k = 0; k < n; k++) { d[2 * k] = (uint8_t)(r[k] >> 8); d[2 * k + 1] = (uint8_t)r[k]; } return (size_t)n * 2; }
static const char *text(const char *id) { static char b[100]; pf_text(&P, F(id), &S, b, sizeof b); return b; }
static const char *num(float v, int prec) { static char b[40]; pf_num(b, sizeof b, v, prec); return b; }

int main(void)
{
    static char err[160];
    uint8_t d[64];
    if (!profile_parse(&P, T, strlen(T), err, sizeof err)) { puts(err); assert(0); }
    pf_state_init(&S);
    assert(!pf_ok(&S, F("v")) && !pf_ok(&S, F("p")));

    const uint16_t a[12] = { 5312, (uint16_t)-1234, 0x0001, 0x0002, 0xFFFE, 0xFFFF, 0x19E7, 750, 0xFFFF, 0x8001, 0x0009, 0 };
    assert(pf_decode(&P, 0, d, regs(d, a, 12), &S));
    assert(near(V("v"), 53.12f) && near(V("i"), -12.34f) && V("big") == 65538.0f && V("le") == -2.0f);
    assert(V("th") == 25.0f && V("tl") == -25.0f && near(V("off"), 25.0f));
    assert(!pf_ok(&S, F("nd")));                                       /* 0xFFFF: no data */
    assert(V("hot") == 1.0f && V("high") == 1.0f && !strcmp(text("st"), "Run") && !strcmp(text("w"), "A; B"));
    assert(near(V("p"), 53.12f * -12.34f) && V("pp") == 0.0f && near(V("np"), 655.5008f) && near(V("ab"), 12.34f));
    assert(near(V("df"), 53.12f - 25.0f));
    assert(V("running") == 1.0f && V("anyw") == 1.0f && V("now") == 0.0f);
    assert(!pf_ok(&S, F("cmin")) && !pf_ok(&S, F("csum")));            /* cells not read yet */

    const uint16_t c[4] = { 3301, 3299, 0, 3310 };                    /* cell 3 missing: reads 0 */
    assert(pf_decode(&P, 1, d, regs(d, c, 4), &S));
    assert(near(V("cmin"), 3.299f) && near(V("cmax"), 3.310f) && near(V("cdel"), 0.011f));
    assert(near(V("cavg"), (3.301f + 3.299f + 3.310f) / 3) && V("clow") == 2.0f && V("chigh") == 4.0f);
    assert(V("ccnt") == 3.0f && near(V("csum"), 9.91f));

    const uint16_t t[6] = { 0x4142, 0x4300, 0x5859, 0x2020, 0x0048, 0x0069 };   /* "ABC" NUL "XY  "; "Hi" */
    assert(pf_decode(&P, 2, d, regs(d, t, 6), &S));
    assert(!strcmp(text("model"), "ABC") && !strcmp(text("lo"), "Hi"));
    const uint16_t t2[6] = { 0x4869, 0x2020, 0x2001, 0x2020, 0x0041, 0x0042 };  /* "Hi", spaces, a control byte */
    assert(pf_decode(&P, 2, d, regs(d, t2, 6), &S) && !strcmp(text("model"), "Hi") && !strcmp(text("lo"), "AB"));

    uint8_t coils[2] = { 0x08, 0x00 };
    assert(pf_decode(&P, 3, coils, 2, &S) && V("c3") == 1.0f);
    assert(!pf_decode(&P, 3, coils, 1, &S));                           /* the wrong size */

    /* Enum and bits text at the edges. */
    uint16_t a2[12];
    memcpy(a2, a, sizeof a2);
    a2[9] = 5; a2[10] = 0;
    assert(pf_decode(&P, 0, d, regs(d, a2, 12), &S));
    assert(!strcmp(text("st"), "Unknown (5)") && !strcmp(text("w"), "None") && V("hot") == 0.0f && V("anyw") == 0.0f && V("now") == 1.0f);

    /* Reject rules: nothing changes. */
    memset(d, 0, sizeof d);
    assert(!pf_decode(&P, 0, d, 24, &S) && near(V("v"), 53.12f));
    a2[0] = 0;
    assert(!pf_decode(&P, 0, d, regs(d, a2, 12), &S) && near(V("v"), 53.12f));
    const uint16_t c0[4] = { 0, 0, 3300, 3300 };
    assert(!pf_decode(&P, 1, d, regs(d, c0, 4), &S) && near(V("cmin"), 3.299f));
    assert(!pf_decode(&P, 0, d, 23, &S));

    /* A block gone stale: its fields and what depends on them unknown. */
    pf_drop(&P, 0, &S);
    assert(!pf_ok(&S, F("v")) && !pf_ok(&S, F("p")) && !pf_ok(&S, F("running")) && pf_ok(&S, F("cmin")));
    assert(!strcmp(text("st"), ""));

    /* Numbers. */
    assert(!strcmp(num(53.12f, 2), "53.12") && !strcmp(num(-12.5f, 0), "-13") && !strcmp(num(0, 0), "0"));
    assert(!strcmp(num(-0.004f, 2), "0.00") && !strcmp(num(3.3f, 3), "3.300") && !strcmp(num(1234567, 0), "1234567"));
    assert(!strcmp(num(NAN, 2), "null") && !strcmp(num(1e30f, 0), "null") && !strcmp(num(-25, 1), "-25.0"));
    assert(pf_prec(&P.field[F("v")]) == 2 && pf_prec(&P.field[F("big")]) == 0 && pf_prec(&P.field[F("off")]) == 1);
    assert(pf_prec(&P.field[F("cell1")]) == 3 && pf_prec(&P.field[F("p")]) == 3 && pf_prec(&P.field[F("ccnt")]) == 0);
    puts("pfdecode: ok");
    return 0;
}
```

- [ ] **Step 3: Run it to see it fail**

Run: `cc -g -Isrc/app tests/test_pfdecode.c src/app/pfdecode.c src/app/profile.c -o /tmp/pd`
Expected: `pfdecode.c` does not exist.

- [ ] **Step 4: Create `src/app/pfdecode.c`**

```c
/*
 * pfdecode.c — A block's reply into a device's values, by its profile: registers and
 * bits to numbers, value names and text, then every calc again. And numbers written out
 * by hand: the gateway's printf has no floating point (newlib nano).
 */
#include "pfdecode.h"
#include <stdio.h>
#include <string.h>

static uint16_t reg(const uint8_t *d, int i) { return (uint16_t)(d[2 * i] << 8 | d[2 * i + 1]); }

static void set_ok(pf_state_t *s, int i, bool ok)
{
    if (ok) s->ok[i >> 3] |= (uint8_t)(1u << (i & 7));
    else s->ok[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

void pf_state_init(pf_state_t *s) { memset(s, 0, sizeof *s); }

static uint32_t raw_of(const pf_block_t *b, const pf_field_t *f, const uint8_t *d)
{
    int i = f->addr - b->addr;
    switch (f->type) {
    case T_U32: case T_S32:     return (uint32_t)reg(d, i) << 16 | reg(d, i + 1);
    case T_U32LE: case T_S32LE: return (uint32_t)reg(d, i + 1) << 16 | reg(d, i);
    case T_U8H: case T_S8H:     return d[2 * i];
    case T_U8L: case T_S8L:     return d[2 * i + 1];
    case T_BIT:                 return (uint32_t)(reg(d, i) >> f->bit & 1u);
    case T_COIL:                return (uint32_t)(d[i / 8] >> (i % 8) & 1u);
    default:                    return reg(d, i);
    }
}

static float value_of(const pf_field_t *f, uint32_t raw)
{
    double v;
    switch (f->type) {
    case T_S16:               v = (int16_t)raw; break;
    case T_S32: case T_S32LE: v = (int32_t)raw; break;
    case T_S8H: case T_S8L:   v = (int8_t)raw; break;
    default:                  v = raw;
    }
    return (float)((v + f->offset) * f->scale);
}

static bool cmp(uint8_t op, float a, float k)
{
    switch (op) {
    case OP_EQ: return a == k;
    case OP_NE: return a != k;
    case OP_GT: return a > k;
    default:    return a < k;
    }
}

/* Printable characters up to a NUL, trailing spaces trimmed. */
static void ascii(const pf_block_t *b, const pf_field_t *f, const uint8_t *d, char *out)
{
    int i = f->addr - b->addr, n = 0;
    bool end = false;
    for (int r = 0; r < f->bit && !end; r++)
        for (int k = f->type == T_ASCIILO ? 1 : 0; k < 2 && !end; k++) {
            uint8_t c = d[2 * (i + r) + k];
            if (!c) end = true;
            else if (c >= 0x20 && c <= 0x7E && n < PF_STR_LEN - 1) out[n++] = (char)c;
        }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static void calc(const profile_t *p, pf_state_t *s)
{
    for (int i = 0; i < p->nfield; i++) {
        const pf_field_t *f = &p->field[i];
        if (f->kind < PF_CALC) continue;
        const uint16_t *a = &p->arg[f->arg];
        bool ok = true;
        float v = 0;
        for (int k = 0; k < f->narg; k++) ok = ok && pf_ok(s, a[k]);
        float x = s->val[a[0]].v;
        if (ok) switch (f->fn) {
        case F_SUB:     v = x - s->val[a[1]].v; break;
        case F_MUL:     v = x * s->val[a[1]].v; break;
        case F_POS:     v = x > 0 ? x : 0; break;
        case F_NEG:     v = x < 0 ? -x : 0; break;
        case F_ABS:     v = x < 0 ? -x : x; break;
        case F_EQ: case F_NE: case F_GT: case F_LT: v = cmp((uint8_t)(f->fn - F_EQ), x, f->k); break;
        case F_ANYBITS: v = (s->val[a[0]].raw & f->mask) != 0; break;
        case F_NOBITS:  v = (s->val[a[0]].raw & f->mask) == 0; break;
        default: {                                   /* over a list, inside `within` if given */
            int n = 0, at = 0;
            float sum = 0, lo = 0, hi = 0;
            for (int k = 0; k < f->narg; k++) {
                float y = s->val[a[k]].v;
                if ((f->flags & PFF_WITHIN) && (y < f->lo || y > f->hi)) continue;
                if (!n || y < lo) { lo = y; if (f->fn == F_ARGMIN) at = k + 1; }
                if (!n || y > hi) { hi = y; if (f->fn == F_ARGMAX) at = k + 1; }
                sum += y;
                n++;
            }
            if (f->fn == F_COUNT) v = (float)n;
            else if (!n) ok = false;
            else v = f->fn == F_SUM ? sum : f->fn == F_MIN ? lo : f->fn == F_MAX ? hi :
                     f->fn == F_AVG ? sum / (float)n : f->fn == F_DELTA ? hi - lo : (float)at;
        }
        }
        s->val[i].v = v;
        s->val[i].raw = 0;
        set_ok(s, i, ok);
    }
}

bool pf_decode(const profile_t *p, int bi, const uint8_t *d, size_t n, pf_state_t *s)
{
    const pf_block_t *b = &p->block[bi];
    if (n != (b->fn <= 2 ? (b->count + 7u) / 8u : b->count * 2u)) return false;
    for (int r = 0; r < p->nreject; r++) {
        const pf_reject_t *j = &p->reject[r];
        bool hit = true;
        if (j->block != bi) continue;
        if (j->kind == RJ_ZERO) {
            for (size_t k = 0; k < n && hit; k++) hit = !d[k];
        } else if (j->kind == RJ_ZEROS) {
            for (int k = 0; k < j->nfield && hit; k++) hit = !raw_of(b, &p->field[j->field[k]], d);
        } else {
            const pf_field_t *f = &p->field[j->field[0]];
            hit = cmp(j->op, value_of(f, raw_of(b, f, d)), j->k);
        }
        if (hit) return false;
    }
    for (int i = 0; i < p->nfield; i++) {
        const pf_field_t *f = &p->field[i];
        if (f->block != bi) continue;
        if (f->kind == PF_ASCII) { ascii(b, f, d, s->str[f->str]); set_ok(s, i, true); continue; }
        uint32_t raw = raw_of(b, f, d);
        bool ok = true;
        for (int k = 0; k < f->nnodata; k++) ok = ok && raw != f->nodata[k];
        if (f->kind == PF_ENUM) raw &= f->mask;
        s->val[i].raw = raw;
        if (f->kind == PF_SENSOR) s->val[i].v = value_of(f, raw);
        else if (f->kind == PF_BINARY && f->type != T_BIT && f->type != T_COIL) s->val[i].v = cmp(f->op, value_of(f, raw), f->k);
        else s->val[i].v = (float)raw;
        set_ok(s, i, ok);
    }
    calc(p, s);
    return true;
}

void pf_drop(const profile_t *p, int bi, pf_state_t *s)
{
    for (int i = 0; i < p->nfield; i++) if (p->field[i].block == bi) set_ok(s, i, false);
    calc(p, s);
}

size_t pf_text(const profile_t *p, int i, const pf_state_t *s, char *out, size_t max)
{
    const pf_field_t *f = &p->field[i];
    size_t n = 0;
    int w;
    if (!max) return 0;
    out[0] = 0;
    if (!pf_ok(s, i)) return 0;
    if (f->kind == PF_ASCII) w = snprintf(out, max, "%s", s->str[f->str]);
    else {
        uint32_t raw = s->val[i].raw;
        for (int k = 0; k < f->nvname; k++) {
            const pf_vname_t *v = &p->vname[f->vname + k];
            if (f->kind == PF_ENUM ? v->value != raw : !(raw >> v->value & 1u)) continue;
            w = snprintf(out + n, max - n, "%s%s", n ? "; " : "", pf_str(p, v->name));
            if (w < 0 || (size_t)w >= max - n) break;
            n += (size_t)w;
            if (f->kind == PF_ENUM) break;
        }
        if (n) return n;
        w = f->kind == PF_ENUM ? snprintf(out, max, "Unknown (%lu)", (unsigned long)raw) : snprintf(out, max, "None");
    }
    return w < 0 ? 0 : (size_t)w < max ? (size_t)w : max - 1;
}

int pf_prec(const pf_field_t *f)
{
    if (f->prec >= 0) return f->prec;
    if (f->kind == PF_CALC) return f->fn == F_COUNT || f->fn == F_ARGMIN || f->fn == F_ARGMAX ? 0 : 3;
    if (f->kind != PF_SENSOR) return 0;
    float sc = f->scale < 0 ? -f->scale : f->scale;
    for (int k = 0; k <= 4; k++, sc *= 10) if (sc >= 0.999f) return k;
    return 4;
}

size_t pf_num(char *out, size_t max, float v, int prec)
{
    static const uint32_t P10[7] = { 1, 10, 100, 1000, 10000, 100000, 1000000 };
    char d[24];
    int k = (int)sizeof d, n;
    if (prec < 0) prec = 0;
    if (prec > 6) prec = 6;
    double a = v < 0 ? -(double)v : (double)v;
    if (!(a == a) || a * P10[prec] >= 1.8e19) { n = snprintf(out, max, "null"); return n < 0 || (size_t)n >= max ? 0 : (size_t)n; }
    uint64_t u = (uint64_t)(a * P10[prec] + 0.5), whole = u / P10[prec];
    uint32_t frac = (uint32_t)(u % P10[prec]);
    d[--k] = 0;
    do { d[--k] = (char)('0' + whole % 10u); whole /= 10u; } while (whole);
    n = snprintf(out, max, "%s%s", v < 0 && u ? "-" : "", d + k);
    if (prec && n >= 0 && (size_t)n < max) {
        int m = snprintf(out + n, max - (size_t)n, ".%0*lu", prec, (unsigned long)frac);
        n = m < 0 ? -1 : n + m;
    }
    return n < 0 || (size_t)n >= max ? 0 : (size_t)n;      /* 0: it did not fit */
}
```

- [ ] **Step 5: Add to `tests/host.sh`** (after `t profile`):

```sh
t pfdecode -Isrc/app tests/test_pfdecode.c src/app/pfdecode.c src/app/profile.c -lm
```

(`t()` hands everything after the name to the compiler, so `-lm` at the end links the maths library; on macOS it is harmless.)

- [ ] **Step 6: Run host tests and the fuzzer**

Run: `bash tests/host.sh | grep -E 'pfdecode|profile|FAILED'` then `CC=clang FUZZ_OUT=/tmp/fz tests/fuzz/run.sh 60 profile`
Expected: `pfdecode: ok`, `profile: ok`; fuzzer `0 failures`.

- [ ] **Step 7: Commit**

```bash
git add src/app/pfdecode.h src/app/pfdecode.c tests/test_pfdecode.c tests/host.sh tests/fuzz/profile.c tests/fuzz/run.sh tests/fuzz/seeds/profile .github/workflows/tests.yml
git commit -m "Device profiles: replies into values

pfdecode.c reads a block's reply into a device's values by its profile:
types, scale, offset, nodata, enum and bits names, ascii; reject rules
keep the last values; calc (lists within a range, arithmetic,
comparisons, bit tests) after every block; numbers written by hand.
Fuzzed with the parser: any text, then replies to each block.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK"
```

### Task 4: The EG4 profile, built in

**Files:**
- Create: `profiles/eg4-bms.txt`, `profiles/README.md`, `tools/embed_profiles.py`, `src/app/profiles.h`, `src/app/profiles.c`, `tests/test_builtin.c`
- Modify: `CMakeLists.txt` (generate `gen/profiles_gen.h`), `tests/host.sh`

**Interfaces:**
- Consumes: `profile_parse`, `pf_decode`, `pf_text` (Tasks 2-3).
- Produces: `const char *profiles_text(const char *id, size_t *len)` (NULL if no such built-in), `const char *profiles_id(int i)` (NULL past the last).

The field ids and names follow the ESPHome component's `esp32-example.yaml` (`total_voltage`, "Total voltage", ...) so that `devN.ids=esphome` with prefix `eg4_bms` gives the ESP32's entity IDs (`sensor.eg4_bms_total_voltage`). Registers and encodings follow `components/eg4_bms/eg4_bms.cpp` (github.com/RAR/esphome-eg4-bms).

- [ ] **Step 1: Write `profiles/eg4-bms.txt`**

```
# EG4 LifePower4 / LL battery BMS, over RS485 (Modbus RTU, function 3).
# From github.com/RAR/esphome-eg4-bms: registers as eg4_bms.cpp reads them, entity names
# as its esp32-example.yaml has them (so devN.ids=esphome, prefix eg4_bms, matches).
# The master pack answers at 0x10, the others at 0x01-0x0F (DIP switches).
profile eg4-bms 1
name "LifePower4 / LL"
maker "EG4"
line 9600 8N1
stale 5
timeout 1000ms
turnaround 50ms                     # packs miss a request that starts within a few ms of another's reply

block live1    hold 0x0000 18 10s
block live2    hold 0x0012 21 10s
block model    hold 0x0069 11 15m info
block firmware hold 0x0075 3  15m info
block serial   hold 0x0078 8  15m info

sensor total_voltage      "Total voltage"      live1 0x0000 u16 *0.01 V dc=voltage sc=measurement
sensor current            "Current"            live1 0x0001 s16 *0.01 A dc=current sc=measurement
sensor cell_voltage_1..16 "Cell voltage #"     live1 0x0002 u16 *0.001 V dc=voltage sc=measurement

sensor pcb_temperature    "Pcb temperature"    live2 0x0012 s16 °C dc=temperature sc=measurement
sensor avg_temperature    "Avg temperature"    live2 0x0013 s16 °C dc=temperature sc=measurement
sensor max_temperature    "Max temperature"    live2 0x0014 s16 °C dc=temperature sc=measurement
sensor remaining_capacity "Remaining capacity" live2 0x0015 u16 Ah sc=measurement
sensor max_charge_current "Max charge current" live2 0x0016 u16 A dc=current sc=measurement
sensor state_of_health    "State of health"    live2 0x0017 u16 % sc=measurement
sensor state_of_charge    "State of charge"    live2 0x0018 u16 % dc=battery sc=measurement
text   status             "Status"             live2 0x0019 enum mask=0x000F 0=Standby 1=Charging 2=Discharging 4=Protect 8="Charging limited"
binary heating            "Heating"            live2 0x0019 bit 15 dc=heat
text   warnings           "Warnings"           live2 0x001A bits 0="Pack OV" 1="Cell OV" 2="Pack UV" 3="Cell UV" 4="Charge OC" 5="Discharge OC" 6="Abnormal temp" 7="MOS overheat" 8="Charge OT" 9="Discharge OT" 10="Charge UT" 11="Discharge UT" 12="Low capacity" 13="Float stopped"
text   protection         "Protection"         live2 0x001B bits 0="Pack OV" 1="Cell OV" 2="Pack UV" 3="Cell UV" 4="Charge OC" 5="Discharge OC" 6="Abnormal temp" 7="MOS overheat" 8="Charge OT" 9="Discharge OT" 10="Charge UT" 11="Discharge UT" 12="Low capacity" 13="Discharge SC"
text   error              "Error"              live2 0x001C bits 0="Voltage error" 1="Temperature error" 2="Current flow error" 4="Cell unbalance"
sensor cycle_count        "Cycle count"        live2 0x001D u32 sc=total_increasing
sensor temperature_1      "Temperature 1"      live2 0x0021 s8h °C dc=temperature sc=measurement
sensor temperature_2      "Temperature 2"      live2 0x0021 s8l °C dc=temperature sc=measurement
sensor temperature_3      "Temperature 3"      live2 0x0022 s8h °C dc=temperature sc=measurement
sensor temperature_4      "Temperature 4"      live2 0x0022 s8l °C dc=temperature sc=measurement
sensor temperature_5      "Temperature 5"      live2 0x0023 s8h °C dc=temperature sc=measurement
sensor temperature_6      "Temperature 6"      live2 0x0023 s8l °C dc=temperature sc=measurement
sensor full_capacity      "Full capacity"      live2 0x0025 u16 *0.1 Ah sc=measurement

text   model              "Model"              model    0x0069 ascii 11 diag
text   firmware_version   "Firmware version"   firmware 0x0075 ascii 3 diag
text   serial_number      "Serial number"      serial   0x0078 ascii 8 diag

calc power                "Power"              mul total_voltage current W dc=power sc=measurement prec=1
calc charging_power       "Charging power"     pos power W dc=power sc=measurement prec=1
calc discharging_power    "Discharging power"  neg power W dc=power sc=measurement prec=1
calc min_cell_voltage     "Min cell voltage"     min cell_voltage_1..16 within 0.001..4.999 V dc=voltage sc=measurement
calc max_cell_voltage     "Max cell voltage"     max cell_voltage_1..16 within 0.001..4.999 V dc=voltage sc=measurement
calc delta_cell_voltage   "Delta cell voltage"   delta cell_voltage_1..16 within 0.001..4.999 V dc=voltage sc=measurement
calc cell_average_voltage "Average cell voltage" avg cell_voltage_1..16 within 0.001..4.999 V dc=voltage sc=measurement
calc min_voltage_cell     "Min voltage cell"     argmin cell_voltage_1..16 within 0.001..4.999
calc max_voltage_cell     "Max voltage cell"     argmax cell_voltage_1..16 within 0.001..4.999
calc cell_count           "Cell count"           count cell_voltage_1..16 within 0.001..4.999 diag
# Charging is blocked by pack and cell OV, charge OC, OT, UT; discharging by pack and
# cell UV, discharge OC, OT, UT and short circuit (eg4_bms.cpp).
calc charging             "Charging"             nobits protection 0x0513
calc discharging          "Discharging"          nobits protection 0x2A2C

reject live1 if zero
reject live1 if total_voltage == 0
reject live2 if zero
reject live2 if zero remaining_capacity state_of_health full_capacity
```

- [ ] **Step 2: Write `tools/embed_profiles.py`**

```python
#!/usr/bin/env python3
"""The built-in device profiles (profiles/*.txt) into a C header: each one's id and text."""
import re, sys
out, files = sys.argv[1], sorted(sys.argv[2:])
rows = []
for f in files:
    data = open(f, "rb").read()
    m = re.search(rb"^[ \t]*profile[ \t]+([a-z0-9-]+)[ \t]", data, re.M)
    if not m:
        sys.exit(f"{f}: no 'profile id' line")
    rows.append((m.group(1).decode(), data))
with open(out, "w") as o:
    o.write("/* generated from profiles/*.txt by tools/embed_profiles.py */\n")
    for k, (pid, data) in enumerate(rows):
        o.write(f"static const char PROFILE_{k}[] = {{" + ",".join(str(b) for b in data) + ",0};\n")
    o.write("static const struct { const char *id, *text; unsigned len; } BUILTIN[] = {\n")
    for k, (pid, data) in enumerate(rows):
        o.write(f'    {{ "{pid}", PROFILE_{k}, {len(data)}u }},\n')
    o.write("};\n")
print(f"profiles: {len(rows)} built in")
```

- [ ] **Step 3: Write `src/app/profiles.h` and `src/app/profiles.c`**

```c
/* profiles.h — The built-in device profiles: profiles/*.txt, embedded at build time. */
#ifndef PROFILES_H
#define PROFILES_H
#include <stddef.h>

const char *profiles_text(const char *id, size_t *len);   /* NULL: none by that id */
const char *profiles_id(int i);                           /* the i-th one's id; NULL past the last */

#endif
```

```c
/* profiles.c — The built-in device profiles (profiles.h). */
#include "profiles.h"
#include "profiles_gen.h"
#include <string.h>

#define N (int)(sizeof BUILTIN / sizeof BUILTIN[0])

const char *profiles_text(const char *id, size_t *len)
{
    for (int i = 0; i < N; i++)
        if (!strcmp(BUILTIN[i].id, id)) { *len = BUILTIN[i].len; return BUILTIN[i].text; }
    return NULL;
}

const char *profiles_id(int i) { return i >= 0 && i < N ? BUILTIN[i].id : NULL; }
```

- [ ] **Step 4: Generate the header in the build** — in `CMakeLists.txt`, before `n540_image(app ...`:

```cmake
file(GLOB PROFILE_TXT CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/profiles/*.txt")
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gen/profiles_gen.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed_profiles.py
            ${CMAKE_BINARY_DIR}/gen/profiles_gen.h ${PROFILE_TXT}
    DEPENDS ${PROFILE_TXT} ${CMAKE_SOURCE_DIR}/tools/embed_profiles.py)
```

and add `${CMAKE_BINARY_DIR}/gen/profiles_gen.h` beside `${CMAKE_BINARY_DIR}/gen/web_page.h` in `n540_image(app ...)`.

- [ ] **Step 5: Write the failing test `tests/test_builtin.c`** (EG4 frames built from the component's decoding, and the REGISTER_MAP.md example strings)

```c
/* test_builtin.c — every built-in profile parses; the EG4 one decodes as the ESPHome
 * component does (eg4_bms.cpp), with the identification strings of its REGISTER_MAP.md.
 *   python3 tools/embed_profiles.py /tmp/profiles_gen.h profiles/*.txt &&
 *   cc -I/tmp -Isrc/app tests/test_builtin.c src/app/profiles.c src/app/profile.c src/app/pfdecode.c -lm -o /tmp/bi && /tmp/bi */
#include "profiles.h"
#include "pfdecode.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static profile_t P;
static pf_state_t S;

static int F(const char *id) { int i = profile_field(&P, id); if (i < 0) printf("no field %s\n", id); assert(i >= 0); return i; }
static float V(const char *id) { int i = F(id); assert(pf_ok(&S, i)); return S.val[i].v; }
static bool near(float a, float b) { return fabsf(a - b) < 1e-4f * (fabsf(b) > 1 ? fabsf(b) : 1); }
static size_t regs(uint8_t *d, const uint16_t *r, int n) { for (int k = 0; k < n; k++) { d[2 * k] = (uint8_t)(r[k] >> 8); d[2 * k + 1] = (uint8_t)r[k]; } return (size_t)n * 2; }
static const char *text(const char *id) { static char b[160]; pf_text(&P, F(id), &S, b, sizeof b); return b; }
static size_t str(uint8_t *d, const char *s, int nregs) { memset(d, 0, (size_t)nregs * 2); memcpy(d, s, strlen(s)); return (size_t)nregs * 2; }

int main(void)
{
    static char err[160];
    static uint8_t d[256];
    size_t n;
    for (int i = 0; profiles_id(i); i++) {
        const char *t = profiles_text(profiles_id(i), &n);
        if (!profile_parse(&P, t, n, err, sizeof err)) { printf("%s: %s\n", profiles_id(i), err); assert(0); }
        printf("  %s: %d blocks, %d fields\n", profiles_id(i), P.nblock, P.nfield);
    }
    assert(!profiles_text("nope", &n) && !profiles_id(-1));

    const char *t = profiles_text("eg4-bms", &n);
    assert(t && profile_parse(&P, t, n, err, sizeof err));
    assert(P.turn_ms == 50 && P.timeout_ms == 1000 && P.baud == 9600 && !strcmp(pf_str(&P, P.maker), "EG4"));
    pf_state_init(&S);
    /* live1: 53.12 V, -12.34 A, cells 3.301 .. 3.316 with cell 16 missing (0) */
    uint16_t l1[18] = { 5312, (uint16_t)-1234 };
    for (int k = 0; k < 15; k++) l1[2 + k] = (uint16_t)(3301 + k);
    l1[17] = 0;
    assert(pf_decode(&P, profile_block(&P, "live1"), d, regs(d, l1, 18), &S));
    assert(near(V("total_voltage"), 53.12f) && near(V("current"), -12.34f) && near(V("cell_voltage_3"), 3.303f));
    assert(near(V("power"), -655.5008f) && V("charging_power") == 0 && near(V("discharging_power"), 655.5008f));
    assert(near(V("min_cell_voltage"), 3.301f) && near(V("max_cell_voltage"), 3.315f) && near(V("delta_cell_voltage"), 0.014f));
    assert(near(V("cell_average_voltage"), 3.308f) && V("min_voltage_cell") == 1 && V("max_voltage_cell") == 15 && V("cell_count") == 15);
    /* live2 */
    /* 0x12.. : temps, capacity, current, SOH, SOC, status 0x8001 (charging, heating), warnings 0x0003,
     * protection 0x1410 (charge OC, charge UT, low capacity), error 0x0001, cycles 0x0001_0002,
     * 0x1F-0x20 unused, temps 1-6 packed, 0x24, full capacity 2000, 0x26 */
    uint16_t l2[21] = { 31, 25, 33, 150, 100, 98, 76, 0x8001, 0x0003, 0x1410, 0x0001, 0x0001, 0x0002, 0, 0, 0x19E7, 0x1A1B, 0x1C1D, 0, 2000, 0 };
    assert(pf_decode(&P, profile_block(&P, "live2"), d, regs(d, l2, 21), &S));
    assert(V("pcb_temperature") == 31 && V("avg_temperature") == 25 && V("max_temperature") == 33 && V("remaining_capacity") == 150);
    assert(V("max_charge_current") == 100 && V("state_of_health") == 98 && V("state_of_charge") == 76 && V("cycle_count") == 65538);
    assert(!strcmp(text("status"), "Charging") && V("heating") == 1);
    assert(!strcmp(text("warnings"), "Pack OV; Cell OV") && !strcmp(text("protection"), "Charge OC; Charge UT; Low capacity"));
    assert(!strcmp(text("error"), "Voltage error"));
    assert(V("charging") == 0 && V("discharging") == 1);              /* 0x1410 & 0x0513: blocked from charging only */
    assert(V("temperature_1") == 25 && V("temperature_2") == -25 && V("temperature_3") == 26 && V("temperature_6") == 29);
    assert(near(V("full_capacity"), 200.0f));
    /* the second block's zero rule: remaining, SOH and full capacity all 0 */
    l2[3] = 0; l2[5] = 0; l2[19] = 0;
    assert(!pf_decode(&P, profile_block(&P, "live2"), d, regs(d, l2, 21), &S) && V("remaining_capacity") == 150);
    /* identification, as REGISTER_MAP.md shows it */
    assert(pf_decode(&P, profile_block(&P, "model"), d, str(d, "ZTR-16S200A-10KW-STD2", 11), &S));
    assert(pf_decode(&P, profile_block(&P, "firmware"), d, str(d, "S02T11", 3), &S));
    assert(pf_decode(&P, profile_block(&P, "serial"), d, str(d, "2309876543210987", 8), &S));
    assert(!strcmp(text("model"), "ZTR-16S200A-10KW-STD2") && !strcmp(text("firmware_version"), "S02T11") &&
           !strcmp(text("serial_number"), "2309876543210987"));
    puts("builtin: ok");
    return 0;
}
```

- [ ] **Step 6: Run it** — add to `tests/host.sh`, before the first `t` line:

```sh
python3 tools/embed_profiles.py "$O/profiles_gen.h" profiles/*.txt > /dev/null || exit 1
```

and after `t pfdecode`:

```sh
t builtin -I"$O" -Isrc/app tests/test_builtin.c src/app/profiles.c src/app/profile.c src/app/pfdecode.c -lm
```

(`$O` is `host.sh`'s build directory.) Run: `bash tests/host.sh | grep -E 'builtin|eg4|FAILED'`
Expected: `eg4-bms: 5 blocks, 53 fields` (2 + 16 cells + 7 sensors + status + heating + 3 bit texts + cycles + 6 temperatures + full capacity + 3 strings + 12 calcs) and `builtin: ok`.

- [ ] **Step 7: Build the firmware** — `cmake -B build && cmake --build build 2>&1 | grep -E '\berror\b|profiles:'`
Expected: `profiles: 1 built in`, no errors.

- [ ] **Step 8: Write `profiles/README.md`** — the format reference: copy the spec's "Profile format" section (statements table, types, value, shown, lists, functions, limits), add the two phase-1 notes (controls refused until phase 2; `reject B if zero f1 f2 ...`), and "How to add a built-in profile: a `profiles/<id>.txt` whose first statement is `profile <id> 1`; it is embedded at the next build, and `tests/test_builtin.c` checks it parses."

- [ ] **Step 9: Commit**

```bash
git add profiles tools/embed_profiles.py src/app/profiles.h src/app/profiles.c tests/test_builtin.c tests/host.sh CMakeLists.txt
git commit -m "Device profiles: EG4 BMS, built in

profiles/eg4-bms.txt from github.com/RAR/esphome-eg4-bms: registers as
eg4_bms.cpp reads them, names as its example YAML has them. Built-in
profiles are embedded at build time (tools/embed_profiles.py).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK"
```

### Task 5: Device settings (`devN.*`) and their record

**Files:**
- Modify: `src/app/cfg.h`, `src/app/cfg.c`, `src/app/cfg_store.c`
- Test: `tests/test_cfg.c`, `tests/test_store.c`

**Interfaces:**
- Produces: `#define DEVS 16`; `dev_cfg_t { char profile[25], name[25], label[33], prefix[25]; uint8_t port, address, controls, ids; }` (112 bytes; `ids` 0 own, 1 esphome); `cfg_t.dev[DEVS]` (after `more`); keys `devN.profile|name|label|prefix|port|address|controls|ids`; `size_t cfg_dev_lines(const cfg_t *c, int i, char *out, size_t max)`.

- [ ] **Step 1: Write the failing tests** — in `tests/test_cfg.c`, just before the final worst-case JSON section:

```c
    /* Devices: devN.* (N = 1..16), on a Modbus gateway port. */
    OK("port2.enabled=1\nport2.mode=modbus\nport2.local_port=502");
    OK("dev1.profile=eg4-bms\ndev1.name=eg4m\ndev1.port=2\ndev1.address=16");
    assert(!strcmp(c.dev[0].profile, "eg4-bms") && !strcmp(c.dev[0].name, "eg4m") && c.dev[0].port == 2 && c.dev[0].address == 16);
    BAD("dev2.profile=eg4-bms");                                                   /* no name */
    BAD("dev2.profile=eg4-bms\ndev2.name=eg4m\ndev2.port=2\ndev2.address=1");      /* the name is taken */
    BAD("dev2.profile=eg4-bms\ndev2.name=p1\ndev2.port=2\ndev2.address=16");       /* so is the address */
    BAD("dev2.profile=eg4-bms\ndev2.name=p1\ndev2.port=1\ndev2.address=1");        /* port 1 is no Modbus gateway */
    BAD("dev2.profile=EG4"); BAD("dev2.name=Big Name"); BAD("dev2.address=248"); BAD("dev2.port=5");
    BAD("dev17.name=x"); BAD("dev0.name=x"); BAD("dev2.ids=maybe"); BAD("dev2.x=1");
    BAD("dev1.ids=esphome");                                                       /* needs the ESP32's prefix */
    OK("dev1.ids=esphome\ndev1.prefix=eg4_bms");
    OK("dev16.profile=eg4-bms\ndev16.name=p1\ndev16.port=2\ndev16.address=1\ndev16.label=Pack 1");
    BAD("port2.mode=server");                                                      /* devices use it */
    {
        char l[600];
        cfg_t t2 = c;
        assert(cfg_dev_lines(&c, 0, l, sizeof l) && strstr(l, "dev1.profile=eg4-bms\n") && strstr(l, "dev1.ids=esphome\n") &&
               strstr(l, "dev1.prefix=eg4_bms\n") && strstr(l, "dev1.address=16\n"));
        assert(cfg_dev_lines(&c, 5, l, sizeof l) == strlen("dev6.profile=\n") && !strcmp(l, "dev6.profile=\n"));
        memset(t2.dev, 0, sizeof t2.dev);
        cfg_dev_lines(&c, 15, l, sizeof l);
        assert(cfg_parse(&t2, l, err, sizeof err) && !memcmp(&t2.dev[15], &c.dev[15], sizeof c.dev[15]));
        for (size_t k = 0; k < 40; k++) assert(!cfg_dev_lines(&c, 0, l, k));      /* never cut short */
    }
    OK("dev16.profile=");
    assert(!c.dev[15].name[0] && !c.dev[15].port && !c.dev[15].label[0]);
    OK("dev1.profile=\nport2.mode=server\nport2.local_port=4002\nport2.enabled=0");
```

In `tests/test_store.c`, before its final `puts`/`return`:

```c
    /* Devices: their own record, 0x0EE000 / 0x0EF000; a damaged one means none. */
    set(&c, "port2.enabled=1\nport2.mode=modbus\nport2.local_port=502\ndev3.profile=eg4-bms\ndev3.name=eg4m\ndev3.port=2\ndev3.address=16");
    assert(cfg_save(&c) && cfg_load(&d) && !memcmp(&c, &d, sizeof c) && !strcmp(d.dev[2].name, "eg4m"));
    {
        int at = ((const uint32_t *)(F[1] + 0xEE000))[0] == 0x56454444u ? 0xEE000 : 0xEF000;
        const uint32_t *h = (const uint32_t *)(F[1] + at);
        assert(h[0] == 0x56454444u && h[1] == 1 && h[3] == DEVS * sizeof(dev_cfg_t));
        F[1][at + 100] ^= 1;
    }
    assert(cfg_load(&d) && !d.dev[2].profile[0] && d.port[1].mode == MODE_MODBUS);   /* the rest as it was */
```

- [ ] **Step 2: Run them to see them fail**

Run: `bash tests/host.sh 2>&1 | grep -E 'cfg|store'`
Expected: build failures: `dev` is not a member of `cfg_t`, `cfg_dev_lines` undeclared.

- [ ] **Step 3: Implement** — `src/app/cfg.h`, after `more_cfg_t`:

```c
/* A device polled on a port, by a profile (device.c). Kept in a record of its own
 * (cfg_store.c), so older firmware never reads it. */
#define DEVS 16
typedef struct {
    char    profile[25];           /* a profile id; empty: no device here */
    char    name[25];              /* in MQTT topics and HA IDs: a-z 0-9 _ - */
    char    label[33];             /* HA's name for the device; empty: the name */
    char    prefix[25];            /* ids=esphome: the ESP32's entity ID prefix, a-z 0-9 _ */
    uint8_t port, address, controls, ids;   /* ids: 0 own IDs, 1 the ESP32's */
} dev_cfg_t;
```

in `cfg_t` after `more_cfg_t more;`:

```c
    dev_cfg_t  dev[DEVS];
```

and with the other declarations:

```c
/* Device i's lines (devN.*), or "devN.profile=" alone if there is none. 0 if they do not fit. */
size_t cfg_dev_lines(const cfg_t *c, int i, char *out, size_t max);
```

`src/app/cfg.c` — before `apply()`:

```c
static bool chars_ok(const char *v, const char *extra, size_t max)
{
    if (strlen(v) > max) return false;
    for (; *v; v++) if (!((*v >= 'a' && *v <= 'z') || (*v >= '0' && *v <= '9') || strchr(extra, *v))) return false;
    return true;
}

/* devN.key=value. An empty profile removes the device. */
static bool dev_apply(dev_cfg_t *d, const char *k, const char *v)
{
    uint32_t n;
    if (!strcmp(k, "profile")) {
        if (!*v) { memset(d, 0, sizeof *d); return true; }
        return chars_ok(v, "-", 24) && set_str(d->profile, sizeof d->profile, v, false);
    }
    if (!strcmp(k, "name")) return chars_ok(v, "_-", 24) && set_str(d->name, sizeof d->name, v, false);
    if (!strcmp(k, "label")) return set_str(d->label, sizeof d->label, v, false);
    if (!strcmp(k, "prefix")) return chars_ok(v, "_", 24) && set_str(d->prefix, sizeof d->prefix, v, false);
    if (!strcmp(k, "ids")) {
        if (strcmp(v, "own") && strcmp(v, "esphome")) return false;
        d->ids = v[0] == 'e';
        return true;
    }
    if (!parse_u32(v, &n)) return false;
    if (!strcmp(k, "port") && n >= 1 && n <= PORTS) { d->port = (uint8_t)n; return true; }
    if (!strcmp(k, "address") && n >= 1 && n <= 247) { d->address = (uint8_t)n; return true; }
    if (!strcmp(k, "controls") && n <= 1) { d->controls = (uint8_t)n; return true; }
    return false;
}
```

At the start of `apply()`:

```c
    if (!strncmp(k, "dev", 3) && k[3] >= '1' && k[3] <= '9') {
        char *e;
        long n = strtol(k + 3, &e, 10);
        if (n < 1 || n > DEVS || *e != '.') return false;
        return dev_apply(&c->dev[n - 1], e + 1, v);
    }
```

At the end of `cfg_validate()`, before `return true;`:

```c
    for (int i = 0; i < DEVS; i++) {
        const dev_cfg_t *d = &c->dev[i];
        if (!d->profile[0]) continue;
        if (!d->name[0]) { snprintf(err, errlen, "Device %d: give it a name", i + 1); return false; }
        if (d->port < 1 || d->port > PORTS) { snprintf(err, errlen, "Device %d: choose its port", i + 1); return false; }
        if (!d->address) { snprintf(err, errlen, "Device %d: give its address, 1 to 247", i + 1); return false; }
        if (c->port[d->port - 1].mode != MODE_MODBUS) {
            snprintf(err, errlen, "Device %d: port %d is not a Modbus gateway", i + 1, d->port);
            return false;
        }
        if (d->ids && !d->prefix[0]) { snprintf(err, errlen, "Device %d: ESPHome naming needs the ESP32's prefix", i + 1); return false; }
        for (int j = 0; j < i; j++) {
            const dev_cfg_t *o = &c->dev[j];
            if (!o->profile[0]) continue;
            if (!strcmp(o->name, d->name)) { snprintf(err, errlen, "Devices %d and %d are both called %s", j + 1, i + 1, d->name); return false; }
            if (o->port == d->port && o->address == d->address) {
                snprintf(err, errlen, "Devices %d and %d both use address %d on port %d", j + 1, i + 1, d->address, d->port);
                return false;
            }
        }
    }
```

After `cfg_lines()`:

```c
size_t cfg_dev_lines(const cfg_t *c, int i, char *out, size_t max)
{
    const dev_cfg_t *d = &c->dev[i];
    size_t n = 0;
    if (!max) return 0;
    if (!d->profile[0]) { ADD("dev%d.profile=\n", i + 1); return n; }
    ADD("dev%d.profile=%s\ndev%d.name=%s\ndev%d.label=%s\ndev%d.port=%u\ndev%d.address=%u\n"
        "dev%d.controls=%u\ndev%d.ids=%s\ndev%d.prefix=%s\n",
        i + 1, d->profile, i + 1, d->name, i + 1, d->label, i + 1, d->port, i + 1, d->address,
        i + 1, d->controls, i + 1, d->ids ? "esphome" : "own", i + 1, d->prefix);
    return n;
}
```

`src/app/cfg_store.c` — after the `MADDR` lines:

```c
/* Devices (devN.*): their own record too. One that cannot be read: no devices. */
#define DMAGIC  0x56454444u
#define DSCHEMA 1u
#define DSIZE   (DEVS * sizeof(dev_cfg_t))
_Static_assert(sizeof(dev_cfg_t) == 112, "device settings layout must not move");
static const uint32_t DADDR[2] = { 0x0EE000u, 0x0EF000u };
```

`rec_t`'s body becomes `uint8_t body[DSIZE > SIZE2 ? DSIZE : SIZE2];`; add `_Static_assert(DSIZE <= sizeof(((rec_t *)0)->body), "record too small");` after it. Add `static dev_cfg_t s_dsaved[DEVS];` and `static side_t s_dev = { DMAGIC, DSCHEMA, DSIZE, DADDR, -1, 0 };`. Then:

```c
static void dev_load(cfg_t *c)
{
    memset(c->dev, 0, sizeof c->dev);
    if (side_load(&s_dev, c->dev) != 1) memset(c->dev, 0, sizeof c->dev);
    for (int i = 0; i < DEVS; i++) {                    /* terminated, whatever was stored */
        dev_cfg_t *d = &c->dev[i];
        d->profile[sizeof d->profile - 1] = d->name[sizeof d->name - 1] = 0;
        d->label[sizeof d->label - 1] = d->prefix[sizeof d->prefix - 1] = 0;
    }
    memcpy(s_dsaved, c->dev, sizeof s_dsaved);
}
```

In `cfg_load()` after `more_load(c);`: `dev_load(c);`. In `cfg_save()` after the `more` block:

```c
    if (memcmp(s_dsaved, c->dev, sizeof s_dsaved)) {
        if (!side_save(&s_dev, c->dev)) return false;
        memcpy(s_dsaved, c->dev, sizeof s_dsaved);
    }
```

(`cfg_load()` validates the main record before `dev_load()` runs, so devices whose port is no longer a Modbus gateway after a downgrade-and-upgrade are dropped by the next `cfg_validate()` the web page runs; `device.c` shows them as "Its port is off, or not a Modbus gateway" meanwhile.)

- [ ] **Step 4: Run the tests**

Run: `bash tests/host.sh 2>&1 | grep -v ': ok'`
Expected: only the informational lines; `cfg`, `store`, `cli` pass.

- [ ] **Step 5: Commit**

```bash
git add src/app/cfg.h src/app/cfg.c src/app/cfg_store.c tests/test_cfg.c tests/test_store.c
git commit -m "Settings: devices (devN.*), in a record of their own

Up to 16: profile, name, label, port (a Modbus gateway), address,
controls, HA naming (own or the ESP32's, with its prefix). Kept at
0x0EE000/0x0EF000, so older firmware never reads them.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK"
```

### Task 6: The engine (`device.c`)

**Files:**
- Create: `src/app/device.h`, `src/app/device.c`, `tests/test_device.c`
- Modify: `tests/host.sh`

**Interfaces:**
- Consumes: `modbus_local`, `modbus_t.turn_us`, `MB_LOCAL` (Task 1); `profile_parse`, `profile_block` (Task 2); `pf_*` (Task 3); `profiles_text` (Task 4); `dev_cfg_t`, `DEVS`, `json_str` (Task 5); `modbus_t *bridge_modbus_port(int port)` (declared here for Task 7 to implement in `bridge.c`; the test stubs it).
- Produces: `device_t` (below); `void device_init(void)`; `void device_configure(const cfg_t *c)`; `void device_poll(void)`; `void device_reply(int port, uint32_t tag, int status, const uint8_t *pdu, size_t n)`; `const device_t *device_get(int i)`; `uint16_t device_line_use(int port)` (permille, last full minute); `size_t device_value(const device_t *d, int f, char *out, size_t max)` (a field's value as JSON); `size_t device_json(int i, char *out, size_t max)`; `void device_on_gone(void (*fn)(int i, const device_t *d))`; `void device_hold(const profile_t *p, int delta)`.

- [ ] **Step 1: Create `src/app/device.h`**

```c
/*
 * device.h — Devices on the RS485 ports, polled for their profiles' values. Each port's
 * devices take turns in its Modbus queue (one request of ours at a time); a reply is
 * decoded into the device's values; misses make blocks stale and devices offline.
 */
#ifndef DEVICE_H
#define DEVICE_H
#include "cfg.h"
#include "modbus.h"
#include "pfdecode.h"

typedef struct {
    bool       used;
    uint8_t    gen, slot;          /* gen: bumps when reconfigured (replies to the old one are ignored) */
    const profile_t *p;            /* NULL: not polled; see error */
    dev_cfg_t  cfg;
    pf_state_t st;
    uint32_t   due[PF_BLOCKS];     /* ms: each block's next read */
    uint8_t    miss[PF_BLOCKS];    /* in a row */
    uint16_t   have, stale;        /* blocks with values now; blocks whose values were dropped */
    bool       online;             /* a block not marked info has values */
    uint32_t   seq;                /* +1 on any change of values, online or error */
    uint32_t   polls, misses, last_ok;
    char       why[48];            /* the last miss */
    char       error[72];          /* why it is not polled */
} device_t;

void  device_init(void);
void  device_configure(const cfg_t *c);   /* devices whose settings changed start afresh */
void  device_poll(void);                  /* main loop */
/* A request of ours ended (modbus.c's io.local, through bridge.c). */
void  device_reply(int port, uint32_t tag, int status, const uint8_t *pdu, size_t n);
const device_t *device_get(int i);        /* NULL: no device i */
uint16_t device_line_use(int port);       /* the line's use by our requests, permille, last minute */
/* Field f's value as JSON: a number, true/false, a string, or null if not known. 0 if it does not fit. */
size_t device_value(const device_t *d, int f, char *out, size_t max);
size_t device_json(int i, char *out, size_t max);   /* settings, state and values; 0 if it does not fit */
/* Told before device i is removed or changed (devmqtt.c, to clear it from HA). It may
 * hold the device's profile meanwhile, so that it stays loaded. */
void  device_on_gone(void (*fn)(int i, const device_t *d));
void  device_hold(const profile_t *p, int delta);
/* Implemented by bridge.c: port's Modbus state, or NULL if it is off or not a gateway. */
modbus_t *bridge_modbus_port(int port);

#endif
```

- [ ] **Step 2: Write the failing test `tests/test_device.c`**

```c
/* test_device.c — the engine (src/app/device.c) with the EG4 profile, a real Modbus queue
 * (modbus.c) and a fake line and clock.
 *   see tests/host.sh for the build line */
#include "device.h"
#include "../tests/fuzz/json_ok.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t t_now = 1000;
uint32_t SystemCoreClock = 400000000;
static modbus_t M[PORTS];
static bool on[PORTS];
modbus_t *bridge_modbus_port(int p) { return on[p] ? &M[p] : NULL; }

static uint8_t L[4096]; static size_t Ln;
static size_t line(void *ctx, const uint8_t *d, size_t n) { (void)ctx; memcpy(L + Ln, d, n); Ln += n; return n; }
static void reply(void *ctx, int c, const uint8_t *d, size_t n) { (void)ctx; (void)c; (void)d; (void)n; }
static void local(void *ctx, uint32_t tag, int st, const uint8_t *pdu, size_t n) { device_reply((int)(intptr_t)ctx, tag, st, pdu, n); }
static int gone_n; static char gone_name[25];
static void gone(int i, const device_t *d) { gone_n++; assert(i == 1); snprintf(gone_name, sizeof gone_name, "%s", d->cfg.name); }

static void run(void) { device_poll(); modbus_poll(&M[1], t_now, 1000000, true); modbus_poll(&M[1], t_now, 1000000, true); }
static void answer(const uint8_t *f, size_t n)             /* a frame without its CRC */
{
    uint8_t b[300];
    memcpy(b, f, n);
    uint16_t c = modbus_crc(b, n);
    b[n] = (uint8_t)c; b[n + 1] = (uint8_t)(c >> 8);
    modbus_from_line(&M[1], b, n + 2);
    modbus_poll(&M[1], t_now, 0, true);
    modbus_poll(&M[1], t_now, 1000000, true);
}
/* What went out: unit, start, count. */
static bool sent(uint8_t unit, uint16_t start, uint16_t count)
{
    bool ok = Ln == 8 && L[0] == unit && L[1] == 3 && (L[2] << 8 | L[3]) == start && (L[4] << 8 | L[5]) == count;
    if (!ok) printf("sent %zu: %02x %02x %02x%02x %02x%02x, wanted %02x %04x %u\n", Ln, L[0], L[1], L[2], L[3], L[4], L[5], unit, start, count);
    Ln = 0;
    return ok;
}
static uint32_t tag(int i, int b) { return (uint32_t)device_get(i)->gen << 16 | (uint32_t)i << 8 | (uint32_t)b; }
static size_t live1(uint8_t *pdu, uint16_t volts)            /* a reply PDU: function, count, 18 registers */
{
    memset(pdu, 0, 38);
    pdu[0] = 3; pdu[1] = 36;
    pdu[2] = (uint8_t)(volts >> 8); pdu[3] = (uint8_t)volts;
    for (int k = 0; k < 16; k++) { pdu[6 + 2 * k] = 0x0C; pdu[7 + 2 * k] = 0xE5; }   /* 3.301 V */
    return 38;
}
static bool known(int dev, const char *id) { const device_t *d = device_get(dev); return pf_ok(&d->st, profile_field(d->p, id)); }

int main(void)
{
    static cfg_t c;
    static uint8_t pdu[300];
    cfg_defaults(&c);
    c.port[1].mode = MODE_MODBUS;
    const dev_cfg_t D[4] = {
        { "eg4-bms", "eg4m", "", "", 2, 0x10, 0, 0 },
        { "eg4-bms", "p1", "", "", 2, 1, 0, 0 },
        { "nope", "x", "", "", 2, 3, 0, 0 },
        { "eg4-bms", "p3", "", "", 3, 1, 0, 0 },
    };
    memcpy(c.dev, D, sizeof D);
    device_init();
    device_on_gone(gone);
    device_configure(&c);
    assert(device_get(0)->p && device_get(1)->p == device_get(0)->p && !device_get(2)->p && device_get(4) == NULL);
    assert(strstr(device_get(2)->error, "No profile 'nope'"));

    /* Turns: each device's blocks in order, devices alternating; then nothing until due. */
    const mb_io_t io = { line, reply, local, (void *)(intptr_t)1 };
    modbus_init(&M[1], &io, 1000, 2000);
    on[1] = true;
    static const uint16_t BL[5][2] = { { 0x0000, 18 }, { 0x0012, 21 }, { 0x0069, 11 }, { 0x0075, 3 }, { 0x0078, 8 } };
    for (int b = 0; b < 5; b++)
        for (int dv = 0; dv < 2; dv++) {
            run();
            assert(sent(dv ? 1 : 0x10, BL[b][0], BL[b][1]));
            answer((const uint8_t[]){ dv ? 1 : 0x10, 0x83, 2 }, 3);        /* "illegal address": a miss */
        }
    assert(M[1].turn_us == 50000);
    assert(strstr(device_get(3)->error, "Its port is off"));                /* port 3 is not on */
    run();
    assert(Ln == 0);
    assert(device_get(0)->misses == 5 && strstr(device_get(0)->why, "Error 2 from the device"));
    t_now += 10000;
    run();
    assert(sent(0x10, 0x0000, 18));                                         /* due again */

    /* A reply: values, online. */
    uint32_t s0 = device_get(0)->seq;
    size_t n = live1(pdu, 5312);
    uint8_t fr[60] = { 0x10 };
    memcpy(fr + 1, pdu, n);
    answer(fr, n + 1);
    const device_t *d0 = device_get(0);
    assert(d0->online && d0->seq > s0 && known(0, "total_voltage") && known(0, "min_cell_voltage"));
    assert(d0->st.val[profile_field(d0->p, "total_voltage")].v > 53.11f);

    /* Misses: after `stale` (5) in a row the block's values go, and the device is offline. */
    for (int k = 0; k < 4; k++) device_reply(1, tag(0, 0), -1, NULL, 0);
    assert(known(0, "total_voltage") && d0->online);
    device_reply(1, tag(0, 0), -1, NULL, 0);
    assert(!known(0, "total_voltage") && !known(0, "power") && !d0->online && (d0->stale & 1));
    assert(strstr(d0->why, "No reply (live1)"));
    /* A reply the profile rejects: a miss, values kept. */
    device_reply(1, tag(0, 0), 0, pdu, live1(pdu, 5312));
    assert(d0->online && known(0, "total_voltage") && !(d0->stale & 1));
    device_reply(1, tag(0, 0), 0, pdu, live1(pdu, 0));
    assert(known(0, "total_voltage") && strstr(d0->why, "A reply its profile rejects"));
    /* The wrong size, and a reply for a device since changed: */
    pdu[1] = 34;
    device_reply(1, tag(0, 0), 0, pdu, 36);
    assert(strstr(d0->why, "A reply of the wrong size"));
    uint32_t m0 = d0->misses;
    device_reply(1, (uint32_t)(d0->gen + 1) << 16 | 0, -1, NULL, 0);
    assert(d0->misses == m0);

    /* Reconfigure one device: only it starts afresh, and HA is told it went. */
    c.dev[1].address = 2;
    device_configure(&c);
    assert(gone_n == 1 && !strcmp(gone_name, "p1") && device_get(1)->polls == 0 && known(0, "total_voltage"));

    /* A request whose end never comes: the port is free again after 15 s. */
    Ln = 0;
    t_now += 20000;
    device_poll();                                       /* queued */
    M[1].qn = 0; M[1].state = MB_IDLE;                   /* ... and lost */
    run(); assert(Ln == 0);
    t_now += 15000;
    run(); assert(Ln == 8);

    /* The line's use, over a minute. */
    t_now += 61000;
    device_poll();
    assert(device_line_use(1) > 0 && device_line_use(1) <= 1000);

    /* As /api/devices shows it. */
    static char j[4600];
    n = device_json(0, j, sizeof j);
    assert(n && json_ok(j) && strstr(j, "\"name\":\"eg4m\"") && strstr(j, "\"total_voltage\""));
    for (size_t k = 0; k < n; k += 97) assert(!device_json(0, j, k));
    assert(!device_json(2, j, sizeof j) || strstr(j, "No profile"));
    puts("device: ok");
    return 0;
}
```

- [ ] **Step 3: Run to see it fail** — add to `tests/host.sh` after `t builtin`:

```sh
t device -DD2_BSS= -I"$O" -Itests/stub -Isrc/app tests/test_device.c src/app/device.c src/app/modbus.c src/app/profiles.c src/app/profile.c src/app/pfdecode.c src/app/cfg.c src/app/acl.c -lm
```

Run: `bash tests/host.sh | grep device` — Expected: `device: BUILD FAILED` (no `device.c`).

- [ ] **Step 4: Create `src/app/device.c`**

```c
/*
 * device.c — Devices polled for their profiles' values (device.h). Per port: which block
 * of which device is most overdue goes next, as one request in the port's Modbus queue
 * (modbus.c's local client); its reply is decoded (pfdecode.c). Profiles are parsed once
 * and shared by the devices that use them.
 */
#include "device.h"
#include "profiles.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>

#ifndef D2_BSS
#define D2_BSS __attribute__((section(".d2_bss")))
#endif
#define LOADED  3                /* different profiles at once */
#define LOST_MS 15000u           /* a request whose end never came: the port is free again */

static D2_BSS profile_t s_prof[LOADED];
static D2_BSS device_t s_dev[DEVS];
static bool    s_loaded[LOADED];
static uint8_t s_users[LOADED], s_holds[LOADED];
static struct { bool busy; uint8_t rr; uint16_t use; uint32_t since, busy_ms, win_at; } s_port[PORTS];
static uint32_t s_baud[PORTS];
static void (*s_gone)(int i, const device_t *d);

void device_init(void)
{
    memset(s_dev, 0, sizeof s_dev);            /* D2 is not zeroed at start */
    memset(s_loaded, 0, sizeof s_loaded);
    memset(s_users, 0, sizeof s_users);
    memset(s_holds, 0, sizeof s_holds);
    memset(s_port, 0, sizeof s_port);
}

void device_on_gone(void (*fn)(int i, const device_t *d)) { s_gone = fn; }

void device_hold(const profile_t *p, int delta)
{
    for (int s = 0; s < LOADED; s++) if (p == &s_prof[s]) s_holds[s] = (uint8_t)(s_holds[s] + delta);
}

const device_t *device_get(int i) { return i >= 0 && i < DEVS && s_dev[i].used ? &s_dev[i] : NULL; }
uint16_t device_line_use(int port) { return port >= 0 && port < PORTS ? s_port[port].use : 0; }

/* The device's profile: already loaded, or parsed into a free place. */
static const profile_t *load(device_t *d, const char *id)
{
    static char err[64];
    size_t n;
    int free_ = -1;
    for (int s = 0; s < LOADED; s++) {
        if (s_loaded[s] && !strcmp(pf_str(&s_prof[s], s_prof[s].id), id)) { s_users[s]++; d->slot = (uint8_t)s; return &s_prof[s]; }
        if (!s_users[s] && !s_holds[s] && free_ < 0) free_ = s;
    }
    const char *t = profiles_text(id, &n);
    if (!t) { snprintf(d->error, sizeof d->error, "No profile '%s'", id); return NULL; }
    if (free_ < 0) { snprintf(d->error, sizeof d->error, "More than %d different profiles are in use", LOADED); return NULL; }
    s_loaded[free_] = false;
    if (!profile_parse(&s_prof[free_], t, n, err, sizeof err)) {
        snprintf(d->error, sizeof d->error, "Profile %s: %s", id, err);
        return NULL;
    }
    s_loaded[free_] = true;
    s_users[free_] = 1;
    d->slot = (uint8_t)free_;
    return &s_prof[free_];
}

void device_configure(const cfg_t *c)
{
    for (int i = 0; i < PORTS; i++) s_baud[i] = c->port[i].baud ? c->port[i].baud : 9600;
    for (int i = 0; i < DEVS; i++) {
        device_t *d = &s_dev[i];
        const dev_cfg_t *k = &c->dev[i];
        bool want = k->profile[0] != 0;
        if (d->used == want && (!want || !memcmp(&d->cfg, k, sizeof *k))) continue;
        if (d->used) {
            if (s_gone) s_gone(i, d);
            if (d->p) s_users[d->slot]--;
        }
        uint8_t gen = (uint8_t)(d->gen + 1);
        memset(d, 0, sizeof *d);
        d->gen = gen;
        if (!want) continue;
        d->used = true;
        d->cfg = *k;
        d->seq = 1;
        d->p = load(d, k->profile);
        for (int b = 0; d->p && b < d->p->nblock; b++) d->due[b] = HAL_GetTick();
    }
}

static void online(device_t *d)
{
    bool on = false;
    for (int b = 0; b < d->p->nblock; b++) on = on || ((d->have >> b & 1u) && !d->p->block[b].info);
    if (on != d->online) { d->online = on; d->seq++; }
}

static void miss(device_t *d, int b, const char *why)
{
    d->misses++;
    snprintf(d->why, sizeof d->why, "%s (%s)", why, pf_str(d->p, d->p->block[b].id));
    if (d->miss[b] < 255) d->miss[b]++;
    if (d->miss[b] >= d->p->stale && (d->have >> b & 1u)) {
        pf_drop(d->p, b, &d->st);
        d->have &= (uint16_t)~(1u << b);
        d->stale |= (uint16_t)(1u << b);
        d->seq++;
    }
    online(d);
}

/* Roughly how long the line was ours: request and reply at 11 bits a byte, the
 * turnaround, and a timeout's whole wait. */
static void used_line(int port, const device_t *d, size_t n, bool timeout)
{
    uint32_t bytes = 8u + (n ? (uint32_t)n + 3u : 0u);
    s_port[port].busy_ms += bytes * 11000u / s_baud[port] + d->p->turn_ms + (timeout ? d->p->timeout_ms : 0u);
}

static void window(int port, uint32_t now)
{
    uint32_t el = now - s_port[port].win_at;
    if (el < 60000u) return;
    uint32_t pm = s_port[port].busy_ms * 1000u / el;
    s_port[port].use = (uint16_t)(pm > 1000u ? 1000u : pm);
    s_port[port].busy_ms = 0;
    s_port[port].win_at = now;
}

void device_reply(int port, uint32_t tag, int st, const uint8_t *pdu, size_t n)
{
    int i = (int)(tag >> 8 & 0xFFu), b = (int)(tag & 0xFFu);
    if (port >= 0 && port < PORTS) s_port[port].busy = false;
    if (i >= DEVS || port < 0 || port >= PORTS) return;
    device_t *d = &s_dev[i];
    if (!d->p || d->gen != (uint8_t)(tag >> 16) || b >= d->p->nblock) return;
    const pf_block_t *k = &d->p->block[b];
    used_line(port, d, st == 0 ? n : 0, st != 0);
    if (st) { miss(d, b, "No reply"); return; }
    if (n >= 2 && (pdu[0] & 0x80)) {
        char w[32];
        snprintf(w, sizeof w, "Error %u from the device", pdu[1]);
        miss(d, b, w);
        return;
    }
    size_t want = k->fn <= 2 ? (k->count + 7u) / 8u : k->count * 2u;
    if (n < 2 || pdu[0] != k->fn || pdu[1] != want || n != 2 + want) { miss(d, b, "A reply of the wrong size"); return; }
    if (!pf_decode(d->p, b, pdu + 2, want, &d->st)) { miss(d, b, "A reply its profile rejects"); return; }
    d->miss[b] = 0;
    d->have |= (uint16_t)(1u << b);
    d->stale &= (uint16_t)~(1u << b);
    d->last_ok = HAL_GetTick();
    d->seq++;
    online(d);
}

/* The most overdue block among the port's devices, starting after the last one asked. */
static void submit(int port, modbus_t *m, uint32_t now)
{
    int best = -1, bb = 0;
    uint32_t late = 0;
    for (int k = 0; k < DEVS; k++) {
        int i = (s_port[port].rr + k) % DEVS;
        device_t *d = &s_dev[i];
        if (!d->p || d->cfg.port != port + 1) continue;
        for (int b = 0; b < d->p->nblock; b++) {
            int32_t l = (int32_t)(now - d->due[b]);
            if (l < 0 || (best >= 0 && (uint32_t)l <= late)) continue;
            best = i;
            bb = b;
            late = (uint32_t)l;
        }
    }
    if (best < 0) return;
    device_t *d = &s_dev[best];
    const pf_block_t *b = &d->p->block[bb];
    const uint8_t pdu[5] = { b->fn, (uint8_t)(b->addr >> 8), (uint8_t)b->addr, (uint8_t)(b->count >> 8), (uint8_t)b->count };
    if (!modbus_local(m, d->cfg.address, pdu, 5, d->p->timeout_ms, (uint32_t)d->gen << 16 | (uint32_t)best << 8 | (uint32_t)bb))
        return;                                  /* 8 waiting already: next pass */
    s_port[port].busy = true;
    s_port[port].since = now;
    s_port[port].rr = (uint8_t)((best + 1) % DEVS);
    d->due[bb] = now + b->period_ms;
    d->polls++;
}

void device_poll(void)
{
    uint32_t now = HAL_GetTick();
    for (int port = 0; port < PORTS; port++) {
        uint16_t turn = 0;
        bool any = false;
        for (int i = 0; i < DEVS; i++)
            if (s_dev[i].p && s_dev[i].cfg.port == port + 1) {
                any = true;
                if (s_dev[i].p->turn_ms > turn) turn = s_dev[i].p->turn_ms;
            }
        if (!any) continue;
        modbus_t *m = bridge_modbus_port(port);
        for (int i = 0; i < DEVS; i++)               /* say why it is not polled, or that it is again */
            if (s_dev[i].p && s_dev[i].cfg.port == port + 1) {
                const char *e = m ? "" : "Its port is off, or not a Modbus gateway";
                if (strcmp(s_dev[i].error, e)) { snprintf(s_dev[i].error, sizeof s_dev[i].error, "%s", e); s_dev[i].seq++; }
            }
        window(port, now);
        if (!m) continue;
        m->turn_us = (uint32_t)turn * 1000u;
        if (s_port[port].busy && now - s_port[port].since < LOST_MS) continue;
        s_port[port].busy = false;
        submit(port, m, now);
    }
}

size_t device_value(const device_t *d, int f, char *out, size_t max)
{
    const pf_field_t *x = &d->p->field[f];
    bool ok = pf_ok(&d->st, f);
    int w;
    if (!ok) w = snprintf(out, max, "null");
    else if (x->kind == PF_ENUM || x->kind == PF_BITS || x->kind == PF_ASCII) {
        char t[160];
        pf_text(d->p, f, &d->st, t, sizeof t);
        return json_str(t, out, max);
    } else if (x->kind == PF_BINARY || x->kind == PF_CALCBIN) w = snprintf(out, max, "%s", d->st.val[f].v != 0 ? "true" : "false");
    else return max > 1 ? pf_num(out, max, d->st.val[f].v, pf_prec(x)) : 0;
    return w < 0 || (size_t)w >= max ? 0 : (size_t)w;
}

size_t device_json(int i, char *out, size_t max)
{
    const device_t *d = device_get(i);
    size_t n = 0;
    char s[3][80];
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= max - n) return 0; n += (size_t)w_; } while (0)
    if (!d || !max) return 0;
    if (!json_str(d->cfg.label, s[0], sizeof s[0]) || !json_str(d->error, s[1], sizeof s[1]) || !json_str(d->why, s[2], sizeof s[2])) return 0;
    OUT("{\"n\":%d,\"profile\":\"%s\",\"name\":\"%s\",\"label\":%s,\"port\":%u,\"address\":%u,\"controls\":%s,"
        "\"ids\":\"%s\",\"prefix\":\"%s\",\"error\":%s,\"online\":%s,\"polls\":%lu,\"misses\":%lu,\"why\":%s,\"ago_ms\":%ld",
        i + 1, d->cfg.profile, d->cfg.name, s[0], d->cfg.port, d->cfg.address, d->cfg.controls ? "true" : "false",
        d->cfg.ids ? "esphome" : "own", d->cfg.prefix, s[1], d->online ? "true" : "false",
        (unsigned long)d->polls, (unsigned long)d->misses, s[2],
        d->last_ok ? (long)((HAL_GetTick() - d->last_ok) & 0x7FFFFFFF) : -1L);
    if (!d->p) { OUT("}"); return n; }
    if (!json_str(pf_str(d->p, d->p->name), s[0], sizeof s[0]) || !json_str(pf_str(d->p, d->p->maker), s[1], sizeof s[1])) return 0;
    OUT(",\"model\":%s,\"maker\":%s,\"values\":[", s[0], s[1]);
    for (int f = 0; f < d->p->nfield; f++) {
        const pf_field_t *x = &d->p->field[f];
        char v[200], nm[96], u[40];
        if (!device_value(d, f, v, sizeof v)) return 0;
        if (!json_str(pf_str(d->p, x->name), nm, sizeof nm) || !json_str(pf_str(d->p, x->unit), u, sizeof u)) return 0;
        OUT("%s[\"%s\",%s,%s,%s]", f ? "," : "", pf_str(d->p, x->id), nm, v, u);
    }
    OUT("]}");
    return n;
#undef OUT
}
```

(`json_str` comes from `cfg.c`; it returns 0 when the text does not fit. A device's `values` are `[id, name, value, unit]` each; a value is a number, `true`/`false`, a string, or `null`.)

- [ ] **Step 5: Run the tests**

Run: `bash tests/host.sh | grep -E 'device|FAILED'`
Expected: `device: ok`.

- [ ] **Step 6: Commit**

```bash
git add src/app/device.h src/app/device.c tests/test_device.c tests/host.sh
git commit -m "Devices: the polling engine

device.c polls each configured device's blocks through its port's
Modbus queue, one request of ours at a time, the most overdue next,
devices in turn; replies are decoded by its profile; misses make
blocks stale and the device offline; profiles are parsed once and
shared. /api/devices' JSON comes from here.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK"
```

### Task 7: Wiring the engine in (bridge, main loop, settings applied)

**Files:**
- Modify: `src/app/bridge.c`, `src/app/cfg.c` (a Modbus port may have no TCP port), `src/app/main.c`, `src/app/api.c`
- Test: `tests/test_bridge.c`, `tests/test_cfg.c`

**Interfaces:**
- Consumes: `device_reply`, `device_init`, `device_configure`, `device_poll`, `bridge_modbus_port` (declared in `device.h`, Task 6).
- Produces: `modbus_t *bridge_modbus_port(int port)` in `bridge.c`; Modbus ports with `local_port=0` poll only (state "ready", no listener).

- [ ] **Step 1: Write the failing tests** — `tests/test_bridge.c`, at file scope after the `uart485_*` fakes:

```c
#include "device.h"
static uint32_t dr_tag; static int dr_st = 9; static size_t dr_n;
void device_reply(int port, uint32_t tag, int st, const uint8_t *pdu, size_t n) { (void)port; (void)pdu; dr_tag = tag; dr_st = st; dr_n = n; }
```

and before the widest-JSON section:

```c
    /* A Modbus port polled only: no listener, ready; the engine's requests reach the line,
     * and their replies reach device_reply(). */
    cfg_defaults(&C);
    set("port3.enabled=1\nport3.mode=modbus\nport3.local_port=0");
    assert(!strcmp(info(2).state, "ready") && bridge_modbus_port(2) && !bridge_modbus_port(0) && !bridge_modbus_port(3));
    assert(modbus_local(bridge_modbus_port(2), 5, (const uint8_t[]){ 3, 0, 0, 0, 1 }, 5, 0, 42));
    bridge_poll();
    assert(lineb(2, "05 03 00 00 00 01 85 8e"));
    heardb(2, "05 03 02 00 07 08 46");
    assert(dr_tag == 42 && dr_st == 0 && dr_n == 4);
    set("port3.enabled=0\nport3.mode=server\nport3.local_port=4003");
    assert(!bridge_modbus_port(2));
```

(`test_bridge` does not link `device.c`: the stub stands in for it.)

`tests/test_cfg.c`, in the Modbus block of Task 5 (after `OK("port2.enabled=1\nport2.mode=modbus\nport2.local_port=502");`):

```c
    OK("port2.local_port=0");                             /* polled only: no TCP */
    BAD("port1.local_port=0");                            /* a TCP server needs one */
    OK("port2.local_port=502");
```

- [ ] **Step 2: Run to see them fail**

Run: `bash tests/host.sh 2>&1 | grep -E 'bridge|cfg'` — Expected: `bridge: BUILD FAILED` (`bridge_modbus_port` undefined) and `cfg` failing on `port2.local_port=0`.

- [ ] **Step 3: Implement** — `src/app/bridge.c`: `#include "device.h"`; in the Modbus section:

```c
static void mb_local(void *ctx, uint32_t tag, int st, const uint8_t *pdu, size_t n)
{
    device_reply((int)((port_t *)ctx - s_p), tag, st, pdu, n);
}

/* The port's Modbus state, for device.c; NULL if it is off or not a gateway. */
modbus_t *bridge_modbus_port(int i)
{
    return i >= 0 && i < PORTS && is_mb(i) && s_p[i].state != LS_OFF ? &s_mb[i] : NULL;
}
```

`mb_start()`: `const mb_io_t io = { mb_line, mb_reply, mb_local, &s_p[i] };`. `port_start()`:

```c
    case MODE_MODBUS:
        mb_start(i);
        if (p->cfg.local_port) start_listen(p);
        else p->state = LS_BOUND;             /* polled only: no TCP clients */
        break;
```

and in `bridge_poll()`'s retry: `if (p->cfg.mode == MODE_SERVER || (p->cfg.mode == MODE_MODBUS && p->cfg.local_port)) start_listen(p);` followed by the existing `else if (p->cfg.mode == MODE_UDP && !p->udp) start_udp(p);`, then `else if (p->cfg.mode == MODE_MODBUS) p->state = LS_BOUND;`, then `else start_resolve(p);`.

`src/app/cfg.c` `cfg_validate()`: `if (!p->local_port && p->mode != MODE_MODBUS) {` for the "enter a local port" check, and in the duplicate-port loop add `p->local_port &&` to the condition.

`src/app/main.c`: `#include "device.h"`; after `CRUMB(3); bridge_init();` add `device_init();`; in the loop after `STEP(15, "bridge"); bridge_poll();` add `STEP(19, "devices"); device_poll();`.

`src/app/api.c`: `#include "device.h"`; after each of the three `bridge_configure(&s_cfg);` add `device_configure(&s_cfg);`.

- [ ] **Step 4: Run tests and build**

Run: `bash tests/host.sh 2>&1 | grep -v ': ok'` and `cmake -B build && cmake --build build 2>&1 | grep -E '\berror\b'; arm-none-eabi-size build/app.elf`
Expected: tests pass; firmware builds (text about +15 KB; D2 use about +100 KB: check `RAM_D2` in the link summary stays under 100%).

- [ ] **Step 5: Commit**

```bash
git add src/app/bridge.c src/app/cfg.c src/app/main.c src/app/api.c tests/test_bridge.c tests/test_cfg.c
git commit -m "Devices: polled from the main loop, through the Modbus ports

A Modbus port with local port 0 is polled only (no TCP listener).
bridge.c hands device.c its Modbus state and passes replies back.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK"
```

### Task 8: MQTT and Home Assistant (`devmqtt.c`)

**Files:**
- Create: `src/app/devmqtt.h`, `src/app/devmqtt.c`, `tests/test_devmqtt.c`
- Modify: `src/app/mqtt_ha.c`, `src/app/main.c`, `tests/host.sh`

**Interfaces:**
- Consumes: `device_get`, `device_value`, `device_on_gone`, `device_hold` (Task 6); `json_str` (cfg); `pf_prec`, `pf_str`.
- Produces: `typedef bool (*devmqtt_pub_t)(const char *topic, const char *payload, bool retain)`; `void devmqtt_init(void)`; `void devmqtt_restart(void)`; `bool devmqtt_poll(devmqtt_pub_t pub, const char *base, const char *host, bool announce, uint32_t now)`.

Topics (spec, "MQTT and Home Assistant"): state `BASE/dev/<name>/state` (one JSON, retained, after a change at most once a second, at least once a minute); status `BASE/dev/<name>/status` (`online`/`offline`, retained); discovery `homeassistant/<sensor|binary_sensor>/HOST_<name>/<field>/config` (retained; empty to remove).

- [ ] **Step 1: Write the failing test `tests/test_devmqtt.c`**

```c
/* test_devmqtt.c — devices' MQTT topics and payloads (src/app/devmqtt.c): discovery for
 * every field, status, state, pacing, removal, HA naming. */
#include "devmqtt.h"
#include "device.h"
#include "../tests/fuzz/json_ok.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t t_now = 1000;
uint32_t SystemCoreClock = 400000000;
modbus_t *bridge_modbus_port(int p) { (void)p; return NULL; }

static struct { char topic[160], payload[4096]; bool retain; } Pb[400];
static int Pn;
static bool full;
static bool pub(const char *t, const char *p, bool r)
{
    if (full) return false;
    assert(Pn < 400 && strlen(p) < sizeof Pb[0].payload);
    snprintf(Pb[Pn].topic, sizeof Pb[Pn].topic, "%s", t);
    snprintf(Pb[Pn].payload, sizeof Pb[Pn].payload, "%s", p);
    Pb[Pn++].retain = r;
    return true;
}
static int drain(bool announce) { int n0 = Pn; for (int k = 0; k < 1000; k++) { int b = Pn; assert(devmqtt_poll(pub, "solar", "n540-test", announce, t_now)); if (Pn == b) break; } return Pn - n0; }
static const char *find(const char *topic) { for (int k = Pn - 1; k >= 0; k--) if (!strcmp(Pb[k].topic, topic)) return Pb[k].payload; return NULL; }
static uint32_t tag(int i, int b) { return (uint32_t)device_get(i)->gen << 16 | (uint32_t)i << 8 | (uint32_t)b; }

int main(void)
{
    static cfg_t c;
    static uint8_t pdu[60];
    cfg_defaults(&c);
    c.port[1].mode = MODE_MODBUS;
    c.dev[0] = (dev_cfg_t){ "eg4-bms", "eg4m", "EG4 master", "", 2, 0x10, 0, 0 };
    device_init();
    devmqtt_init();
    device_configure(&c);
    const device_t *d = device_get(0);
    memset(pdu, 0, sizeof pdu);
    pdu[0] = 3; pdu[1] = 36; pdu[2] = 0x14; pdu[3] = 0xC0;                       /* 53.12 V */
    for (int k = 0; k < 16; k++) { pdu[6 + 2 * k] = 0x0C; pdu[7 + 2 * k] = 0xE5; }
    device_reply(1, tag(0, 0), 0, pdu, 38);
    assert(d->online);

    /* Discovery: one config per field, then status, then state. */
    devmqtt_restart();
    int n = drain(true);
    assert(n == d->p->nfield + 2);
    for (int f = 0; f < d->p->nfield; f++) {
        const pf_field_t *x = &d->p->field[f];
        char t[160];
        bool bin = x->kind == PF_BINARY || x->kind == PF_CALCBIN;
        snprintf(t, sizeof t, "homeassistant/%s/n540-test_eg4m/%s/config", bin ? "binary_sensor" : "sensor", pf_str(d->p, x->id));
        const char *p = find(t);
        assert(p && json_ok(p) && strlen(p) < 1600);
        assert(strstr(p, "\"~\":\"solar/dev/eg4m\"") && strstr(p, "\"availability_mode\":\"all\"") && strstr(p, "\"via_device\":\"n540-test\""));
        char u[120];
        snprintf(u, sizeof u, "\"unique_id\":\"n540-test_eg4m_%s\"", pf_str(d->p, x->id));
        assert(strstr(p, u));
        snprintf(u, sizeof u, "\"default_entity_id\":\"%s.n540_eg4m_%s\"", bin ? "binary_sensor" : "sensor", pf_str(d->p, x->id));
        assert(strstr(p, u));
    }
    const char *tv = find("homeassistant/sensor/n540-test_eg4m/total_voltage/config");
    assert(strstr(tv, "\"unit_of_measurement\":\"V\"") && strstr(tv, "\"device_class\":\"voltage\"") &&
           strstr(tv, "\"state_class\":\"measurement\"") && strstr(tv, "\"suggested_display_precision\":2") &&
           strstr(tv, "\"name\":\"EG4 master\"") && strstr(tv, "\"manufacturer\":\"EG4\""));
    const char *ht = find("homeassistant/binary_sensor/n540-test_eg4m/heating/config");
    assert(strstr(ht, "'ON' if value_json.heating else 'OFF'") && strstr(ht, "\"device_class\":\"heat\""));
    assert(strstr(find("homeassistant/sensor/n540-test_eg4m/model/config"), "\"entity_category\":\"diagnostic\""));
    assert(!strcmp(find("solar/dev/eg4m/status"), "online"));
    const char *s = find("solar/dev/eg4m/state");
    assert(s && json_ok(s) && strlen(s) < 3584 && strstr(s, "\"total_voltage\":53.12") && strstr(s, "\"pcb_temperature\":null") &&
           strstr(s, "\"heating\":null") && strstr(s, "\"cell_voltage_16\":3.301"));
    for (int k = 0; k < Pn; k++) assert(Pb[k].retain);

    /* Pacing: a change is sent no sooner than a second after the last, and without one a minute later. */
    assert(drain(true) == 0);
    pdu[3] = 0xC1;
    device_reply(1, tag(0, 0), 0, pdu, 38);
    assert(drain(true) == 0);
    t_now += 1000;
    assert(drain(true) == 1 && strstr(find("solar/dev/eg4m/state"), "\"total_voltage\":53.13"));
    t_now += 59999;
    assert(drain(true) == 0);
    t_now += 1;
    assert(drain(true) == 1);

    /* No room: the same thing again next time. */
    full = true;
    devmqtt_restart();
    assert(!devmqtt_poll(pub, "solar", "n540-test", true, t_now));
    full = false;
    assert(drain(true) == d->p->nfield + 2);

    /* Discovery off: the configs go empty. */
    devmqtt_restart();
    Pn = 0;
    drain(false);
    assert(!strcmp(find("homeassistant/sensor/n540-test_eg4m/total_voltage/config"), ""));

    /* ESPHome naming: the ESP32's entity IDs. */
    c.dev[0].ids = 1;
    snprintf(c.dev[0].prefix, sizeof c.dev[0].prefix, "eg4_bms");
    Pn = 0;
    device_configure(&c);                                   /* a changed device: removed, then new */
    drain(true);
    assert(strstr(find("homeassistant/sensor/n540-test_eg4m/total_voltage/config"), "\"default_entity_id\":\"sensor.eg4_bms_total_voltage\""));
    assert(strstr(find("homeassistant/binary_sensor/n540-test_eg4m/heating/config"), "\"default_entity_id\":\"binary_sensor.eg4_bms_heating\""));
    assert(strstr(find("homeassistant/sensor/n540-test_eg4m/cell_voltage_3/config"), "\"default_entity_id\":\"sensor.eg4_bms_cell_voltage_3\""));

    /* Removed: every config, the state and the status emptied. */
    int nf = d->p->nfield;
    memset(&c.dev[0], 0, sizeof c.dev[0]);
    Pn = 0;
    device_configure(&c);
    drain(true);
    assert(Pn == nf + 2);
    int empty = 0;
    for (int k = 0; k < Pn; k++) empty += Pb[k].payload[0] == 0;
    assert(empty == Pn && find("solar/dev/eg4m/state") && find("solar/dev/eg4m/status"));
    puts("devmqtt: ok");
    return 0;
}
```

- [ ] **Step 2: Run to see it fail** — add to `tests/host.sh` after `t device`:

```sh
t devmqtt -DD2_BSS= -I"$O" -Itests/stub -Isrc/app tests/test_devmqtt.c src/app/devmqtt.c src/app/device.c src/app/modbus.c src/app/profiles.c src/app/profile.c src/app/pfdecode.c src/app/cfg.c src/app/acl.c -lm
```

Expected: `devmqtt: BUILD FAILED` (no `devmqtt.c`).

- [ ] **Step 3: Create `src/app/devmqtt.h`**

```c
/* devmqtt.h — Devices over MQTT, with Home Assistant discovery (devmqtt.c). mqtt_ha.c owns
 * the connection and calls these. */
#ifndef DEVMQTT_H
#define DEVMQTT_H
#include <stdbool.h>
#include <stdint.h>

typedef bool (*devmqtt_pub_t)(const char *topic, const char *payload, bool retain);
void devmqtt_init(void);                  /* after device_init() */
void devmqtt_restart(void);               /* a new connection, base topic or discovery setting: all again */
/* One thing published: a discovery config, a status, a state, or a removed device's
 * topics emptied. False if pub had no room: the same again next time. */
bool devmqtt_poll(devmqtt_pub_t pub, const char *base, const char *host, bool announce, uint32_t now);

#endif
```

- [ ] **Step 4: Create `src/app/devmqtt.c`**

```c
/*
 * devmqtt.c — Each device's values over MQTT: its state as one JSON object, its status,
 * and an HA discovery config per field; a removed device's emptied. One publish a call,
 * so the main loop keeps moving (mqtt_ha.c calls this each pass once its own are done).
 */
#include "devmqtt.h"
#include "device.h"
#include <stdio.h>
#include <string.h>

#define BUF 3584                    /* within lwIP MQTT's 4096-byte output buffer */
static char s_buf[BUF];
static struct { uint8_t gen; int16_t disc; bool status, online; uint32_t seq, at; } s_d[DEVS];
static struct { bool used; char name[25]; const profile_t *p; int16_t at; } s_gone[DEVS];

static const char *comp(const pf_field_t *f) { return f->kind == PF_BINARY || f->kind == PF_CALCBIN ? "binary_sensor" : "sensor"; }

/* "a_b" as an HA object id: lower case, anything but a-z 0-9 as _. */
static void object(char *out, size_t max, const char *a, const char *b)
{
    size_t n = 0;
    for (const char *s = a; *s && n + 1 < max; s++) out[n++] = (char)((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ? *s : *s >= 'A' && *s <= 'Z' ? *s + 32 : '_');
    if (n + 1 < max) out[n++] = '_';
    for (const char *s = b; *s && n + 1 < max; s++) out[n++] = (char)((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ? *s : *s >= 'A' && *s <= 'Z' ? *s + 32 : '_');
    out[n] = 0;
}

static void gone(int i, const device_t *d)
{
    (void)i;
    if (!d->p) return;
    for (int k = 0; k < DEVS; k++)
        if (!s_gone[k].used) {
            s_gone[k].used = true;
            snprintf(s_gone[k].name, sizeof s_gone[k].name, "%s", d->cfg.name);
            s_gone[k].p = d->p;
            s_gone[k].at = 0;
            device_hold(d->p, 1);
            return;
        }
}

void devmqtt_init(void) { memset(s_d, 0, sizeof s_d); memset(s_gone, 0, sizeof s_gone); device_on_gone(gone); }

void devmqtt_restart(void)
{
    for (int i = 0; i < DEVS; i++) { s_d[i].disc = 0; s_d[i].status = false; s_d[i].seq = 0; s_d[i].at = 0; }
}

#define ADD(...) do { int w_ = snprintf(b + n, max - n, __VA_ARGS__); if (w_ < 0 || (size_t)w_ >= max - n) return 0; n += (size_t)w_; } while (0)

static size_t config(char *b, size_t max, const device_t *d, int fi, const char *base, const char *host)
{
    const profile_t *p = d->p;
    const pf_field_t *f = &p->field[fi];
    const char *id = pf_str(p, f->id);
    char nm[96], dn[80], mk[64], md[96], un[40], ent[80], own[40];
    size_t n = 0;
    bool bin = f->kind == PF_BINARY || f->kind == PF_CALCBIN;
    if (!json_str(pf_str(p, f->name), nm, sizeof nm) || !json_str(d->cfg.label[0] ? d->cfg.label : d->cfg.name, dn, sizeof dn) ||
        !json_str(pf_str(p, p->maker), mk, sizeof mk) || !json_str(pf_str(p, p->name), md, sizeof md) ||
        !json_str(pf_str(p, f->unit), un, sizeof un)) return 0;
    if (d->cfg.ids) object(ent, sizeof ent, d->cfg.prefix, pf_str(p, f->name));
    else { snprintf(own, sizeof own, "n540_%s", d->cfg.name); object(ent, sizeof ent, own, id); }
    ADD("{\"~\":\"%s/dev/%s\",\"name\":%s,\"unique_id\":\"%s_%s_%s\",\"default_entity_id\":\"%s.%s\","
        "\"availability\":[{\"topic\":\"%s/status\"},{\"topic\":\"~/status\"},"
        "{\"topic\":\"~/state\",\"value_template\":\"{{ 'online' if value_json.%s is not none else 'offline' }}\"}],"
        "\"availability_mode\":\"all\",\"state_topic\":\"~/state\"",
        base, d->cfg.name, nm, host, d->cfg.name, id, comp(f), ent, base, id);
    if (bin) ADD(",\"value_template\":\"{{ 'ON' if value_json.%s else 'OFF' }}\"", id);
    else ADD(",\"value_template\":\"{{ value_json.%s }}\"", id);
    if (f->unit) ADD(",\"unit_of_measurement\":%s", un);
    if (f->dc) ADD(",\"device_class\":\"%s\"", pf_str(p, f->dc));
    if (f->sc) ADD(",\"state_class\":\"%s\"", pf_str(p, f->sc));
    if (f->kind == PF_SENSOR || f->kind == PF_CALC) ADD(",\"suggested_display_precision\":%d", pf_prec(f));
    if (f->flags & PFF_DIAG) ADD(",\"entity_category\":\"diagnostic\"");
    ADD(",\"device\":{\"identifiers\":[\"%s_%s\"],\"name\":%s,\"manufacturer\":%s,\"model\":%s,\"via_device\":\"%s\"}}",
        host, d->cfg.name, dn, mk, md, host);
    return n;
}

static size_t state(char *b, size_t max, const device_t *d)
{
    size_t n = 0;
    ADD("{");
    for (int f = 0; f < d->p->nfield; f++) {
        char v[200];
        if (!device_value(d, f, v, sizeof v)) return 0;
        ADD("%s\"%s\":%s", f ? "," : "", pf_str(d->p, d->p->field[f].id), v);
    }
    ADD("}");
    return n;
}
#undef ADD

bool devmqtt_poll(devmqtt_pub_t pub, const char *base, const char *host, bool announce, uint32_t now)
{
    char topic[160];
    for (int k = 0; k < DEVS; k++) {                     /* removed devices: emptied first */
        if (!s_gone[k].used) continue;
        const profile_t *p = s_gone[k].p;
        int at = s_gone[k].at;
        if (at < p->nfield) {
            const pf_field_t *f = &p->field[at];
            snprintf(topic, sizeof topic, "homeassistant/%s/%s_%s/%s/config", comp(f), host, s_gone[k].name, pf_str(p, f->id));
        } else {
            snprintf(topic, sizeof topic, "%s/dev/%s/%s", base, s_gone[k].name, at == p->nfield ? "state" : "status");
        }
        if (!pub(topic, "", true)) return false;
        if (++s_gone[k].at > p->nfield + 1) { device_hold(p, -1); s_gone[k].used = false; }
        return true;
    }
    for (int i = 0; i < DEVS; i++) {
        const device_t *d = device_get(i);
        if (!d || !d->p) continue;
        if (s_d[i].gen != d->gen) { memset(&s_d[i], 0, sizeof s_d[i]); s_d[i].gen = d->gen; }
        if (s_d[i].disc >= 0 && s_d[i].disc < d->p->nfield) {
            const pf_field_t *f = &d->p->field[s_d[i].disc];
            snprintf(topic, sizeof topic, "homeassistant/%s/%s_%s/%s/config", comp(f), host, d->cfg.name, pf_str(d->p, f->id));
            size_t n = announce ? config(s_buf, sizeof s_buf, d, s_d[i].disc, base, host) : 0;
            if (announce && !n) { s_d[i].disc++; return true; }        /* will not fit: left out */
            if (!pub(topic, announce ? s_buf : "", true)) return false;
            if (++s_d[i].disc == d->p->nfield) s_d[i].disc = -1;
            return true;
        }
        if (!s_d[i].status || s_d[i].online != d->online) {
            snprintf(topic, sizeof topic, "%s/dev/%s/status", base, d->cfg.name);
            if (!pub(topic, d->online ? "online" : "offline", true)) return false;
            s_d[i].status = true;
            s_d[i].online = d->online;
            return true;
        }
        if ((d->seq != s_d[i].seq && now - s_d[i].at >= 1000u) || now - s_d[i].at >= 60000u) {
            snprintf(topic, sizeof topic, "%s/dev/%s/state", base, d->cfg.name);
            if (!state(s_buf, sizeof s_buf, d)) { s_d[i].seq = d->seq; s_d[i].at = now; return true; }   /* too big: skipped */
            if (!pub(topic, s_buf, true)) return false;
            s_d[i].seq = d->seq;
            s_d[i].at = now;
            return true;
        }
    }
    return true;
}
```

(`s_d[i].disc` starts at 0 after `memset`; it becomes -1 once every field is announced; `devmqtt_restart()` sets it back to 0. The state's first send waits for `now - 0 >= 1000`, true from a second after start.)

- [ ] **Step 5: Wire it into `mqtt_ha.c` and `main.c`** — `mqtt_ha.c`: `#include "devmqtt.h"`; in `cb_conn()` inside `if (st == MQTT_CONNECT_ACCEPTED) {` add `devmqtt_restart();`; in `mqtt_ha_configure()` inside `if (strcmp(base, s_base)) {` add `devmqtt_restart();` and inside the `if (strcmp(s_name, ...) || ...)` block add `devmqtt_restart();`; at the end of `mqtt_ha_poll()`, after the subscribe block:

```c
    if (!devmqtt_poll(pub, s_base, g_hostname, !s_run.no_announce, HAL_GetTick())) return;
```

`main.c`: `#include "devmqtt.h"`; after `device_init();` add `devmqtt_init();`.

- [ ] **Step 6: Run tests and build**

Run: `bash tests/host.sh 2>&1 | grep -v ': ok'` and `cmake --build build 2>&1 | grep -E '\berror\b'`
Expected: `devmqtt: ok`; firmware builds.

- [ ] **Step 7: Commit**

```bash
git add src/app/devmqtt.h src/app/devmqtt.c tests/test_devmqtt.c tests/host.sh src/app/mqtt_ha.c src/app/main.c
git commit -m "Devices over MQTT, with Home Assistant discovery

devmqtt.c publishes each device's state (one JSON object), status and a
discovery config per field (HA device via the gateway; own entity IDs,
or the ESP32's with devN.ids=esphome); a removed or changed device's
topics are emptied. One publish a main-loop pass.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK"
```

### Task 9: `/api/devices`, settings in parts, metrics, the Devices page

**Files:**
- Modify: `src/app/api.h`, `src/app/api.c`, `src/http/http.c`, `src/app/metrics.h`, `src/app/metrics.c`, `web/index.html`, `tools/n540.py`, `tests/sim/sim.c`, `tests/fuzz/http.c`
- Test: `tests/test_metrics.c`, `tests/test_metrics_http.py`

**Interfaces:**
- Consumes: `device_get`, `device_json`, `device_line_use` (Task 6); `cfg_dev_lines` (Task 5); `profiles_id` (Task 4); `do_parts` (`http.c`).
- Produces: `int api_devices(uint32_t part, char *out, size_t max)`; `int api_settings(uint32_t part, char *out, size_t max)`; `GET /api/devices` → `{"profiles":[...],"line_use":[4 permille],"devices":[device_json...]}`; `GET /api/settings` unchanged in content (`{"text":"..."}`), now with every device's lines, sent in parts; metrics families `n540_device_up`, `n540_device_polls_total`, `n540_device_misses_total` (label `device`), `n540_port_line_use_ratio` (label `port`).

- [ ] **Step 1: Metrics, test first** — `src/app/metrics.h`, in `metrics_t` after `port[PORTS]`:

```c
    struct { const char *name; bool used, online; uint32_t polls, misses; } dev[DEVS];
    uint16_t line_use[PORTS];               /* permille */
```

`tests/test_metrics.c` `widest()`: fill `m->dev[i]` for all 16 with `name` 24 characters (`"abcdefghijklmnopqrstuvwx"`), `used = online = true`, `polls = misses = UINT32_MAX`; `m->line_use[i] = 1000`. Add assertions after the Modbus case:

```c
    assert(strstr(all, "n540_device_up{device=\"abcdefghijklmnopqrstuvwx\"} 1\n"));
    assert(strstr(all, "n540_port_line_use_ratio{port=\"4\"} 1.000\n"));
```

Run `bash tests/host.sh | grep metrics` — Expected: FAIL (the families are missing).

- [ ] **Step 2: Implement the families** — in `metrics.c`'s `default:` branch, before `uint32_t k = part - 22;`:

```c
        if (part >= 32) {
            static const char *const DN[3] = { "n540_device_up", "n540_device_polls_total", "n540_device_misses_total" };
            static const char *const DH[3] = { "1 while a device answers (one of its data blocks has values).",
                                               "Reads sent to a device.", "Reads a device did not answer, answered with an error, or whose answer was refused." };
            if (part == 35) {
                head(&w, "n540_port_line_use_ratio", "gauge", "Share of the last minute the line was busy with the gateway's own reads.");
                for (int i = 0; i < PORTS; i++) {
                    put(&w, "n540_port_line_use_ratio{port=\"%d\"} %u.%03u\n", i + 1, m->line_use[i] / 1000u, m->line_use[i] % 1000u);
                }
                break;
            }
            if (part > 35) return -1;
            bool any = false;
            for (int i = 0; i < DEVS; i++) any |= m->dev[i].used;
            if (!any) break;
            head(&w, DN[part - 32], part == 32 ? "gauge" : "counter", DH[part - 32]);
            for (int i = 0; i < DEVS; i++) {
                if (!m->dev[i].used) continue;
                uint32_t v = part == 32 ? m->dev[i].online : part == 33 ? m->dev[i].polls : m->dev[i].misses;
                put(&w, "%s{device=\"", DN[part - 32]);
                label(&w, m->dev[i].name);
                put(&w, "\"} %lu\n", (unsigned long)v);
            }
            break;
        }
```

(Device names are `a-z 0-9 _ -`, but `label()` escapes anyway.) In `api_metrics()` (api.c), fill them:

```c
    for (int i = 0; i < DEVS; i++) {
        const device_t *dv = device_get(i);
        if (!dv) continue;
        m.dev[i] = (typeof(m.dev[i])){ dv->cfg.name, true, dv->online, dv->polls, dv->misses };
    }
    for (int i = 0; i < PORTS; i++) m.line_use[i] = device_line_use(i);
```

Run `bash tests/host.sh | grep metrics` — Expected: `metrics: ...` passes.

- [ ] **Step 3: `/api/devices` and `/api/settings` in parts** — `api.h`:

```c
/* GET /api/devices and /api/settings, part by part (http.c's do_parts). */
int  api_devices(uint32_t part, char *out, size_t max);
int  api_settings(uint32_t part, char *out, size_t max);
```

`api.c` — replace the `/api/settings` branch of `get_one()` (it moves to `api_settings`), and add:

```c
int api_devices(uint32_t part, char *out, size_t max)
{
    size_t n = 0;
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); if (w_ < 0 || (size_t)w_ >= max - n) return -2; n += (size_t)w_; } while (0)
    if (part == 0) {
        OUT("{\"profiles\":[");
        for (int i = 0; profiles_id(i); i++) OUT("%s\"%s\"", i ? "," : "", profiles_id(i));
        OUT("],\"line_use\":[%u,%u,%u,%u],\"devices\":[", device_line_use(0), device_line_use(1), device_line_use(2), device_line_use(3));
        return (int)n;
    }
    if (part <= DEVS) {
        int i = (int)part - 1;
        if (!device_get(i)) return 0;
        bool first = true;
        for (int k = 0; k < i; k++) first = first && !device_get(k);
        if (!first) OUT(",");
        size_t w = device_json(i, out + n, max - n);
        return w ? (int)(n + w) : -2;
    }
    if (part == DEVS + 1) { OUT("]}\n"); return (int)n; }
    return -1;
#undef OUT
}

/* JSON's escapes for s, without the quotes. 0 if it does not fit. */
static size_t escaped(const char *s, char *out, size_t max)
{
    size_t w = json_str(s, out, max);
    if (w < 2) return 0;
    memmove(out, out + 1, w - 2);
    out[w - 2] = 0;
    return w - 2;
}

int api_settings(uint32_t part, char *out, size_t max)
{
    static char text[3200];
    static cfg_t c;                                     /* as it was at part 0 */
    size_t w;
    if (part == 0) {
        char iso[17] = "", when[24] = "";
        c = s_cfg;
        if (s_pending) c.net = s_trial;
        if (clock_iso(iso, false))
            snprintf(when, sizeof when, "%.4s-%.2s-%.2s %.2s:%.2s UTC", iso, iso + 4, iso + 6, iso + 9, iso + 11);
        int h = snprintf(text, sizeof text, "# Settings of %s (%s), firmware %s, %s\n"
                         "# Restore them on its Hardware page. Passwords and keys are not here.\n",
                         c.device_name, g_hostname, g_fw_id.version, when[0] ? when : "time unknown");
        if (h <= 0 || (size_t)h >= sizeof text || !cfg_lines(&c, text + h, sizeof text - (size_t)h) || max < 9) return -2;
        memcpy(out, "{\"text\":\"", 9);
        w = escaped(text, out + 9, max - 9);
        return w ? (int)(9 + w) : -2;
    }
    if (part <= DEVS) {
        if (!cfg_dev_lines(&c, (int)part - 1, text, sizeof text)) return -2;
        w = escaped(text, out, max);
        return w ? (int)w : -2;
    }
    if (part == DEVS + 1) {
        if (max < 4) return -2;
        memcpy(out, "\"}\n", 4);
        return 3;
    }
    return -1;
}
```

`http.c`, in the `GET` branch beside `/metrics`:

```c
        if (!strcmp(path, "/api/devices"))
            return do_parts(c, end && !strncmp(end + 1, "HTTP/1.1", 8), "application/json", api_devices);
        if (!strcmp(path, "/api/settings"))
            return do_parts(c, end && !strncmp(end + 1, "HTTP/1.1", 8), "application/json", api_settings);
```

(These come before the generic `/api/` handling, so `get_one()` no longer sees `/api/settings`.) In `tests/sim/sim.c` and `tests/fuzz/http.c`, add stubs returning a few small parts:

```c
int api_devices(uint32_t part, char *o, size_t n) { return part == 0 ? snprintf(o, n, "{\"profiles\":[],\"line_use\":[0,0,0,0],\"devices\":[") : part == 1 ? snprintf(o, n, "]}\n") : -1; }
int api_settings(uint32_t part, char *o, size_t n) { return part == 0 ? snprintf(o, n, "{\"text\":\"device.name=sim\\n") : part == 1 ? snprintf(o, n, "\"}\n") : -1; }
```

In `tests/test_metrics_http.py` add, after the HTTP/1.1 check:

```python
c = http.client.HTTPConnection(host, port, timeout=10)
for path in ("/api/devices", "/api/settings"):
    c.request("GET", path); r = c.getresponse(); j = json.loads(r.read())
    assert r.status == 200 and r.getheader("Transfer-Encoding") == "chunked", path
print("ok  /api/devices and /api/settings in parts")
```

(with `import json` at the top).

- [ ] **Step 4: Loading settings in batches** — a settings file with devices can exceed a signed request's 4608 bytes. `tools/n540.py`: `save` reads `GET /api/settings`'s `text` (it has the devices; the console's `get` does not) and `load` posts in batches:

```python
    elif cmd == "save":
        text = json.loads(d.get("/api/settings"))["text"]
        with open(a[2], "w") as f:
            f.write(text)
        print(f"{len(text.splitlines())} settings saved to {a[2]}")
    elif cmd == "load":
        lines = [l for l in open(a[2]).read().splitlines() if l.strip() and not l.startswith("#")]
        lines.sort(key=lambda l: l.startswith("dev"))           # ports before the devices that need them
        batch, pending = [], False
        for l in lines + [None]:
            if l is None or (batch and len("\n".join(batch + [l])) > 4000):
                r = json.loads(d.auth("POST", "/api/config", "\n".join(batch).encode()))
                pending = pending or r["net_pending"]
                batch = []
            if l is not None:
                batch.append(l)
        print("network settings are on trial: run keep within 2 minutes or they revert" if pending else "loaded")
```

The page's restore (`$("restore").onsubmit` in `web/index.html`) does the same: replace `const r=JSON.parse(await post("/api/config",t));` with

```js
const L=t.split("\n").sort((a,b)=>(a.startsWith("dev")?1:0)-(b.startsWith("dev")?1:0));let r={net_pending:false},B=[];
 for(const l of [...L,null]){if(l===null||(B.length&&(B.join("\n")+"\n"+l).length>4000)){const x=JSON.parse(await post("/api/config",B.join("\n")));r.net_pending=r.net_pending||x.net_pending;B=[]}if(l!==null)B.push(l)}
```

and raise its file-size limit from 8000 to 16000.

- [ ] **Step 5: The Devices page** — `web/index.html`. After the `p-events` div add:

```html
 <div id="p-devices" hidden><div class="top"><a class="back" href="#status">All stations</a><div class="head"><h2>Devices</h2><button id="devadd" class="primary" type="button" data-auth="">Add device</button></div><p class="say" id="dsay"></p></div>
  <table class="stack" id="devrows"></table>
  <section id="devone" hidden><div class="head"><h3 id="devname"></h3><button id="devedit" type="button" data-auth="">Edit</button><button id="devdel" class="danger" type="button" data-auth="">Remove</button></div><p class="hint" id="devwhy"></p><table class="stack" id="devvals"></table></section>
  <section id="devform" hidden></section>
 </div>
```

In the script: add `devices:"devices"` to `PAGES`; add `"devices"` to `show()`'s page list and call `devices()` at its end beside `events()`; in `buildList()` add `add("#devices","Devices","config");` after the Events one (it becomes item 9); in `paint()` add

```js
 setItem(9,!devs?"idle":devs.devices.some(d=>d.error||!d.online)?"warn":"ok","Devices",!devs?"Reading":!devs.devices.length?"None":devs.devices.filter(d=>d.online).length+" of "+devs.devices.length+" online");
```

and in `tick()` call `devices()` where `events()` is called. Then:

```js
/* ---- devices ---- */
let devs=null,devAt=0,devSel=0,devEd=null;
async function devices(force){const now=performance.now();if(!force&&devAt&&now-devAt<(page==="devices"?3000:30000))return;devAt=now;
 try{devs=await getj("/api/devices")}catch(e){devAt=0;return}drawDevices()}
const devVal=(v,u)=>v===null?"Unknown":v===true?"On":v===false?"Off":typeof v==="number"?nb(v.toLocaleString(undefined,{maximumFractionDigits:3})+(u?" "+u:"")):v;
function drawDevices(){if(!devs||page!=="devices")return;const D=devs.devices,on=D.filter(d=>d.online).length;
 const use=[...new Set(D.map(d=>d.port))].sort().map(p=>"port "+p+" "+Math.round(devs.line_use[p-1]/10)+"%").join(", ");
 put($("dsay"),D.length?plural(D.length,"device")+", "+on+" online. Line use: "+use+".":"No devices yet. Add one on a port set to Modbus gateway.");
 fillRows($("devrows"),["Device","Model","Where","State","Last read"],D.map(d=>[el("a",{href:"#devices",class:"name",onclick(e){e.preventDefault();devSel=d.n;devEd=null;drawDevices()}},d.label||d.name),
  d.model?d.maker+" "+d.model:d.profile,"Port "+d.port+", address "+d.address,
  d.error?el("span",{class:"bad"},d.error):d.online?el("span",{class:"ok"},"Online"):el("span",{class:"warn"},"Offline"),d.ago_ms<0?"Never":ago(d.ago_ms)]));
 const d=D.find(x=>x.n===devSel);$("devone").hidden=!d||!!devEd;
 if(d&&!devEd){put($("devname"),d.label||d.name);put($("devwhy"),plural(d.polls,"read")+", "+plural(d.misses,"miss")+(d.why?". Last: "+d.why:"")+".");
  fillRows($("devvals"),["Value","Now"],(d.values||[]).map(v=>[v[1],devVal(v[2],v[3])]))}
 $("devform").hidden=!devEd;if(devEd)$("devform").replaceChildren(devForm(devEd));authUI()}
function devForm(d){const mb=cfg.ports.map((p,i)=>[i+1,p]).filter(([,p])=>p.mode==="modbus");
 const f=el("form",{},el("h3",{},d.n?"Edit "+(d.label||d.name):"Add a device"),
  el("div",{class:"grid-f"},
   el("label",{},"Profile",el("select",{name:"profile"},...devs.profiles.map(p=>el("option",{value:p,selected:p===d.profile},p)))),
   el("label",{},"Name",el("input",{name:"name",value:d.name||"",maxLength:24,required:true,pattern:"[a-z0-9_\\-]+",title:"a-z, 0-9, _ and -"})),
   el("label",{},"Label in Home Assistant",el("input",{name:"label",value:d.label||"",maxLength:32})),
   el("label",{},"Port",el("select",{name:"port"},...mb.map(([n,p])=>el("option",{value:n,selected:n===d.port},n+" "+(p.name||"Port "+n))))),
   el("label",{},"Address",el("input",{name:"address",type:"number",min:1,max:247,value:d.address||1,required:true})),
   el("label",{},"Home Assistant IDs",el("select",{name:"ids"},el("option",{value:"own",selected:d.ids!=="esphome"},"The gateway's own"),el("option",{value:"esphome",selected:d.ids==="esphome"},"The ESP32's (ESPHome)"))),
   el("label",{},"ESPHome prefix",el("input",{name:"prefix",value:d.prefix||"",maxLength:24,pattern:"[a-z0-9_]*"}))),
  el("p",{class:"hint"},mb.length?"ESPHome IDs only once the ESP32 is removed from Home Assistant: two cannot hold the same ID.":"First set a port to Modbus gateway (its local port may be 0)."),
  el("div",{class:"acts"},el("button",{class:"primary","data-auth":""},"Save device"),el("button",{type:"button",onclick(){devEd=null;drawDevices()}},"Cancel"),el("p",{class:"msg",role:"status"})));
 f.onsubmit=async ev=>{ev.preventDefault();const e=f.elements,m=f.querySelector(".msg");
  const n=d.n||[...Array(16).keys()].map(k=>k+1).find(k=>!devs.devices.some(x=>x.n===k));if(!n)return say(m,"All 16 places are in use.","bad");
  const L=["profile","name","label","port","address","ids","prefix"].map(k=>"dev"+n+"."+k+"="+e[k].value.trim());
  say(m,"Saving…");try{await post("/api/config",L.join("\n"));devEd=null;devSel=n;await devices(true)}catch(x){say(m,x.message,"bad")}};
 return f}
$("devadd").onclick=()=>{devEd={};drawDevices()};
$("devedit").onclick=()=>{devEd=devs.devices.find(x=>x.n===devSel)||null;drawDevices()};
$("devdel").onclick=async()=>{const d=devs.devices.find(x=>x.n===devSel);if(!d)return;
 try{await post("/api/config","dev"+d.n+".profile=");devSel=0;await devices(true)}catch(e){say($("devwhy"),e.message,"bad")}};
```

Check it in a browser against the simulator stub and, with Task 10's real device, the gateway: the list, a device's values, add, edit, remove; dark and light; at 390 px.

- [ ] **Step 6: Run everything**

Run: `bash tests/host.sh 2>&1 | grep -v ': ok'`; `CC=clang FUZZ_OUT=/tmp/fz tests/fuzz/run.sh 60 http profile`; the simulator with `tests/test_metrics_http.py` (start `tests/sim/run.sh`, wait for `/status`, run the script); `cmake --build build`.
Expected: all pass; fuzzers `0 failures`.

- [ ] **Step 7: Commit**

```bash
git add src/app/api.h src/app/api.c src/http/http.c src/app/metrics.h src/app/metrics.c tests/test_metrics.c tests/test_metrics_http.py tests/sim/sim.c tests/fuzz/http.c tools/n540.py web/index.html
git commit -m "Devices: /api/devices, the Devices page, metrics; settings in parts

GET /api/devices (profiles, line use, each device's settings, state and
values) and /api/settings (now with devN.* lines) are sent part by part.
The Devices page lists, adds, edits and removes devices and shows their
values. Settings files load in batches under 4000 bytes, ports first.
/metrics: n540_device_up, _polls_total, _misses_total, and each port's
line use.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK"
```

### Task 10: Docs, CI, and the gateway

**Files:**
- Modify: `docs/GATEWAY.md` (a "Devices" section; storage line for `0x0EE000`/`0x0EF000`), `docs/ROADMAP.md` (the item), `.github/workflows/tests.yml` (fuzz `profile` target; nothing else: host tests and the simulator step already run the new tests)

- [ ] **Step 1: `docs/GATEWAY.md`** — a "## Devices" section after "Modbus TCP gateway": what a device is (a profile, a port in Modbus gateway mode, an address); the settings table (`devN.profile|name|label|port|address|controls|ids|prefix`); topics (`BASE/dev/<name>/state|status`, discovery `homeassistant/<component>/HOST_<name>/<field>/config`); HA naming and the ESP32 swap (remove the ESPHome device first; `ids=esphome`, `prefix=eg4_bms`); timing (`turnaround`, `timeout`, `stale`, line use on the page and in `/metrics`); phase-1 limits (reading only; built-in profiles only; a base-topic change leaves devices' old retained topics). In "## Storage": "Devices have their own two copies at `0x0EE000` / `0x0EF000`."

- [ ] **Step 2: Push the branch and wait for CI**

```bash
git diff main --stat; git diff main | grep -cE 'AKIA[A-Z0-9]{16}'   # expect 0
git push -u origin devices
gh run watch $(gh run list --branch devices --limit 1 --json databaseId --jq '.[0].databaseId') --exit-status
```

Expected: both jobs pass.

- [ ] **Step 3: Install on the gateway and check it with no device wired** — follow the Global Constraints' install steps. Then: set port 2 to a poll-only Modbus port and add a device on it (nothing is wired, so it stays offline):

```bash
python3 tools/n540.py 10.20.1.210 set port2.enabled=1 port2.mode=modbus port2.local_port=0 \
  dev1.profile=eg4-bms dev1.name=eg4test dev1.port=2 dev1.address=16
curl -s http://10.20.1.210/api/devices | python3 -m json.tool | head -40
```

Expected: the device listed, `"online": false`, `misses` growing, `why` "No reply (…)", `line_use[1]` above 0; port 1 untouched (`serial_tx` 0, both TigoBus clients); `/metrics` has `n540_device_up{device="eg4test"} 0`. Ask the user to check Home Assistant: a device "eg4test" via the gateway, its entities unavailable. Then remove it and put port 2 back:

```bash
python3 tools/n540.py 10.20.1.210 set dev1.profile= port2.enabled=0 port2.mode=server port2.local_port=4002
```

Expected: HA's eg4test device's entities disappear (configs emptied).

- [ ] **Step 4: When the EG4 is wired** (not part of this plan's completion; tell the user): beside its ESP32 with own IDs, compare values for a few minutes; then remove the ESPHome device in HA, set `devN.ids=esphome` and `devN.prefix=eg4_bms`, and check history continues.

- [ ] **Step 5: Merge** — fast-forward `main` to the branch, push, and record in the project memory: device and main versions, what phase 1 delivered, what phase 2 holds.

