# Device Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Home Assistant can change a device's settings over MQTT, starting with the two EG4
FlexBOSS inverters, which SolarAssistant keeps reading through the same ports.

**Architecture:** A Modbus gateway port learns RTU frames over TCP and an answering unit 0, so
SolarAssistant and the gateway's own polls share one queue. Profiles gain controls: fields read
from a hold block that can also be written. A pure module turns a command into the bits to write.
The engine queues writes ahead of polls: read the register, write only if it changes, read the
block back, and log each outcome. MQTT carries commands in and announces controls to HA.

**Tech Stack:** C (bare-metal STM32H750, lwIP raw API, newlib nano: no printf floats), host tests
with ASan/UBSan (`tests/host.sh`), libFuzzer-style targets (`tests/fuzz/run.sh`), vanilla JS page.

**Spec:** docs/superpowers/specs/2026-11-27-device-controls-design.md (it extends
docs/superpowers/specs/2026-11-26-device-profiles-design.md).

## Global Constraints

- The loader stays byte-identical: `cmp -l build/loader_flash.bin <baseline>` may differ only in
  bytes 0x2a8 to 0x2bf (the version text).
- No gateway install, no change to ports 2 or 3, no restart of the Pi's `n540-tty@` units unless
  the user is present and says go. Port 1 never transmits; after any install check port 1's
  `serial_tx` is 0 and both TigoBus clients are back.
- Nothing private in commits: no real LAN addresses (docs use 10.20.x or 10.0.0.x), no host
  names, no serial numbers, no home-directory paths. Commit trailers as the session gives them.
- The gateway's printf has no floating point and no `%ll`: numbers go out through `pf_num()`.
- `tests/host.sh` passes after every task; `tests/fuzz/run.sh 20` passes for any target a task
  touches.
- Event notes keep 39 characters (`events.h`).

## File map

| File | Change |
|---|---|
| `src/app/modbus.h`, `modbus.c` | RTU client framing (`rtu`, `modbus_set_rtu`), unit 0 `MB_BC_ANSWER` |
| `src/app/cfg.h`, `cfg.c` | `modbus_framing`, `modbus_broadcast=answer`; helpers |
| `src/app/bridge.c` | applies them to each port's queue |
| `src/app/api.c` | settings text buffer 3456; `/api/devices` in parts |
| `web/index.html` | port form selects; EV_WRITE text |
| `src/app/profile.h`, `profile.c` | control kinds and statements |
| `src/app/pfdecode.c` | decoding controls |
| `src/app/pfwrite.h`, `pfwrite.c` (new) | a command into a write |
| `src/app/device.h`, `device.c` | write queue, `device_command`, `device_json_part` |
| `src/app/events.h` | `EV_WRITE` |
| `src/app/devmqtt.c`, `src/app/mqtt_ha.c`, `src/net/lwipopts.h` | commands, discovery, buffers |
| `profiles/eg4-18kpv.txt` -> `profiles/eg4-hybrid.txt` | settings blocks, 41 controls |
| tests: `test_modbus.c`, `test_bridge.c`, `test_cfg.c`, `test_profile.c`, `test_pfdecode.c`, `test_pfwrite.c` (new), `test_devctl.c` (new), `test_devmqtt.c`, `test_builtin.c`, `tests/stub/events_stub.c` (new), `tests/fuzz/modbus.c`, `tests/fuzz/command.c` (new), `tests/fuzz/run.sh`, `tests/host.sh` | |
| docs: `docs/GATEWAY.md`, `profiles/README.md`, `README.md`, `proxy/README.md` | |

---

### Task 1: RTU frames over TCP and an answering unit 0, in modbus.c

**Files:**
- Modify: `src/app/modbus.h`, `src/app/modbus.c`
- Test: `tests/test_modbus.c`, `tests/fuzz/modbus.c`

**Interfaces:**
- Produces: `enum { MB_BC_OFF, MB_BC_ALL, MB_BC_ANSWER };` `modbus_t.broadcast` is `uint8_t`
  holding one of them; `modbus_t.rtu` (bool); `void modbus_set_rtu(modbus_t *m, bool rtu);`

- [ ] **Step 1: Write the failing test.** In `tests/test_modbus.c`, just before the final
  `printf("modbus: ok, ...")`, add:

```c
    /* RTU frames over TCP (SolarAssistant through socat): each taken when its function says it
     * is whole, in any pieces; answered as RTU frames; unit 0 answered when the port says so. */
    modbus_init(&M, &IO, 1000, GAP);
    modbus_set_rtu(&M, true);
    modbus_open(&M, 0, true);
    modbus_open(&M, 1, false);
    assert(ask(0, "01 03 00"));                                 /* a piece */
    poll(GAP); assert(Ln == 0 && M.qn == 0);
    assert(ask(0, "00 00 01 84 0a"));
    poll(GAP); assert(on_line("01 03 00 00 00 01 84 0a"));
    poll(GAP); hear("01 03 02 00 2a 39 9b");
    assert(to(0, "01 03 02 00 2a 39 9b"));                      /* as heard, CRC and all */
    assert(ask(0, "01 10 00 01 00 02 04 00 0a 01 02 92 30"));   /* 16: nine bytes and the count */
    poll(GAP); assert(on_line("01 10 00 01 00 02 04 00 0a 01 02 92 30"));
    poll(GAP); hear("01 10 00 01 00 02 10 08");
    assert(to(0, "01 10 00 01 00 02 10 08"));
    uint32_t ref = M.s.refused;                                 /* a bad CRC: dropped, nothing sent */
    assert(ask(0, "01 03 00 00 00 01 84 0b"));
    poll(GAP); assert(Ln == 0 && M.qn == 0 && An[0] == 0 && M.s.refused == ref + 1);
    /* A function whose length cannot be told: exception 1, and the rest of that piece dropped. */
    assert(ask(0, "01 2b 0e 01 00 70 77 01 03 00 00 00 01 84 0a"));
    assert(to(0, "01 ab 01 9e f0") && M.qn == 0 && M.s.refused == ref + 2);
    assert(ask(1, "01 06 00 01 00 03 98 0b"));                  /* a reader may not write */
    assert(to(1, "01 86 01 83 a0") && M.qn == 0);
    assert(ask(0, "01 03 00 00 00 01 84 0a"));                  /* no reply in time */
    poll(GAP); Ln = 0; poll(GAP);
    now += 1000; poll(GAP);
    assert(to(0, "01 83 0b 00 f7"));
    M.broadcast = MB_BC_ANSWER;                                 /* unit 0 answers, as EG4 inverters do */
    assert(ask(0, "00 03 00 00 00 01 85 db"));
    poll(GAP); assert(on_line("00 03 00 00 00 01 85 db"));
    poll(GAP); now += 30; hear("00 03 02 00 2a 04 5b");
    assert(to(0, "00 03 02 00 2a 04 5b") && M.s.broadcasts == 0);
    assert(ask(0, "01 03 00"));                                 /* back to Modbus TCP: the piece is forgotten */
    modbus_set_rtu(&M, false);
    assert(ask(0, "00 60 00 00 00 06 01 03 00 00 00 01"));
    poll(GAP); assert(on_line("01 03 00 00 00 01 84 0a"));
```

- [ ] **Step 2: Run it to see it fail.** Run: `tests/host.sh 2>&1 | grep -A3 '^modbus'`.
  Expected: `modbus: BUILD FAILED` (`modbus_set_rtu`, `MB_BC_ANSWER` undeclared).

- [ ] **Step 3: Implement.** In `src/app/modbus.h`, after the `#define MB_LOCAL` line add
  `enum { MB_BC_OFF, MB_BC_ALL, MB_BC_ANSWER };   /* unit 0: refused; sent to every device, unanswered; a device that answers */`.
  In `modbus_t` replace `bool broadcast; ...` with:

```c
    uint8_t  broadcast;         /* MB_BC_*: what unit 0 is (bridge sets it) */
    bool     rtu;               /* clients send RTU frames and are answered in them (modbus_set_rtu) */
```

  Change the `refused` comment in `mb_stats_t` to end "... 248 to 255; or an RTU frame from a
  client that will not do (bad CRC, unknown length) */". Change the comment over
  `modbus_from_client` to "Bytes from a client, in any pieces: Modbus TCP, or RTU frames when
  m->rtu. False: not Modbus TCP; close the connection (never for RTU)." and add after
  `modbus_may_write`:

```c
/* Clients' framing: false Modbus TCP, true RTU frames over TCP. A change forgets what each
 * client has half sent. */
void modbus_set_rtu(modbus_t *m, bool rtu);
```

  In `src/app/modbus.c` replace `exception()` and `take()` and add the RTU reader:

```c
void modbus_set_rtu(modbus_t *m, bool rtu)
{
    if (m->rtu == rtu) return;
    m->rtu = rtu;
    for (int c = 0; c < MB_CLIENTS; c++) m->cl[c].n = 0;
}

/* An answer of our own: the request's header, the function with 0x80, the code. For an RTU
 * client: unit, function | 0x80, code, CRC. */
static void exception(modbus_t *m, int c, uint16_t tid, uint8_t unit, uint8_t fn, uint8_t code)
{
    if (c < 0 || c >= MB_CLIENTS || !m->cl[c].open) return;
    if (m->rtu) {
        uint8_t r[5] = { unit, (uint8_t)(fn | 0x80), code, 0, 0 };
        uint16_t crc = modbus_crc(r, 3);
        r[3] = (uint8_t)crc;
        r[4] = (uint8_t)(crc >> 8);
        m->io.reply(m->io.ctx, c, r, sizeof r);
        return;
    }
    uint8_t r[9] = { (uint8_t)(tid >> 8), (uint8_t)tid, 0, 0, 0, 3, unit, (uint8_t)(fn | 0x80), code };
    m->io.reply(m->io.ctx, c, r, sizeof r);
}
```

```c
static void take(modbus_t *m, int c, uint16_t tid, uint8_t unit, const uint8_t *pdu, uint16_t plen)
{
    m->s.requests++;
    if (unit == 0 ? !m->broadcast : unit > 247) {     /* a client left at its default must not reach them all */
        m->s.refused++;
        exception(m, c, tid, unit, pdu[0], EX_NO_PATH);
        return;
    }
    if (!m->cl[c].may_write && !may_read_only(pdu, plen)) {
        m->s.refused++;
        exception(m, c, tid, unit, pdu[0], EX_ILLEGAL_FUNCTION);
        return;
    }
    if (m->qn == MB_QUEUE) {
        m->s.busy++;
        exception(m, c, tid, unit, pdu[0], EX_BUSY);
        return;
    }
    mb_req_t *r = &m->q[m->qn++];
    r->client = (int8_t)c;
    r->unit = unit;
    r->tid = tid;
    r->len = plen;
    r->timeout_ms = 0;
    r->tag = 0;
    memcpy(r->pdu, pdu, plen);
}

/* An RTU frame's length from its first bytes: 0 while too few are here to tell, -1 for a
 * function whose length cannot be told. */
static int rtu_len(const uint8_t *b, uint16_t have)
{
    if (have < 2) return 0;
    if (b[1] >= 1 && b[1] <= 6) return 8;
    if (b[1] == 15 || b[1] == 16) return have < 7 ? 0 : 9 + b[6];
    return -1;
}

/* RTU frames from a client. A bad CRC, an unknown function or a frame too long: what it has
 * is dropped, with the rest of this piece, as the frame's end cannot be found inside it. */
static void from_rtu(modbus_t *m, int c, const uint8_t *d, size_t n)
{
    uint8_t *b = m->cl[c].buf;
    for (;;) {
        uint16_t have = m->cl[c].n;
        int want = rtu_len(b, have);
        if (want < 0 || want > 3 + MB_PDU) {
            m->s.refused++;
            if (want < 0) exception(m, c, 0, b[0], b[1], EX_ILLEGAL_FUNCTION);
            m->cl[c].n = 0;
            return;
        }
        if (want && have == want) {
            m->cl[c].n = 0;
            if (modbus_crc(b, have - 2u) != (uint16_t)(b[have - 2] | b[have - 1] << 8)) { m->s.refused++; return; }
            take(m, c, 0, b[0], b + 1, (uint16_t)(have - 3));
            continue;
        }
        if (!n) return;
        size_t need = want ? (size_t)want : have < 2 ? 2u : 7u;
        size_t k = need - have < n ? need - have : n;
        memcpy(b + have, d, k);
        m->cl[c].n = (uint16_t)(have + k);
        d += k;
        n -= k;
    }
}
```

  In `modbus_from_client`, after `if (!m->cl[c].open) return false;` add
  `if (m->rtu) { from_rtu(m, c, d, n); return true; }`, and replace its
  `take(m, c, b, (uint16_t)(have - 6));` with
  `take(m, c, (uint16_t)(b[0] << 8 | b[1]), b[6], b + 7, (uint16_t)(have - 7));`.
  In `modbus_poll`, case `MB_SENDING`, replace `if (r->unit == 0) {` with
  `if (r->unit == 0 && m->broadcast != MB_BC_ANSWER) {`. In case `MB_WAITING`, replace
  `if (r->client >= 0 && m->cl[r->client].open) m->io.reply(m->io.ctx, r->client, a, 7u + plen);` with:

```c
                if (r->client >= 0 && m->cl[r->client].open) {
                    if (m->rtu) m->io.reply(m->io.ctx, r->client, m->rx, m->rxn);   /* as heard: its CRC is good */
                    else m->io.reply(m->io.ctx, r->client, a, 7u + plen);
                }
```

  Update the file's header comment: add "Clients may instead send RTU frames over TCP (unit,
  PDU, CRC), and are answered in them."

- [ ] **Step 4: Fuzz harness.** In `tests/fuzz/modbus.c`: after `M.broadcast = d[0] & 1;` add
  `if (d[0] & 0x80) M.broadcast = MB_BC_ANSWER;` and `modbus_set_rtu(&M, d[0] & 0x40);`. Replace
  the body of `reply()` after its first assert with:

```c
    if (M.rtu) {
        assert(n >= 5 && n <= 3 + MB_PDU);
        assert(modbus_crc(d, n - 2) == (uint16_t)(d[n - 2] | d[n - 1] << 8));
        return;
    }
    assert(n >= 9 && n <= 7 + MB_PDU);
    assert(d[2] == 0 && d[3] == 0 && (size_t)(d[4] << 8 | d[5]) == n - 6);
```

  and say in its header comment: "a reply is whole Modbus TCP, or a whole RTU frame with a good
  CRC for an RTU port".

- [ ] **Step 5: Run.** `tests/host.sh` (expect `modbus: ok, ...` and every other line ok), then
  `tests/fuzz/run.sh 20 modbus` (expect no findings).

- [ ] **Step 6: Commit.** `git add src/app/modbus.h src/app/modbus.c tests/test_modbus.c tests/fuzz/modbus.c`;
  message "Modbus: RTU frames over TCP, and a unit 0 that answers".

---

### Task 2: The port settings, the bridge, the page and the docs

**Files:**
- Modify: `src/app/cfg.h`, `src/app/cfg.c`, `src/app/bridge.c`, `src/app/api.c:531`,
  `web/index.html:349-360`, `docs/GATEWAY.md` (Ports table), `proxy/README.md`
- Test: `tests/test_cfg.c`, `tests/test_bridge.c`

**Interfaces:**
- Consumes: Task 1's `MB_BC_*`, `modbus_set_rtu()`.
- Produces: settings `portN.modbus_framing=tcp|rtu`, `portN.modbus_broadcast=0|1|answer`;
  `const char *cfg_modbus_unit0(const cfg_t *c, int port);` (`"0"`, `"1"`, `"answer"`),
  `bool cfg_modbus_rtu(const cfg_t *c, int port);`; JSON `"modbus_broadcast":"answer","modbus_framing":"rtu"`.

- [ ] **Step 1: Write the failing tests.** In `tests/test_cfg.c` replace the two JSON checks
  at lines 92-93 with:

```c
    { static char jj[4344]; assert(cfg_json(&c, jj, sizeof jj) && strstr(jj, "\"modbus_broadcast\":\"0\",\"modbus_framing\":\"tcp\",\"modbus_tcp\":false}") &&
                                   strstr(jj, "\"modbus_broadcast\":\"1\",\"modbus_framing\":\"tcp\",\"modbus_tcp\":true}"));
```

  and after line 99 (`OK("port1.modbus_tcp=1\nport3.modbus_tcp=1\nport3.modbus_broadcast=0"); ...`) add:

```c
    /* modbus_broadcast=answer and modbus_framing: bits of their own, from the spare bytes. */
    OK("port3.modbus_broadcast=answer");
    assert(c.more.modbus_answer == 4 && !(c.more.modbus_broadcast & 4) && !strcmp(cfg_modbus_unit0(&c, 2), "answer"));
    OK("port3.modbus_broadcast=1");
    assert(!c.more.modbus_answer && (c.more.modbus_broadcast & 4) && !strcmp(cfg_modbus_unit0(&c, 2), "1"));
    OK("port3.modbus_broadcast=0"); assert(!strcmp(cfg_modbus_unit0(&c, 2), "0"));
    BAD("port3.modbus_broadcast=Answer");
    assert(!cfg_modbus_rtu(&c, 1)); OK("port2.modbus_framing=rtu"); assert(c.more.modbus_rtu == 2 && cfg_modbus_rtu(&c, 1));
    BAD("port2.modbus_framing=RTU"); BAD("port2.modbus_framing=");
    OK("port4.modbus_broadcast=answer");
    { static char jj[4344]; assert(cfg_json(&c, jj, sizeof jj) && strstr(jj, "\"modbus_broadcast\":\"answer\",\"modbus_framing\":\"tcp\"") &&
                                   strstr(jj, "\"modbus_framing\":\"rtu\""));
      assert(cfg_lines(&c, jj, sizeof jj) && strstr(jj, "port2.modbus_framing=rtu\n") && strstr(jj, "port4.modbus_broadcast=answer\n"));
      static cfg_t t; cfg_defaults(&t); assert(cfg_parse(&t, jj, err, sizeof err) && t.more.modbus_rtu == 2 && t.more.modbus_answer == 8); }
    OK("port2.modbus_framing=tcp\nport4.modbus_broadcast=0");
```

  Change the settings text bound at the end from `assert(k <= 3200 - 200); }  /* api.c: text[3200], ...`
  to `assert(k <= 3456 - 200); }  /* api.c: text[3456], after a header of at most ~190 bytes */`.

  In `tests/test_bridge.c`, after `set("port2.listen_only=0\nport2.enabled=0\nport2.access=");`
  (end of the first Modbus section) add:

```c
    /* RTU frames over TCP with unit 0 answered: SolarAssistant through socat, to EG4 inverters.
     * Back to Modbus TCP without reopening the line or closing the client. */
    set("port2.enabled=1\nport2.mode=modbus\nport2.local_port=4002\nport2.modbus_framing=rtu\nport2.modbus_broadcast=answer");
    struct tcp_pcb *sa = from(4002, "10.0.0.7");
    assert(!sa->aborted);
    sayb(sa, "00 04 00 00 00 01 30 1b");
    assert(lineb(1, "00 04 00 00 00 01 30 1b"));
    heardb(1, "00 04 02 00 0c 84 f5");
    assert(outb(sa, "00 04 02 00 0c 84 f5"));
    set("port2.modbus_framing=tcp\nport2.modbus_broadcast=0");
    assert(!sa->closed);
    sayb(sa, "00 01 00 00 00 06 01 03 00 00 00 01");
    assert(lineb(1, "01 03 00 00 00 01 84 0a"));
    heardb(1, "01 03 02 00 2a 39 9b");
    assert(outb(sa, "00 01 00 00 00 05 01 03 02 00 2a"));
    set("port2.enabled=0\nport2.mode=server");
```

- [ ] **Step 2: Run to see them fail.** `tests/host.sh 2>&1 | grep -E '^(cfg|bridge)'`.
  Expected: both `BUILD FAILED` (`cfg_modbus_unit0`, `modbus_answer` undeclared).

- [ ] **Step 3: Settings.** In `src/app/cfg.h` `more_cfg_t`, after `modbus_no_tcp` add:

```c
    uint8_t modbus_rtu;            /* bit per port: TCP clients send RTU frames (modbus_framing=rtu) */
    uint8_t modbus_answer;         /* bit per port: unit 0 is a device that answers (modbus_broadcast=answer);
                                      modbus_broadcast's bit is then clear */
    uint8_t spare[146];            /* for later settings */
```

  (replacing `uint8_t spare[148];`), and after the struct
  `_Static_assert(sizeof(more_cfg_t) == 256, "more_cfg_t is stored: keep its size");`.
  After `bool cfg_modbus_tcp(...)` declare:

```c
/* What unit 0 is on this port's Modbus gateway: "0" refused, "1" every device, "answer" a device that answers. */
const char *cfg_modbus_unit0(const cfg_t *c, int port);
/* Whether this port's TCP clients send RTU frames rather than Modbus TCP. */
bool cfg_modbus_rtu(const cfg_t *c, int port);
```

  In `src/app/cfg.c`, after `cfg_modbus_tcp`:

```c
const char *cfg_modbus_unit0(const cfg_t *c, int i) { return c->more.modbus_answer >> i & 1 ? "answer" : c->more.modbus_broadcast >> i & 1 ? "1" : "0"; }
bool cfg_modbus_rtu(const cfg_t *c, int i) { return c->more.modbus_rtu >> i & 1; }
```

  Replace the `modbus_broadcast` branch of `apply()` and add `modbus_framing`:

```c
        if (!strcmp(k + 6, "modbus_broadcast")) {
            if (strcmp(v, "0") && strcmp(v, "1") && strcmp(v, "answer")) return false;
            uint8_t bit = (uint8_t)(1u << (k[4] - '1'));
            c->more.modbus_broadcast = (uint8_t)(*v == '1' ? c->more.modbus_broadcast | bit : c->more.modbus_broadcast & ~bit);
            c->more.modbus_answer = (uint8_t)(*v == 'a' ? c->more.modbus_answer | bit : c->more.modbus_answer & ~bit);
            return true;
        }
        if (!strcmp(k + 6, "modbus_framing")) {
            if (strcmp(v, "tcp") && strcmp(v, "rtu")) return false;
            uint8_t bit = (uint8_t)(1u << (k[4] - '1'));
            c->more.modbus_rtu = (uint8_t)(*v == 'r' ? c->more.modbus_rtu | bit : c->more.modbus_rtu & ~bit);
            return true;
        }
```

  In `cfg_json` replace the port tail `ADD(",\"quiet_min\":%u,...,\"modbus_broadcast\":%s,\"modbus_tcp\":%s}", ...)` with
  `"... ,\"modbus_timeout\":%u,\"modbus_broadcast\":\"%s\",\"modbus_framing\":\"%s\",\"modbus_tcp\":%s}"`
  passing `cfg_modbus_unit0(c, i), cfg_modbus_rtu(c, i) ? "rtu" : "tcp"` for the two. In `cfg_lines`
  replace `%s.modbus_broadcast=%u\n%s.modbus_tcp=%u\n` with
  `%s.modbus_broadcast=%s\n%s.modbus_framing=%s\n%s.modbus_tcp=%u\n`, passing
  `pre, cfg_modbus_unit0(c, i), pre, cfg_modbus_rtu(c, i) ? "rtu" : "tcp", pre, (unsigned)cfg_modbus_tcp(c, i)`.
  In `src/app/api.c:531` change `static char text[3200];` to `static char text[3456];`.

- [ ] **Step 4: The bridge.** In `src/app/bridge.c`, beside `s_mb_broadcast` declare
  `static uint8_t s_mb_answer, s_mb_rtu;   /* bit per port: unit 0 answers; clients send RTU frames */`
  and after `is_mb()` add
  `static uint8_t unit0(int i) { return s_mb_answer >> i & 1 ? MB_BC_ANSWER : s_mb_broadcast >> i & 1 ? MB_BC_ALL : MB_BC_OFF; }`.
  In `mb_start()` and in `bridge_poll()`'s same-wiring branch replace
  `s_mb[i].broadcast = s_mb_broadcast >> i & 1;` with
  `s_mb[i].broadcast = unit0(i); modbus_set_rtu(&s_mb[i], s_mb_rtu >> i & 1);`. In
  `bridge_configure()` add `s_mb_answer = c->more.modbus_answer; s_mb_rtu = c->more.modbus_rtu;`.

- [ ] **Step 5: Run.** `tests/host.sh`: all ok, including `cfg` and `bridge`.

- [ ] **Step 6: The page.** In `web/index.html`:
  - In the Network fieldset (line 349), after `inp("modbus_timeout","Reply timeout, ms",num(50,10000))` add
    `,sel("modbus_framing","Clients send",[["tcp","Modbus TCP"],["rtu","RTU frames over TCP"]]),sel("modbus_broadcast","Unit 0",[["0","Refused"],["1","Every device, no reply"],["answer","A device that answers"]])`.
  - Line 350: the checks div becomes `el("div",{class:"checks","data-f":"modbus_tcp",style:"margin-top:12px"},chk("modbus_tcp", ...as now...))`,
    dropping its `chk("modbus_broadcast", ...)`.
  - Line 357: replace `f.elements.modbus_broadcast.closest("label").hidden=off;` with
    `for(const n of ["modbus_broadcast","modbus_framing"])f.querySelector("[data-f="+n+"]").hidden=off||v!=="modbus";`.
  - Line 360: replace `f.querySelector("[data-f=modbus_broadcast]").hidden=v!=="modbus";` with
    `f.querySelector("[data-f=modbus_tcp]").hidden=v!=="modbus";` (keeping the modbus_timeout part).
  - Check it: `tests/sim/run.sh` and open `http://127.0.0.1:8541/`, sign in, open a port's
    settings, set Mode to Modbus gateway: the two selects show, the browser console has no
    errors, and saving with "RTU frames over TCP" sends `portN.modbus_framing=rtu`.

- [ ] **Step 7: Docs.** In `docs/GATEWAY.md`'s Ports table replace the `modbus_broadcast` row and
  add one after it:

```markdown
| modbus_broadcast | Modbus gateway: what unit 0 is. 0 (the default) refuses it; 1 sends it to every device, and none replies; `answer` passes it on and waits for the reply, as EG4 inverters answer unit 0 |
| modbus_framing | Modbus gateway: what clients send. `tcp` (the default): Modbus TCP; `rtu`: RTU frames over TCP, answered in RTU frames, as a program on a serial device sends them through [proxy/](../proxy/README.md). Firmware older than this reads such a port as Modbus TCP, and drops those clients |
```

  In `proxy/README.md` replace the "**The port must be in server mode**" bullet with:
  "**The port must be in server mode, or a Modbus gateway with `modbus_framing=rtu`.** The device
  carries RTU bytes as they are, so Modbus RTU programs work as on a USB adapter. A Modbus gateway
  also lets the gateway's own devices poll the same line, in turn with the program; set
  `modbus_broadcast=answer` if the program asks unit 0 (SolarAssistant does, of EG4 inverters).
  Allow a little longer for replies, for the network."

- [ ] **Step 8: Commit.** Files above; message "Ports: modbus_framing=rtu and modbus_broadcast=answer".

- [ ] **Step 9: Checkpoint A, on the gateway, only with the user present.**
  1. `git status` clean; reconfigure and build:
     `cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake && cmake --build build`.
  2. Loader: `cmp -l build/loader_flash.bin <baseline>`: only bytes 680 to 703 (decimal offsets
     as `cmp` prints them, 1-based) may differ.
  3. With the user's go: `python3 tools/n540.py <gateway> update build/app.bin`; after 35 s,
     crashes 0, port 1 listening with `serial_tx` 0 and both clients, MQTT connected.
  4. With the user's go, switch the inverters' ports:
     `set port2.mode=modbus port2.modbus_framing=rtu port2.modbus_broadcast=answer port3.mode=modbus port3.modbus_framing=rtu port3.modbus_broadcast=answer`
     (local ports stay 4002/4003). SolarAssistant reconnects within seconds; over two minutes
     each port's `modbus.requests` and `replies` rise together in `/api/state`, `timeouts` stays
     near 0, and SolarAssistant still shows live values.
  5. If anything is wrong: `set port2.mode=server port3.mode=server` restores the old path.

---

### Task 3: Control statements in profiles, and decoding them

**Files:**
- Modify: `src/app/profile.h`, `src/app/profile.c`, `src/app/pfdecode.c`, `src/app/device.c` (`device_value`)
- Test: `tests/test_profile.c`, `tests/test_pfdecode.c`; new seed `tests/fuzz/seeds/profile/controls`

**Interfaces:**
- Produces: kinds `PF_NUMBER, PF_SELECT, PF_SWITCH, PF_TIME` (between `PF_ASCII` and `PF_CALC`),
  flag `PFF_HIDDEN = 4`, `static inline bool pf_control(const pf_field_t *f)`. Field members as
  used by controls: a number's min `lo`, max `hi`, step `k`; a switch's on value `k`, off value
  `lo`, its bits `mask`; a select's bits `mask` (0xFFFF without `mask=`), its choices in `vname`.

- [ ] **Step 1: Write the failing tests.** In `tests/test_profile.c` change line 160 to
  `bad(HEAD "number n \"N\" reg 0x10 u16 min 0 max 9 step 1\n", "line 3: no block 'reg'");` and add
  before `puts("profile: ok");`:

```c
    /* Controls: read from a hold block like the rest; what each statement makes. */
    assert(parse(HEAD
        "number lim \"Limit\" b 0x10 u16 *0.1 min 0 max 25 kW\n"
        "number amps \"Amps\" b 0x11 u16 min 0 max 250 step 5 A hidden\n"
        "number big \"Big\" b 0x12 s32le min -100000 max 100000\n"
        "select by \"By\" b 0x14 mask=0x000E 0=Off 2=Time 4=Voltage 6=SOC\n"
        "select pri \"Priority\" b 0x15 0=\"Battery first\" 1=\"PV first\"\n"
        "switch ac \"AC\" b 0x16 bit 7\n"
        "switch run \"Run\" b 0x17 on 1 off 0 hidden\n"
        "time t1 \"Start\" b 0x18\n"));
    assert(F("lim")->kind == PF_NUMBER && F("lim")->scale == 0.1f && F("lim")->lo == 0 && F("lim")->hi == 25 &&
           F("lim")->k == 0.1f && pf_control(F("lim")) && !(F("lim")->flags & PFF_HIDDEN));
    assert(F("amps")->k == 5 && (F("amps")->flags & PFF_HIDDEN) && F("big")->type == T_S32LE && F("big")->k == 1);
    assert(F("by")->kind == PF_SELECT && F("by")->mask == 0x000E && F("by")->nvname == 4 && P.vname[F("by")->vname + 3].value == 6);
    assert(F("pri")->mask == 0xFFFF);
    assert(F("ac")->kind == PF_SWITCH && F("ac")->mask == 0x80 && F("ac")->k == 0x80 && F("ac")->lo == 0);
    assert(F("run")->mask == 0xFFFF && F("run")->k == 1 && F("run")->lo == 0 && (F("run")->flags & PFF_HIDDEN));
    assert(F("t1")->kind == PF_TIME && F("t1")->type == T_U16 && !pf_control(&(pf_field_t){ .kind = PF_CALC }));
    packed();
    bad("profile t 1\nblock i input 0 4 10s\nnumber n \"N\" i 0 u16 min 0 max 9\n", "line 3: a control needs a hold block");
    bad(HEAD "number n \"N\" b 0x10 u8l min 0 max 9\n", "line 3: a number is u16, s16, u32, s32, u32le or s32le");
    bad(HEAD "number n \"N\" b 0x10 u16 min 0\n", "line 3: a number needs min and max");
    bad(HEAD "number n \"N\" b 0x10 u16 min 9 max 0\n", "line 3: min is above max");
    bad(HEAD "number n \"N\" b 0x10 u16 min 0 max 70000\n", "line 3: min and max must fit the register");
    bad(HEAD "number n \"N\" b 0x10 u16 *0.1 min -1 max 5\n", "line 3: min and max must fit the register");
    bad(HEAD "number n \"N\" b 0x10 u16 min 0 max 9 step 0\n", "line 3: step must be above 0");
    bad(HEAD "number n \"N\" b 0x10 u16 min x max 9\n", "line 3: min needs a number");
    bad(HEAD "number n \"N\" b 0x23 u32 min 0 max 9\n", "line 3: 0x0023 is outside block b");
    bad(HEAD "select s \"S\" b 0x10 mask=0x0E 1=A\n", "line 3: 1 is outside the mask");
    bad(HEAD "select s \"S\" b 0x10 0=A mask=0x0E\n", "line 3: mask comes before the values");
    bad(HEAD "select s \"S\" b 0x10 0=A 1=A\n", "line 3: two choices are named A");
    bad(HEAD "select s \"S\" b 0x10\n", "line 3: select needs: id");
    bad(HEAD "switch w \"W\" b 0x10 bit 16\n", "line 3: bit must be 0 to 15");
    bad(HEAD "switch w \"W\" b 0x10 on 1 off 1\n", "line 3: switch needs on V off V");
    bad(HEAD "switch w \"W\" b 0x10 up 1\n", "line 3: switch needs bit N, or on V off V");
    bad(HEAD "time t \"T\" b 0x10 wide\n", "line 3: unknown word 'wide'");
    bad(HEAD "button b1 \"B\" b 0x10 value 1\n", "line 3: button comes with a later firmware");
    assert(parse(HEAD "sensor up \"Up\" b 0x10 u16 min\n") && !strcmp(pf_str(&P, F("up")->unit), "min"));   /* minutes, still */
```

  In `tests/test_pfdecode.c` add before `puts("pfdecode: ok");`:

```c
    /* Controls: read as the rest. A switch that reads neither on nor off is unknown, and so is
     * a time that is not one. */
    static const char *C =
        "profile c 1\n"
        "block s hold 60 6 1m\n"
        "number lim \"Limit\" s 60 u16 *0.1 min 0 max 25 kW\n"
        "select by \"By\" s 61 mask=0x000E 0=Off 2=Time 4=Voltage 6=SOC\n"
        "switch ac \"AC\" s 62 bit 7\n"
        "switch run \"Run\" s 63 on 1 off 0\n"
        "time t1 \"Start\" s 64\n"
        "time t2 \"End\" s 65\n";
    char e2[96], tx[16];
    assert(profile_parse(&B, C, strlen(C), e2, sizeof e2));
    profile_view(&B, &P);
    pf_state_init(&S);
    uint8_t r[12] = { 0, 120, 0xF3, 0x55, 0xF3, 0x55, 0, 5, 0x3B, 0x17, 0x00, 0x18 };
    assert(pf_decode(&P, 0, r, 12, &S));
    assert(fabsf(S.val[F("lim")].v - 12.0f) < 1e-4f && pf_ok(&S, F("lim")) && pf_prec(&P.field[F("lim")]) == 1);
    assert(pf_text(&P, F("by"), &S, tx, sizeof tx) && !strcmp(tx, "Voltage") && S.val[F("by")].raw == 4);
    assert(pf_ok(&S, F("ac")) && S.val[F("ac")].v == 0 && S.val[F("ac")].raw == 0);
    assert(!pf_ok(&S, F("run")));
    assert(pf_text(&P, F("t1"), &S, tx, sizeof tx) == 5 && !strcmp(tx, "23:59"));
    assert(!pf_ok(&S, F("t2")) && pf_text(&P, F("t2"), &S, tx, sizeof tx) == 0);
    r[5] = 0xD5;                                                /* 0xF3D5: bit 7 set */
    assert(pf_decode(&P, 0, r, 12, &S) && S.val[F("ac")].v == 1 && S.val[F("ac")].raw == 0x80);
```

- [ ] **Step 2: Run to see them fail.** `tests/host.sh 2>&1 | grep -E '^(profile|pfdecode)'`:
  `BUILD FAILED` (`PF_NUMBER`, `pf_control` undeclared).

- [ ] **Step 3: profile.h.** Replace the kinds enum and the flags enum:

```c
enum { PF_SENSOR, PF_BINARY, PF_ENUM, PF_BITS, PF_ASCII,
       PF_NUMBER, PF_SELECT, PF_SWITCH, PF_TIME,    /* controls: read as the rest, and written (pfwrite.c) */
       PF_CALC, PF_CALCBIN };                       /* calcs last: pfdecode.c works out every kind from PF_CALC */
enum { PFF_DIAG = 1, PFF_WITHIN = 2, PFF_HIDDEN = 4 };    /* hidden: a control HA announces disabled */
```

  In `pf_field_t` change the comments of `mask` and `scale, k, lo, hi` to:

```c
    uint32_t mask, nodata[4];          /* mask: an enum's, a select's or a switch's bits; anybits/nobits' */
    float    scale, k, lo, hi;         /* k: a comparison's constant, a number's step, a switch's "on" value;
                                          lo, hi: within, or a number's min and max; lo: a switch's "off" value */
```

  and after `pf_str()` add
  `static inline bool pf_control(const pf_field_t *f) { return f->kind >= PF_NUMBER && f->kind <= PF_TIME; }`.

- [ ] **Step 4: profile.c.** Extend the mods flags:
  `enum { M_SCALE = 1, M_UNIT = 2, M_HA = 4, M_DIAG = 8, M_WITHIN = 16, M_NODATA = 32, M_HIDDEN = 64, M_LIMITS = 128 };`.
  Add before `mods()`:

```c
/* A number's min and max, as the register values they are written as, fit its type. */
static bool fits_type(const pf_field_t *f)
{
    double lo = 0, hi = 65535;
    if (f->type == T_S16) { lo = -32768; hi = 32767; }
    else if (f->type == T_U32 || f->type == T_U32LE) hi = 4294967295.0;
    else if (f->type == T_S32 || f->type == T_S32LE) { lo = -2147483648.0; hi = 2147483647.0; }
    double a = (double)f->lo / f->scale - f->offset, b = (double)f->hi / f->scale - f->offset;
    return a >= lo - 0.5 && a <= hi + 0.5 && b >= lo - 0.5 && b <= hi + 0.5;
}
```

  In `mods()` declare `unsigned lim = 0;` beside `bool unit = false;`, add these two branches
  just before the unit branch (`} else if ((allow & M_UNIT) && !unit ...`):

```c
        } else if (!strcmp(t, "hidden") && (allow & M_HIDDEN)) {
            f->flags |= PFF_HIDDEN;
        } else if ((allow & M_LIMITS) && (!strcmp(t, "min") || !strcmp(t, "max") || !strcmp(t, "step"))) {
            if (i + 1 >= s->ntok || !num_f(s->tok[i + 1], &x)) return fail(s, "%s needs a number", t);
            if (t[1] == 'i') { f->lo = x; lim |= 1; }
            else if (t[1] == 'a') { f->hi = x; lim |= 2; }
            else if (x <= 0) return fail(s, "step must be above 0");
            else f->k = x;
            i++;
```

  and before `mods()`'s final `return true;`:

```c
    if (allow & M_LIMITS) {
        if (lim != 3) return fail(s, "a number needs min and max");
        if (f->hi < f->lo) return fail(s, "min is above max");
        if (!f->k) f->k = f->scale < 0 ? -f->scale : f->scale;
        if (!fits_type(f)) return fail(s, "min and max must fit the register");
    }
```

  Add after `st_text()`:

```c
/* id "name" block address, of a control: from a hold block, which it is written back to. */
static bool where_ctl(ps_t *s, pf_field_t *t, const char **name)
{
    if (!where(s, t, name)) return false;
    if (s->p->block[t->block].fn != 3) return fail(s, "a control needs a hold block");
    return true;
}

/* number id "name" block address type [value] min M max M [step S] [shown] [hidden] */
static bool st_number(ps_t *s)
{
    pf_field_t t;
    const char *name;
    tpl_init(&t);
    if (s->ntok < 6) return fail(s, "number needs: id \"name\" block address type min M max M");
    if (!where_ctl(s, &t, &name)) return false;
    int ti = type_of(s->tok[5]);
    if (ti < 0 || TYPES[ti].type > T_S32LE) return fail(s, "a number is u16, s16, u32, s32, u32le or s32le");
    t.kind = PF_NUMBER;
    t.type = TYPES[ti].type;
    return each(s, s->tok[1], name, &t, TYPES[ti].regs, 6, M_SCALE | M_UNIT | M_HA | M_DIAG | M_HIDDEN | M_LIMITS);
}

/* select id "name" block address [mask=M] v=name ... [diag] [hidden]: each value as it is in
 * the register, under the mask (as an enum reads it). */
static bool st_select(ps_t *s)
{
    pf_build_t *p = s->p;
    pf_field_t t;
    const char *name;
    int64_t l;
    tpl_init(&t);
    t.type = T_U16;
    t.mask = 0xFFFF;
    if (s->ntok < 6) return fail(s, "select needs: id \"name\" block address v=name ...");
    if (!where_ctl(s, &t, &name)) return false;
    t.kind = PF_SELECT;
    t.vname = p->nvname;
    for (int i = 5; i < s->ntok; i++) {
        char *w = s->tok[i], *eq = strchr(w, '=');
        if (!strcmp(w, "diag")) { t.flags |= PFF_DIAG; continue; }
        if (!strcmp(w, "hidden")) { t.flags |= PFF_HIDDEN; continue; }
        if (!eq) return fail(s, "expected value=name, not '%s'", w);
        *eq = 0;
        if (!strcmp(w, "mask")) {
            if (p->nvname != t.vname) return fail(s, "mask comes before the values");
            if (!num_i(eq + 1, 1, 0xFFFF, &l)) return fail(s, "bad mask '%s'", eq + 1);
            t.mask = (uint32_t)l;
            continue;
        }
        if (!num_i(w, 0, 0xFFFF, &l)) return fail(s, "bad value '%s'", w);
        if ((uint32_t)l & ~t.mask) return fail(s, "%s is outside the mask", w);
        char *vn = unq(eq + 1);
        if (!vn || !*vn || strlen(vn) > PF_NAME_MAX) return fail(s, "the name must be \"quoted\", 1 to %d characters", PF_NAME_MAX);
        for (int k = t.vname; k < p->nvname; k++)
            if (!strcmp(p->pool + p->vname[k].name, vn)) return fail(s, "two choices are named %s", vn);
        if (p->nvname == PF_VNAMES) return fail(s, "too many value names (%d)", PF_VNAMES);
        p->vname[p->nvname].value = (uint32_t)l;
        if (!pool_add(s, vn, &p->vname[p->nvname].name)) return false;
        p->nvname++;
    }
    t.nvname = (uint16_t)(p->nvname - t.vname);
    if (!t.nvname) return fail(s, "select needs at least one value=name");
    return each(s, s->tok[1], name, &t, 1, s->ntok, 0);
}

/* switch id "name" block address, then bit N or on V off V, [diag] [hidden] */
static bool st_switch(ps_t *s)
{
    pf_field_t t;
    const char *name;
    int64_t a, b;
    int i;
    tpl_init(&t);
    t.type = T_U16;
    if (s->ntok < 7) return fail(s, "switch needs: id \"name\" block address, then bit N or on V off V");
    if (!where_ctl(s, &t, &name)) return false;
    t.kind = PF_SWITCH;
    if (!strcmp(s->tok[5], "bit")) {
        if (!num_i(s->tok[6], 0, 15, &a)) return fail(s, "bit must be 0 to 15");
        t.mask = 1u << a;
        t.k = (float)(1u << a);
        t.lo = 0;
        i = 7;
    } else if (!strcmp(s->tok[5], "on")) {
        if (s->ntok < 9 || strcmp(s->tok[7], "off") || !num_i(s->tok[6], 0, 0xFFFF, &a) || !num_i(s->tok[8], 0, 0xFFFF, &b) || a == b)
            return fail(s, "switch needs on V off V, two different values of 0 to 65535");
        t.mask = 0xFFFF;
        t.k = (float)a;
        t.lo = (float)b;
        i = 9;
    } else {
        return fail(s, "switch needs bit N, or on V off V");
    }
    return each(s, s->tok[1], name, &t, 1, i, M_DIAG | M_HIDDEN);
}

/* time id "name" block address [diag] [hidden]: HH:MM, the hour in the low byte */
static bool st_time(ps_t *s)
{
    pf_field_t t;
    const char *name;
    tpl_init(&t);
    t.type = T_U16;
    t.mask = 0xFFFF;
    if (s->ntok < 5) return fail(s, "time needs: id \"name\" block address");
    if (!where_ctl(s, &t, &name)) return false;
    t.kind = PF_TIME;
    return each(s, s->tok[1], name, &t, 1, 5, M_DIAG | M_HIDDEN);
}
```

  In `profile_parse()` replace the `number/select/switch/button` "phase 2" branch with:

```c
        else if (!strcmp(w, "number")) ok = st_number(&s);
        else if (!strcmp(w, "select")) ok = st_select(&s);
        else if (!strcmp(w, "switch")) ok = st_switch(&s);
        else if (!strcmp(w, "time")) ok = st_time(&s);
        else if (!strcmp(w, "button")) ok = fail(&s, "button comes with a later firmware");
```

- [ ] **Step 5: pfdecode.c.** In `pf_decode()`'s field loop replace from
  `if (f->kind == PF_ENUM) raw &= f->mask;` through the `else s->val[i].v = (float)raw;` line with:

```c
        if (f->kind == PF_ENUM || f->kind == PF_SELECT || f->kind == PF_SWITCH) raw &= f->mask;
        if (f->kind == PF_SWITCH) ok = ok && ((float)raw == f->k || (float)raw == f->lo);
        if (f->kind == PF_TIME) ok = ok && (raw & 0xFFu) < 24 && raw >> 8 < 60;
        s->val[i].raw = raw;
        if (f->kind == PF_SENSOR || f->kind == PF_NUMBER) s->val[i].v = value_of(f, raw);
        else if (f->kind == PF_SWITCH) s->val[i].v = (float)raw == f->k;
        else if (f->kind == PF_BINARY && f->type != T_BIT && f->type != T_COIL) s->val[i].v = cmp(f->op, value_of(f, raw), f->k);
        else s->val[i].v = (float)raw;
```

  In `pf_text()`: after `if (f->kind == PF_ASCII) w = ...;` add
  `else if (f->kind == PF_TIME) w = snprintf(out, max, "%02u:%02u", (unsigned)(s->val[i].raw & 0xFFu), (unsigned)(s->val[i].raw >> 8));`,
  and in its value-name loop treat a select as an enum: each `f->kind == PF_ENUM` there becomes
  `(f->kind == PF_ENUM || f->kind == PF_SELECT)` (three places). In `pf_prec()` change
  `if (f->kind != PF_SENSOR) return 0;` to `if (f->kind != PF_SENSOR && f->kind != PF_NUMBER) return 0;`.
  Update `pfdecode.h`'s `pf_text` comment: "A text field, a select or a time: ... a select's
  choice, a time's HH:MM ...".

- [ ] **Step 6: device_value.** In `src/app/device.c` `device_value()`: the text branch condition
  becomes `x->kind == PF_ENUM || x->kind == PF_BITS || x->kind == PF_ASCII || x->kind == PF_SELECT || x->kind == PF_TIME`,
  and the true/false branch `x->kind == PF_BINARY || x->kind == PF_CALCBIN || x->kind == PF_SWITCH`.

- [ ] **Step 7: Fuzz seed.** Create `tests/fuzz/seeds/profile/controls` holding the `C` profile
  text of step 1, as a plain file.

- [ ] **Step 8: Run.** `tests/host.sh` all ok; `tests/fuzz/run.sh 20 profile` no findings.

- [ ] **Step 9: Commit.** Message "Profiles: number, select, switch and time controls, read as
  the rest".

---

### Task 4: A command into what is written (pfwrite.c)

**Files:**
- Create: `src/app/pfwrite.h`, `src/app/pfwrite.c`, `tests/test_pfwrite.c`, `tests/fuzz/command.c`
- Modify: `tests/host.sh`, `tests/fuzz/run.sh`

**Interfaces:**
- Consumes: Task 3's control kinds and members.
- Produces:

```c
typedef struct { uint16_t addr; uint8_t regs; uint16_t mask[2], value[2]; } pf_write_t;
bool pf_command(const profile_t *p, int i, const char *payload, pf_write_t *w, char *why, size_t max);
bool pf_merge(const pf_write_t *w, const uint16_t have[2], uint16_t out[2]);
```

  The `why` texts: "not a number", "out of range", "not one of the choices", "not ON or OFF",
  "not a time of day", "not a control".

- [ ] **Step 1: Write the failing test** `tests/test_pfwrite.c`:

```c
/* test_pfwrite.c — commands into writes (src/app/pfwrite.c).
 *   cc -Isrc/app tests/test_pfwrite.c src/app/pfwrite.c src/app/profile.c -lm -o /tmp/pw && /tmp/pw */
#include "pfwrite.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static pf_build_t B;
static profile_t P;
static const char *T =
    "profile c 1\n"
    "block s hold 60 12 1m\n"
    "number lim \"Limit\" s 60 u16 *0.1 min 0 max 25 kW\n"
    "number amps \"Amps\" s 61 u16 min 0 max 250 step 5 A\n"
    "number tmp \"Temp\" s 62 s16 *0.1 min -20 max 60\n"
    "number big \"Big\" s 63 u32 min 0 max 100000\n"
    "number lit \"Little\" s 65 s32le min -100000 max 100000\n"
    "select by \"By\" s 67 mask=0x000E 0=Off 2=Time 4=Voltage 6=SOC\n"
    "switch ac \"AC\" s 68 bit 7\n"
    "switch run \"Run\" s 69 on 1 off 0\n"
    "time t1 \"Start\" s 70\n"
    "sensor v \"V\" s 71 u16\n";
static int F(const char *id) { int i = profile_field(&P, id); assert(i >= 0); return i; }
static pf_write_t W;
static char why[48];
static bool cmd(const char *id, const char *s) { return pf_command(&P, F(id), s, &W, why, sizeof why); }

int main(void)
{
    char e[96];
    assert(profile_parse(&B, T, strlen(T), e, sizeof e));
    profile_view(&B, &P);
    /* A number: scaled, rounded to its step, within min and max. */
    assert(cmd("lim", "12") && W.addr == 60 && W.regs == 1 && W.mask[0] == 0xFFFF && W.value[0] == 120);
    assert(cmd("lim", "12.04") && W.value[0] == 120 && cmd("lim", "12.0") && W.value[0] == 120);
    assert(cmd("lim", "25") && W.value[0] == 250 && cmd("lim", "0") && W.value[0] == 0);
    assert(!cmd("lim", "25.1") && !strcmp(why, "out of range"));
    assert(!cmd("lim", "-0.1") && !strcmp(why, "out of range"));
    assert(!cmd("lim", "abc") && !strcmp(why, "not a number"));
    assert(!cmd("lim", "") && !cmd("lim", "1e3") && !cmd("lim", "nan") && !strcmp(why, "not a number"));
    assert(cmd("amps", "52") && W.value[0] == 50 && cmd("amps", "53") && W.value[0] == 55);
    assert(!cmd("amps", "253") && !strcmp(why, "out of range"));          /* to the step: 255 */
    assert(cmd("tmp", "-5") && W.value[0] == (uint16_t)-50);
    assert(cmd("big", "70000") && W.regs == 2 && W.value[0] == 1 && W.value[1] == 4464);    /* high word first */
    assert(cmd("lit", "-2") && W.regs == 2 && W.addr == 65 && W.value[0] == 0xFFFE && W.value[1] == 0xFFFF);   /* low first */
    /* A select: a choice's name, in its bits. */
    assert(cmd("by", "SOC") && W.mask[0] == 0x000E && W.value[0] == 6);
    assert(!cmd("by", "soc") && !strcmp(why, "not one of the choices"));
    /* A switch: ON and OFF, as its bit or its two values. */
    assert(cmd("ac", "ON") && W.mask[0] == 0x80 && W.value[0] == 0x80 && cmd("ac", "OFF") && W.value[0] == 0);
    assert(cmd("run", "ON") && W.mask[0] == 0xFFFF && W.value[0] == 1);
    assert(!cmd("run", "on") && !strcmp(why, "not ON or OFF"));
    /* A time: HH:MM, the hour in the low byte. */
    assert(cmd("t1", "23:59") && W.value[0] == 0x3B17 && cmd("t1", "00:00") && W.value[0] == 0);
    assert(!cmd("t1", "24:00") && !cmd("t1", "7:30") && !cmd("t1", "07:60") && !cmd("t1", "0730") && !strcmp(why, "not a time of day"));
    assert(!cmd("v", "1") && !strcmp(why, "not a control"));
    /* Merged with what was read: only the control's bits change. */
    uint16_t have[2] = { 0xF355, 0 }, out[2];
    assert(cmd("ac", "ON") && pf_merge(&W, have, out) && out[0] == 0xF3D5);
    have[0] = 0xF3D5;
    assert(!pf_merge(&W, have, out) && out[0] == 0xF3D5);                 /* already so: nothing to write */
    assert(cmd("by", "Time") && pf_merge(&W, have, out) && out[0] == 0xF3D3);
    have[0] = 1; have[1] = 4464;
    assert(cmd("big", "70000") && !pf_merge(&W, have, out));
    have[1] = 0;
    assert(pf_merge(&W, have, out) && out[0] == 1 && out[1] == 4464);
    puts("pfwrite: ok");
    return 0;
}
```

  Add to `tests/host.sh` after the `pfdecode` line:
  `t pfwrite -Isrc/app tests/test_pfwrite.c src/app/pfwrite.c src/app/profile.c -lm`.

- [ ] **Step 2: Run to see it fail.** `tests/host.sh 2>&1 | grep -A2 '^pfwrite'`: `BUILD FAILED`.

- [ ] **Step 3: Implement.** `src/app/pfwrite.h`:

```c
/*
 * pfwrite.h — A command for a control (profile.h) as what is written: the register or two at
 * the control's address, the bits it owns there, and their new value. No I/O: device.c reads
 * the registers first and writes only if this changes them.
 */
#ifndef PFWRITE_H
#define PFWRITE_H
#include "profile.h"

typedef struct {
    uint16_t addr;
    uint8_t  regs;                 /* 1, or 2 for a 32-bit number */
    uint16_t mask[2], value[2];    /* per register: the bits written, and what they become */
} pf_write_t;

/* The payload for control field i: a number in decimal ("80", "12.5"), a choice's name, ON or
 * OFF, or HH:MM. False, with the reason in why, if it will not do. */
bool pf_command(const profile_t *p, int i, const char *payload, pf_write_t *w, char *why, size_t max);
/* `have`, the registers as read, with the write applied, into out. True if that changes them. */
bool pf_merge(const pf_write_t *w, const uint16_t have[2], uint16_t out[2]);
#endif
```

  `src/app/pfwrite.c`:

```c
/*
 * pfwrite.c — Commands into writes (pfwrite.h). Numbers are read by hand, as profile.c reads
 * them: no exponents, no "inf" or "nan", nothing a locale could change.
 */
#include "pfwrite.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool no(char *why, size_t max, const char *s) { if (max) snprintf(why, max, "%s", s); return false; }

/* [-]digits[.digits]; NaN if it is not that. */
static double number(const char *t)
{
    double v = 0, f = 1;
    bool neg = *t == '-', any = false;
    if (*t == '-' || *t == '+') t++;
    for (; *t >= '0' && *t <= '9'; t++) { v = v * 10 + (*t - '0'); any = true; if (v > 1e12) return NAN; }
    if (*t == '.') for (t++; *t >= '0' && *t <= '9'; t++) { f /= 10; v += (*t - '0') * f; any = true; }
    return any && !*t ? (neg ? -v : v) : NAN;
}

static bool two(const char *t) { return t[0] >= '0' && t[0] <= '9' && t[1] >= '0' && t[1] <= '9'; }

bool pf_command(const profile_t *p, int i, const char *s, pf_write_t *w, char *why, size_t max)
{
    const pf_field_t *f = &p->field[i];
    memset(w, 0, sizeof *w);
    w->addr = f->addr;
    w->regs = 1;
    w->mask[0] = (uint16_t)f->mask;
    switch (f->kind) {
    case PF_NUMBER: {
        double v = number(s), step = f->k > 0 ? f->k : 1;
        if (isnan(v)) return no(why, max, "not a number");
        v = round(v / step) * step;
        if (v < f->lo - step / 1000 || v > f->hi + step / 1000) return no(why, max, "out of range");
        double r = round(v / f->scale - f->offset);
        uint32_t raw = f->type == T_S16 || f->type == T_S32 || f->type == T_S32LE ? (uint32_t)(int32_t)r : (uint32_t)r;
        w->mask[0] = 0xFFFF;
        if (f->type == T_U16 || f->type == T_S16) { w->value[0] = (uint16_t)raw; return true; }
        bool le = f->type == T_U32LE || f->type == T_S32LE;
        w->regs = 2;
        w->mask[1] = 0xFFFF;
        w->value[0] = (uint16_t)(le ? raw : raw >> 16);
        w->value[1] = (uint16_t)(le ? raw >> 16 : raw);
        return true;
    }
    case PF_SELECT:
        for (int k = 0; k < f->nvname; k++) {
            const pf_vname_t *n = &p->vname[f->vname + k];
            if (!strcmp(pf_str(p, n->name), s)) { w->value[0] = (uint16_t)n->value; return true; }
        }
        return no(why, max, "not one of the choices");
    case PF_SWITCH:
        if (!strcmp(s, "ON")) w->value[0] = (uint16_t)f->k;
        else if (!strcmp(s, "OFF")) w->value[0] = (uint16_t)f->lo;
        else return no(why, max, "not ON or OFF");
        return true;
    case PF_TIME: {
        if (strlen(s) != 5 || s[2] != ':' || !two(s) || !two(s + 3)) return no(why, max, "not a time of day");
        unsigned h = (unsigned)(s[0] - '0') * 10u + (unsigned)(s[1] - '0'), m = (unsigned)(s[3] - '0') * 10u + (unsigned)(s[4] - '0');
        if (h > 23 || m > 59) return no(why, max, "not a time of day");
        w->value[0] = (uint16_t)(m << 8 | h);
        return true;
    }
    default:
        return no(why, max, "not a control");
    }
}

bool pf_merge(const pf_write_t *w, const uint16_t have[2], uint16_t out[2])
{
    bool change = false;
    for (int r = 0; r < w->regs; r++) {
        out[r] = (uint16_t)((have[r] & ~w->mask[r]) | (w->value[r] & w->mask[r]));
        change = change || out[r] != have[r];
    }
    return change;
}
```

- [ ] **Step 4: Fuzz target** `tests/fuzz/command.c`:

```c
/* command.c — commands for every control of every built-in profile (src/app/pfwrite.c): a
 * field picked by the first byte, the rest as the payload. What must hold: refused with a
 * reason, or a write of 1 or 2 registers at the field's address whose value lies in its mask,
 * and a merge that changes only masked bits. */
#include "pfwrite.h"
#include "profiles.h"
#include <assert.h>
#include <string.h>

static pf_build_t B[8];
static profile_t P[8];
static int np = -1;

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (np < 0) {
        char e[96];
        size_t len;
        for (np = 0; np < 8 && profiles_id(np); np++) {
            const char *t = profiles_text(profiles_id(np), &len);
            assert(profile_parse(&B[np], t, len, e, sizeof e));
            profile_view(&B[np], &P[np]);
        }
    }
    if (n < 2 || n > 64 || !np) return 0;
    const profile_t *p = &P[d[0] % np];
    int i = d[1] % p->nfield;
    char s[64];
    memcpy(s, d + 2, n - 2);
    s[n - 2] = 0;
    pf_write_t w;
    char why[32] = "";
    if (!pf_command(p, i, s, &w, why, sizeof why)) { assert(why[0]); return 0; }
    assert(pf_control(&p->field[i]) && w.addr == p->field[i].addr && (w.regs == 1 || w.regs == 2));
    uint16_t have[2] = { 0xA5A5, 0x5A5A }, out[2];
    pf_merge(&w, have, out);
    for (int r = 0; r < w.regs; r++) assert(!(w.value[r] & ~w.mask[r]) && !((out[r] ^ have[r]) & ~w.mask[r]));
    return 0;
}
```

  In `tests/fuzz/run.sh` add `command` to the default target list (`${*:-cfg http hclient modbus profile command}`)
  and a case line:
  `command) python3 tools/embed_profiles.py "$OUT/profiles_gen.h" profiles/*.txt > /dev/null && build command -I"$OUT" -Isrc/app tests/fuzz/command.c src/app/pfwrite.c src/app/profiles.c src/app/profile.c -lm ;;`.
  Give it seeds: `mkdir -p tests/fuzz/seeds/command` and files `on` (bytes `00 00 4f 4e`, "ON"),
  `time` (`00 00` then "07:30") and `num` (`00 00` then "12.5"), written with `printf`.

- [ ] **Step 5: Run.** `tests/host.sh` (expect `pfwrite: ok`); `tests/fuzz/run.sh 20 command`.
  Until Task 7 the built-in profiles have no controls, so the fuzz target only checks refusals.

- [ ] **Step 6: Commit.** Message "Controls: a command into the bits it writes (pfwrite.c)".

---

### Task 5: The write path in the engine

**Files:**
- Modify: `src/app/device.h`, `src/app/device.c`, `src/app/events.h`, `web/index.html` (evText, evBad), `tests/host.sh`
- Create: `tests/stub/events_stub.c`, `tests/test_devctl.c`

**Interfaces:**
- Consumes: `pf_command`, `pf_merge`, `pf_write_t` (Task 4); `pf_control` (Task 3).
- Produces: `bool device_command(const char *name, const char *control, const char *payload, bool retained);`
  `EV_WRITE` (events.h): port = the device's port (0 if none), a = device number (1-16, 0 if
  unknown) | outcome << 8 with outcomes 0 ok, 1 unchanged, 2 refused, 3 failed, 4 reads back
  otherwise; s = "control=value", then "; " and the detail when there is one.

- [ ] **Step 1: The event stub, and every engine test linking it.** Create `tests/stub/events_stub.c`:

```c
/* events_stub.c — events_add() for host tests that link device.c without events.c: the last
 * one kept for the test to look at, and a count. */
#include "events.h"
#include <stdio.h>

int ev_n, ev_port;
ev_kind_t ev_kind;
uint32_t ev_a;
char ev_s[64];

void events_add(ev_kind_t k, int port, uint32_t a, const char *s)
{
    ev_n++;
    ev_kind = k;
    ev_port = port;
    ev_a = a;
    snprintf(ev_s, sizeof ev_s, "%s", s ? s : "");
}
```

  In `tests/host.sh`, add `tests/stub/events_stub.c src/app/pfwrite.c` to the `device`,
  `devmqtt`, `devroom` and `devmqtt_long` lines, and add:
  `t devctl -DD2_BSS= -Itests/stub -Isrc/app tests/test_devctl.c tests/stub/events_stub.c src/app/device.c src/app/pfwrite.c src/app/modbus.c src/app/profile.c src/app/pfdecode.c src/app/cfg.c src/app/acl.c -lm`.

- [ ] **Step 2: Write the failing test** `tests/test_devctl.c`:

```c
/* test_devctl.c — writes to devices' controls (src/app/device.c, pfwrite.c): a command
 * queued, the register read, written only if that changes it, the block read back; refusals,
 * failures and their events. Own profile source; a real Modbus queue, a fake line and clock.
 *   see tests/host.sh for the build line */
#include "device.h"
#include "events.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t t_now = 1000;
uint32_t SystemCoreClock = 400000000;
extern int ev_n, ev_port;
extern ev_kind_t ev_kind;
extern uint32_t ev_a;
extern char ev_s[64];
static modbus_t M;
modbus_t *bridge_modbus_port(int p) { return p == 1 ? &M : NULL; }

static const char *T =
    "profile c 1\n"
    "name \"Controlled\"\n"
    "block live input 0 2 10s\n"
    "block set hold 20 6 1m info\n"
    "sensor v \"V\" live 0 u16\n"
    "number lim \"Limit\" set 20 u16 *0.1 min 0 max 25 kW\n"
    "switch ac \"AC\" set 21 bit 7\n"
    "time t1 \"Start\" set 22\n"
    "number big \"Big\" set 23 u32 min 0 max 100000\n"
    "select by \"By\" set 25 mask=0x000E 0=Off 2=Time 4=Voltage 6=SOC hidden\n";
const char *profiles_id(int i) { return i ? NULL : "c"; }
const char *profiles_text(const char *id, size_t *len) { if (strcmp(id, "c")) return NULL; *len = strlen(T); return T; }

static uint8_t L[512]; static size_t Ln;
static size_t line(void *ctx, const uint8_t *d, size_t n) { (void)ctx; memcpy(L + Ln, d, n); Ln += n; return n; }
static void reply(void *ctx, int c, const uint8_t *d, size_t n) { (void)ctx; (void)c; (void)d; (void)n; }
static void local(void *ctx, uint32_t tag, int st, const uint8_t *pdu, size_t n) { (void)ctx; device_reply(1, tag, st, pdu, n); }
static const mb_io_t IO = { line, reply, local, NULL };
static void run(void) { device_poll(); modbus_poll(&M, t_now, 1000000, true); modbus_poll(&M, t_now, 1000000, true); }
static void answer(const uint8_t *pdu, size_t n)           /* from unit 1, its CRC added */
{
    uint8_t b[300];
    b[0] = 1;
    memcpy(b + 1, pdu, n);
    uint16_t c = modbus_crc(b, n + 1);
    b[n + 1] = (uint8_t)c; b[n + 2] = (uint8_t)(c >> 8);
    modbus_from_line(&M, b, n + 3);
    modbus_poll(&M, t_now, 0, true);
    modbus_poll(&M, t_now, 1000000, true);
}
static bool sent(const uint8_t *pdu, size_t n)             /* what went to unit 1: this PDU */
{
    bool ok = Ln == n + 3 && L[0] == 1 && !memcmp(L + 1, pdu, n);
    if (!ok) { printf("sent %zu:", Ln); for (size_t k = 0; k < Ln; k++) printf(" %02x", L[k]); printf("\n"); }
    Ln = 0;
    return ok;
}
#define PDU(...) (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ })
#define SET_READ PDU(3, 0, 20, 0, 6)
static void set_reply(uint16_t lim, uint16_t ac, uint16_t t1, uint32_t big, uint16_t by)   /* the set block's six registers */
{
    const uint16_t r[6] = { lim, ac, t1, (uint16_t)(big >> 16), (uint16_t)big, by };
    uint8_t p[14] = { 3, 12 };
    for (int k = 0; k < 6; k++) { p[2 + 2 * k] = (uint8_t)(r[k] >> 8); p[3 + 2 * k] = (uint8_t)r[k]; }
    answer(p, sizeof p);
}
static bool event(int outcome, const char *s)
{
    bool ok = ev_kind == EV_WRITE && ev_port == 2 && ev_a == (1u | (uint32_t)outcome << 8) && !strcmp(ev_s, s);
    if (!ok) printf("event %d port %d a %lx \"%s\"\n", ev_kind, ev_port, (unsigned long)ev_a, ev_s);
    return ok;
}
static const char *val(const char *id) { static char v[32]; const device_t *d = device_get(0); assert(device_value(d, profile_field(d->p, id), v, sizeof v)); return v; }

int main(void)
{
    static cfg_t c;
    cfg_defaults(&c);
    c.port[1].mode = MODE_MODBUS;
    c.dev[0] = (dev_cfg_t){ "c", "inv", "", "", 2, 1, 1, 0 };          /* controls on */
    device_init();
    device_configure(&c);
    modbus_init(&M, &IO, 1000, 2000);
    assert(device_get(0)->p);

    run(); assert(sent(PDU(4, 0, 0, 0, 2))); answer(PDU(4, 4, 0, 1, 0, 2), 6);
    run(); assert(sent(SET_READ)); set_reply(100, 0xF355, 0x0001, 0, 0);
    assert(!strcmp(val("lim"), "10.0") && !strcmp(val("ac"), "false") && !strcmp(val("t1"), "\"01:00\"") && !strcmp(val("by"), "\"Off\""));

    /* A switch: its register read, written with its bit alone changed, the block read back. */
    int e0 = ev_n;
    t_now += 600;
    assert(device_command("inv", "ac", "ON", false));
    run(); assert(sent(PDU(3, 0, 21, 0, 1)));
    answer(PDU(3, 2, 0xF3, 0x55), 4);
    run(); assert(sent(PDU(6, 0, 21, 0xF3, 0xD5)));
    answer(PDU(6, 0, 21, 0xF3, 0xD5), 5);
    run(); assert(sent(SET_READ));
    set_reply(100, 0xF3D5, 0x0001, 0, 0);
    assert(ev_n == e0 + 1 && event(0, "ac=ON") && !strcmp(val("ac"), "true"));

    /* Already so: read, not written, and the block read again so HA sees what it holds. */
    t_now += 600;
    assert(device_command("inv", "ac", "ON", false));
    run(); assert(sent(PDU(3, 0, 21, 0, 1)));
    answer(PDU(3, 2, 0xF3, 0xD5), 4);
    assert(event(1, "ac=ON"));
    run(); assert(sent(SET_READ));
    set_reply(100, 0xF3D5, 0x0001, 0, 0);

    /* A number, scaled; the device keeps something else: said so. */
    t_now += 600;
    assert(device_command("inv", "lim", "12.5", false));
    run(); assert(sent(PDU(3, 0, 20, 0, 1)));
    answer(PDU(3, 2, 0, 100), 4);
    run(); assert(sent(PDU(6, 0, 20, 0, 125)));
    answer(PDU(6, 0, 20, 0, 125), 5);
    run(); assert(sent(SET_READ));
    set_reply(120, 0xF3D5, 0x0001, 0, 0);
    assert(event(4, "lim=12.5; 12.0"));

    /* Two registers, by function 16. */
    t_now += 600;
    assert(device_command("inv", "big", "70000", false));
    run(); assert(sent(PDU(3, 0, 23, 0, 2)));
    answer(PDU(3, 4, 0, 0, 0, 0), 6);
    run(); assert(sent(PDU(16, 0, 23, 0, 2, 4, 0, 1, 0x11, 0x70)));
    answer(PDU(16, 0, 23, 0, 2), 5);
    run(); assert(sent(SET_READ));
    set_reply(120, 0xF3D5, 0x0001, 70000, 0);
    assert(event(0, "big=70000"));

    /* The device refuses, or does not answer: said so. */
    t_now += 600;
    assert(device_command("inv", "t1", "07:30", false));
    run(); assert(sent(PDU(3, 0, 22, 0, 1)));
    answer(PDU(3, 2, 0, 1), 4);
    run(); assert(sent(PDU(6, 0, 22, 0x1E, 0x07)));
    answer(PDU(0x86, 3), 2);
    assert(event(3, "t1=07:30; error 3"));
    t_now += 600;
    assert(device_command("inv", "t1", "07:30", false));
    run(); assert(sent(PDU(3, 0, 22, 0, 1)));
    t_now += 1000; run();
    assert(event(3, "t1=07:30; no reply"));

    /* Refused, each with its event: nothing queued, nothing sent. */
    assert(!device_command("inv", "lim", "30", false) && event(2, "lim=30; out of range"));
    assert(!device_command("inv", "v", "1", false) && event(2, "v=1; no such control"));
    assert(!device_command("inv", "nope", "1", false) && event(2, "nope=1; no such control"));
    assert(!device_command("inv", "ac", "ON", true) && event(2, "ac=ON; retained"));
    assert(!device_command("x", "ac", "ON", false) && ev_port == 0 && ev_a == 2u << 8 && !strcmp(ev_s, "x/ac=ON; no such device"));
    run(); assert(Ln == 0);

    /* Controls off: refused. */
    c.dev[0].controls = 0;
    device_configure(&c);
    assert(!device_command("inv", "ac", "ON", false) && event(2, "ac=ON; controls off"));
    c.dev[0].controls = 1;
    device_configure(&c);
    run(); assert(sent(PDU(4, 0, 0, 0, 2))); answer(PDU(4, 4, 0, 1, 0, 2), 6);
    run(); assert(sent(SET_READ)); set_reply(120, 0xF3D5, 0x0001, 70000, 0);

    /* A newer command for a queued control replaces its value and keeps its place; writes go
     * before polls, 2 a second at most per device. */
    t_now += 600;
    assert(device_command("inv", "lim", "5", false) && device_command("inv", "t1", "08:00", false) && device_command("inv", "lim", "6", false));
    run(); assert(sent(PDU(3, 0, 20, 0, 1)));
    answer(PDU(3, 2, 0, 120), 4);
    run(); assert(sent(PDU(6, 0, 20, 0, 60)));
    answer(PDU(6, 0, 20, 0, 60), 5);
    run(); assert(sent(SET_READ));
    set_reply(60, 0xF3D5, 0x0001, 70000, 0);
    assert(event(0, "lim=6"));
    run(); assert(Ln == 0);                                    /* t1 waits: 500 ms since lim's */
    t_now += 500;
    run(); assert(sent(PDU(3, 0, 22, 0, 1)));
    answer(PDU(3, 2, 0, 1), 4);
    run(); assert(sent(PDU(6, 0, 22, 0, 8)));
    answer(PDU(6, 0, 22, 0, 8), 5);
    run(); assert(sent(SET_READ));
    set_reply(60, 0xF3D5, 0x0008, 70000, 0);
    assert(event(0, "t1=08:00"));

    /* 16 waiting at most, across devices. */
    for (int i = 1; i < 5; i++) { c.dev[i] = c.dev[0]; snprintf(c.dev[i].name, sizeof c.dev[i].name, "inv%d", i); c.dev[i].address = (uint8_t)(i + 1); }
    device_configure(&c);
    static const char *const CT[4] = { "lim", "ac", "t1", "big" }, *const PV[4] = { "1", "OFF", "09:00", "5" };
    int queued = 0;
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++) {
            char nm[25];
            if (i) snprintf(nm, sizeof nm, "inv%d", i); else snprintf(nm, sizeof nm, "inv");
            queued += device_command(nm, CT[k], PV[k], false);
        }
    assert(queued == 16 && ev_a == (5u | 2u << 8) && !strcmp(ev_s, "big=5; too many waiting"));

    /* A write on the line when the port's queue starts again (as on a settings change):
     * given up, and said so. */
    t_now += 600;
    run(); assert(sent(PDU(3, 0, 20, 0, 1)));
    modbus_init(&M, &IO, 1000, 2000);
    run();
    assert(event(3, "lim=1; lost"));
    puts("devctl: ok");
    return 0;
}
```

- [ ] **Step 3: Run to see it fail.** `tests/host.sh 2>&1 | grep -A3 '^devctl'`: `BUILD FAILED`
  (`device_command`, `EV_WRITE` undeclared).

- [ ] **Step 4: events.h and device.h.** In `src/app/events.h` add after `EV_DROPPED,`:

```c
    EV_WRITE,          /* a device's control: port its port; a: device number (0 unknown) | outcome << 8,
                          0 ok, 1 unchanged, 2 refused, 3 failed, 4 reads back otherwise;
                          s: "control=value", then "; " and why */
```

  In `src/app/device.h` add before `device_get`:

```c
/* A command for one of a device's controls (over MQTT): queued to be written (true), or
 * refused, with an event saying why. retained: the broker kept it, so it is stale. */
bool  device_command(const char *name, const char *control, const char *payload, bool retained);
```

- [ ] **Step 5: device.c.** Add `#include "events.h"` and `#include "pfwrite.h"`. After the
  `LOST_MS` define add:

```c
#define WRITES       16          /* commands waiting, for all devices */
#define WRITE_GAP_MS 500u        /* 2 writes a second per device at most */

/* What a request of ours is for (tag bits 24-25); n (bits 0-7) is a poll's block or a write's place. */
enum { TAG_POLL, TAG_READ, TAG_WRITE, TAG_CHECK };
enum { W_FREE, W_QUEUED, W_READING, W_WRITING, W_CHECKING };
enum { WR_OK, WR_UNCHANGED, WR_REFUSED, WR_FAILED, WR_DIFFERS };   /* an EV_WRITE's outcome */
typedef struct {
    uint8_t    stage, dev, gen, field;
    uint8_t    port;                 /* the port it went out on, once read (0-based) */
    uint32_t   seq;                  /* arrival: the oldest goes first */
    pf_write_t w;
    uint16_t   want[2];              /* the registers as written */
    char       what[40];             /* "control=value", for its event */
} wq_t;
static wq_t s_wq[WRITES];
static uint32_t s_wseq, s_wlast[DEVS];
```

  In `device_init()` add `memset(s_wq, 0, sizeof s_wq); memset(s_wlast, 0, sizeof s_wlast); s_wseq = 0;`.
  Add after `used_line()`/`window()`:

```c
/* Every write, and every refused command, in the event log. A note keeps 39 characters, so
 * the device is a number in `a`, with the outcome above it. */
static void wr_event(int i, int outcome, const char *what, const char *more)
{
    char s[80];
    snprintf(s, sizeof s, "%s%s%s", what, more[0] ? "; " : "", more);
    events_add(EV_WRITE, i >= 0 ? s_dev[i].cfg.port : 0, (uint32_t)(i + 1) | (uint32_t)outcome << 8, s);
}

static uint32_t tag_of(int op, const device_t *d, int i, int n) { return (uint32_t)op << 24 | (uint32_t)d->gen << 16 | (uint32_t)i << 8 | (uint32_t)n; }

/* One request of ours into the port's queue; the port is busy until it ends. */
static bool ask(int port, modbus_t *m, int i, int op, int n, const uint8_t *pdu, uint16_t len)
{
    const device_t *d = &s_dev[i];
    if (!m || !modbus_local(m, d->cfg.address, pdu, len, d->p->timeout_ms, tag_of(op, d, i, n))) return false;
    s_port[port].busy = true;
    s_port[port].since = HAL_GetTick();
    return true;
}

/* The port's queue was started again, or a request's end never came: writes on the line are given up. */
static void wr_lost(int port)
{
    for (int k = 0; k < WRITES; k++) {
        wq_t *q = &s_wq[k];
        if (q->stage <= W_QUEUED || q->port != port) continue;       /* its device may be gone: the port it went on */
        wr_event(q->dev, WR_FAILED, q->what, "lost");
        q->stage = W_FREE;
    }
}

/* The registers at addr, from a reply to a read that began at `from`. */
static bool regs_at(const uint8_t *pdu, size_t n, uint16_t from, uint16_t addr, uint8_t regs, uint16_t out[2])
{
    size_t at = 2u + 2u * (size_t)(addr - from);
    if (n < 2 || (pdu[0] & 0x80) || addr < from || at + 2u * regs > n) return false;
    for (int r = 0; r < regs; r++) out[r] = (uint16_t)(pdu[at + 2 * r] << 8 | pdu[at + 2 * r + 1]);
    return true;
}
```

  Split today's `device_reply()` body after its gen check into
  `static void poll_reply(int port, device_t *d, int b, int st, const uint8_t *pdu, size_t n)`
  (from `const pf_block_t *k = &d->p->block[b];` to the end, unchanged) and make `device_reply()`:

```c
void device_reply(int port, uint32_t tag, int st, const uint8_t *pdu, size_t n)
{
    int op = (int)(tag >> 24 & 3u), i = (int)(tag >> 8 & 0xFFu), k = (int)(tag & 0xFFu);
    if (port >= 0 && port < PORTS) s_port[port].busy = false;
    if (i >= DEVS || port < 0 || port >= PORTS) return;
    if (op != TAG_POLL) { write_reply(port, i, (uint8_t)(tag >> 16), op, k, st, pdu, n); return; }
    device_t *d = &s_dev[i];
    if (!d->p || d->gen != (uint8_t)(tag >> 16) || k >= d->p->nblock) return;
    poll_reply(port, d, k, st, pdu, n);
}
```

  with, above it:

```c
/* A write's request ended: read -> written (or unchanged) -> read back. */
static void write_reply(int port, int i, uint8_t gen, int op, int k, int st, const uint8_t *pdu, size_t n)
{
    static const uint8_t AT[] = { W_FREE, W_READING, W_WRITING, W_CHECKING };
    device_t *d = &s_dev[i];
    if (k >= WRITES) return;
    wq_t *q = &s_wq[k];
    if (q->stage != AT[op] || q->dev != i || q->gen != gen) return;        /* not the one asked for */
    if (!d->p || d->gen != gen) { q->stage = W_FREE; return; }               /* its device changed meanwhile */
    const pf_field_t *f = &d->p->field[q->field];
    const pf_block_t *b = &d->p->block[f->block];
    modbus_t *m = bridge_modbus_port(port);
    uint32_t now = HAL_GetTick();
    if (op == TAG_CHECK) {
        uint16_t got[2];
        bool read = st == 0 && regs_at(pdu, n, b->addr, q->w.addr, q->w.regs, got), same = read;
        poll_reply(port, d, f->block, st, pdu, n);
        for (int r = 0; read && r < q->w.regs; r++) same = same && !((got[r] ^ q->want[r]) & q->w.mask[r]);
        char v[24] = "";
        if (read && !same && !device_value(d, q->field, v, sizeof v)) v[0] = 0;
        wr_event(i, read && !same ? WR_DIFFERS : WR_OK, q->what, read ? v : "not read back");
        q->stage = W_FREE;
        return;
    }
    used_line(port, d, st == 0 ? n : 0, st != 0);
    char e[16] = "";
    if (st) snprintf(e, sizeof e, "no reply");
    else if (n >= 2 && (pdu[0] & 0x80)) snprintf(e, sizeof e, "error %u", pdu[1]);
    else if (op == TAG_READ && (n != 2u + 2u * q->w.regs || pdu[0] != 3)) snprintf(e, sizeof e, "bad reply");
    if (e[0]) { wr_event(i, WR_FAILED, q->what, e); q->stage = W_FREE; return; }
    if (op == TAG_READ) {
        uint16_t have[2] = { 0, 0 };
        regs_at(pdu, n, q->w.addr, q->w.addr, q->w.regs, have);
        if (!pf_merge(&q->w, have, q->want)) {
            wr_event(i, WR_UNCHANGED, q->what, "");
            q->stage = W_FREE;
            d->due[f->block] = now;                           /* HA may show an older value: read it */
            return;
        }
        uint8_t w[10] = { 6, (uint8_t)(q->w.addr >> 8), (uint8_t)q->w.addr };
        uint16_t len = 5;
        if (q->w.regs == 1) { w[3] = (uint8_t)(q->want[0] >> 8); w[4] = (uint8_t)q->want[0]; }
        else {
            w[0] = 16; w[3] = 0; w[4] = 2; w[5] = 4;
            w[6] = (uint8_t)(q->want[0] >> 8); w[7] = (uint8_t)q->want[0]; w[8] = (uint8_t)(q->want[1] >> 8); w[9] = (uint8_t)q->want[1];
            len = 10;
        }
        if (!ask(port, m, i, TAG_WRITE, k, w, len)) { wr_event(i, WR_FAILED, q->what, "not sent"); q->stage = W_FREE; return; }
        q->stage = W_WRITING;
        return;
    }
    const uint8_t r[5] = { b->fn, (uint8_t)(b->addr >> 8), (uint8_t)b->addr, (uint8_t)(b->count >> 8), (uint8_t)b->count };
    if (!ask(port, m, i, TAG_CHECK, k, r, 5)) {
        wr_event(i, WR_OK, q->what, "not read back");
        q->stage = W_FREE;
        d->due[f->block] = now;
        return;
    }
    q->stage = W_CHECKING;
}
```

  (`write_reply` calls `poll_reply`, so define `poll_reply` first.) In `submit()` replace the
  `modbus_local(...)` call and the two `s_port[port]` lines after it with
  `if (!ask(port, m, best, TAG_POLL, bb, pdu, 5)) return;   /* 8 waiting already: next pass */`.
  Add before `device_poll()`:

```c
/* The oldest command waiting for this port whose device may write now: its register read
 * first. False if there is none. */
static bool write_next(int port, modbus_t *m, uint32_t now)
{
    int best = -1;
    for (int k = 0; k < WRITES; k++) {
        wq_t *q = &s_wq[k];
        if (q->stage != W_QUEUED) continue;
        const device_t *d = &s_dev[q->dev];
        if (!d->p || d->gen != q->gen) { q->stage = W_FREE; continue; }       /* its device changed */
        if (d->cfg.port != port + 1 || now - s_wlast[q->dev] < WRITE_GAP_MS) continue;
        if (best < 0 || (int32_t)(q->seq - s_wq[best].seq) < 0) best = k;
    }
    if (best < 0) return false;
    wq_t *q = &s_wq[best];
    const uint8_t pdu[5] = { 3, (uint8_t)(q->w.addr >> 8), (uint8_t)q->w.addr, 0, q->w.regs };
    if (!ask(port, m, q->dev, TAG_READ, best, pdu, 5)) return true;      /* the queue is full: next pass */
    q->stage = W_READING;
    q->port = (uint8_t)port;
    s_wlast[q->dev] = now;
    return true;
}

bool device_command(const char *name, const char *control, const char *payload, bool retained)
{
    char what[40], e[32];
    int i = -1, f = -1;
    for (int k = 0; k < DEVS; k++) if (s_dev[k].used && !strcmp(s_dev[k].cfg.name, name)) i = k;
    if (i < 0) {
        snprintf(what, sizeof what, "%s/%s=%s", name, control, payload);
        wr_event(-1, WR_REFUSED, what, "no such device");
        return false;
    }
    device_t *d = &s_dev[i];
    snprintf(what, sizeof what, "%s=%s", control, payload);
    if (d->p) f = profile_field(d->p, control);
    const char *why = retained ? "retained" : !d->cfg.controls ? "controls off" :
                      f < 0 || !pf_control(&d->p->field[f]) ? "no such control" : NULL;
    pf_write_t w;
    if (!why && !pf_command(d->p, f, payload, &w, e, sizeof e)) why = e;
    if (why) { wr_event(i, WR_REFUSED, what, why); return false; }
    int slot = -1;
    for (int k = 0; k < WRITES && slot < 0; k++)              /* the same control waiting: the newer value */
        if (s_wq[k].stage == W_QUEUED && s_wq[k].dev == i && s_wq[k].field == f && s_wq[k].gen == d->gen) slot = k;
    for (int k = 0; k < WRITES && slot < 0; k++)
        if (s_wq[k].stage == W_FREE) { slot = k; s_wq[k].seq = ++s_wseq; }
    if (slot < 0) { wr_event(i, WR_REFUSED, what, "too many waiting"); return false; }
    wq_t *q = &s_wq[slot];
    q->stage = W_QUEUED;
    q->dev = (uint8_t)i;
    q->gen = d->gen;
    q->field = (uint8_t)f;
    q->w = w;
    snprintf(q->what, sizeof q->what, "%s", what);
    return true;
}
```

  In `device_poll()`: in the `if (!m) { ... }` branch add `wr_lost(port);` before
  `s_port[port].busy = false;`. Replace
  `if (s_port[port].busy && !modbus_local_pending(m)) s_port[port].busy = false;   /* dropped by a re-init */`
  with `if (s_port[port].busy && !modbus_local_pending(m)) { s_port[port].busy = false; wr_lost(port); }   /* dropped by a re-init */`,
  and replace the final `s_port[port].busy = false; submit(port, m, now);` with:

```c
        if (s_port[port].busy) wr_lost(port);               /* its end never came */
        s_port[port].busy = false;
        if (!write_next(port, m, now)) submit(port, m, now);
```

  Update device.c's header comment: "... Commands for controls are written before polls: the
  register read, written if that changes it, the block read back."

- [ ] **Step 6: The event on the page.** In `web/index.html` `evText`, before `default:` add:

```js
 case 20:{const d=a&255,o=a>>8,i=s.indexOf("; "),w=i<0?s:s.slice(0,i),m=i<0?"":s.slice(i+2),who=d?"Device "+d:"A device";
  return o===0?who+" set "+w+(m?" ("+m+")":""):o===1?who+": "+w+", already so":o===2?who+": "+w+" refused"+(m?", "+m:""):o===3?who+": "+w+" failed"+(m?", "+m:""):who+": "+w+" written, reads back "+m}
```

  and extend `evBad` with `||(e[4]===20&&e[6]>>8>=2)`.

- [ ] **Step 7: Run.** `tests/host.sh`: all ok, `devctl: ok` among them. `test_device`'s
  `tag()` helper (op 0 in bits 24-25) still matches polls.

- [ ] **Step 8: Commit.** Message "Devices: commands written to controls, read first and back,
  each in the event log".

---

### Task 6: MQTT and Home Assistant, and /api/devices in parts

**Files:**
- Modify: `src/app/devmqtt.c`, `src/app/mqtt_ha.c`, `src/net/lwipopts.h`, `src/app/device.h`,
  `src/app/device.c` (`device_json`), `src/app/api.c` (`api_devices`)
- Test: `tests/test_devctl.c` (adds `src/app/devmqtt.c` to its host.sh line), `tests/test_devmqtt.c`

**Interfaces:**
- Consumes: `device_command()` (Task 5), `pf_control()`, `PFF_HIDDEN` (Task 3).
- Produces: `#define DEV_PARTS 3`; `int device_json_part(int i, int k, char *out, size_t max);`
  (-1 if it does not fit; part 0: settings, state and values 0-39; part 1: values 40-79; part 2:
  the rest and the closing `]}`); `device_json()` is their concatenation.

- [ ] **Step 1: Write the failing tests.** In `tests/host.sh` add `src/app/devmqtt.c` to the
  `devctl` line. In `tests/test_devctl.c` add `#include "devmqtt.h"` and
  `#include "../tests/fuzz/json_ok.h"`, the publisher below after `val()`, and before
  `puts("devctl: ok");` the checks that follow it:

```c
static struct { char topic[160], payload[8192]; } Pb[64];
static int Pn;
static bool pub(const char *t, const char *p, bool r)
{
    (void)r;
    int k = 0;
    while (k < Pn && strcmp(Pb[k].topic, t)) k++;              /* the latest for each topic */
    if (k == Pn) { assert(Pn < 64); Pn++; }
    snprintf(Pb[k].topic, sizeof Pb[k].topic, "%s", t);
    snprintf(Pb[k].payload, sizeof Pb[k].payload, "%s", p);
    return true;
}
static void drain(void) { for (int k = 0; k < 400; k++) assert(devmqtt_poll(pub, "solar", "n540-test", true, t_now)); }
static const char *got(const char *t) { for (int k = 0; k < Pn; k++) if (!strcmp(Pb[k].topic, t)) return Pb[k].payload; return NULL; }
```

```c
    /* Discovery: each control as its HA kind, with its command topic; a hidden one disabled.
     * From a clean slate: every device gone (their waiting commands with them), one back. */
    memset(c.dev, 0, sizeof c.dev);
    device_configure(&c);
    c.dev[0] = (dev_cfg_t){ "c", "inv", "", "", 2, 1, 1, 0 };
    device_configure(&c);
    modbus_init(&M, &IO, 1000, 2000);                           /* the write on the line goes with it */
    devmqtt_init();
    drain();
    const char *n = got("homeassistant/number/n540-test_inv/lim/config"), *s;
    assert(n && json_ok(n) && strstr(n, "\"command_topic\":\"~/set/lim\"") && strstr(n, "\"entity_category\":\"config\"") &&
           strstr(n, "\"min\":0.0,\"max\":25.0,\"step\":0.1,\"mode\":\"box\"") && strstr(n, "\"unit_of_measurement\":\"kW\""));
    s = got("homeassistant/switch/n540-test_inv/ac/config");
    assert(s && json_ok(s) && strstr(s, "{{ 'ON' if value_json['ac'] else 'OFF' }}") && strstr(s, "\"command_topic\":\"~/set/ac\""));
    s = got("homeassistant/text/n540-test_inv/t1/config");
    assert(s && json_ok(s) && strstr(s, "\"pattern\":\"^([01][0-9]|2[0-3]):[0-5][0-9]$\",\"min\":5,\"max\":5"));
    s = got("homeassistant/select/n540-test_inv/by/config");
    assert(s && json_ok(s) && strstr(s, "\"options\":[\"Off\",\"Time\",\"Voltage\",\"SOC\"]") && strstr(s, "\"enabled_by_default\":false"));
    s = got("homeassistant/sensor/n540-test_inv/v/config");
    assert(s && !strstr(s, "command_topic") && !strstr(s, "enabled_by_default"));
    run(); assert(sent(PDU(4, 0, 0, 0, 2))); answer(PDU(4, 4, 0, 1, 0, 2), 6);
    run(); assert(sent(SET_READ)); set_reply(60, 0xF3D5, 0x0008, 70000, 4);
    t_now += 2000; drain();
    s = got("solar/dev/inv/state");
    assert(s && json_ok(s) && strstr(s, "\"lim\":6.0") && strstr(s, "\"ac\":true") && strstr(s, "\"t1\":\"08:00\"") && strstr(s, "\"by\":\"Voltage\""));
    /* Controls off: their configs emptied, the sensor's kept. */
    c.dev[0].controls = 0;
    device_configure(&c);
    drain();
    assert(!strcmp(got("homeassistant/number/n540-test_inv/lim/config"), "") && !strcmp(got("homeassistant/select/n540-test_inv/by/config"), ""));
    assert(got("homeassistant/sensor/n540-test_inv/v/config")[0] == '{');
    /* /api/devices: a device in parts, whole JSON together. */
    static char j[16384];
    int w0 = device_json_part(0, 0, j, sizeof j);
    assert(w0 > 0 && device_json_part(0, 2, j + w0, sizeof j - (size_t)w0) == 2 && !strcmp(j + w0, "]}"));
    assert(device_json(0, j, sizeof j) && json_ok(j) && strstr(j, "[\"lim\",\"Limit\",6.0,\"kW\"]"));
```

  (The device has 6 values, so parts 1 and 2 hold none of them; part 2 still closes the object.)

  In `tests/test_devmqtt.c`: change `Pb[400]`'s `payload[4096]` to `payload[8192]`; in the worst
  case fill, make the first branch `x->kind == PF_SENSOR || x->kind == PF_CALC || x->kind == PF_NUMBER`,
  the enum branch `x->kind == PF_ENUM || x->kind == PF_SELECT`, and add before its final `else`:
  `} else if (x->kind == PF_TIME) { w->st.val[f].raw = 0x3B17;`. Replace the `strlen(s) < 3584`
  bound with `strlen(s) < 7680` and the device_json check with:

```c
        int jmax = 0;
        for (int k = 0; k < DEV_PARTS; k++) { int w2 = device_json_part(0, k, j, sizeof j); assert(w2 >= 0); if (w2 > jmax) jmax = w2; }
        static char jall[3 * 5622];
        size_t jn = device_json(0, jall, sizeof jall);
        assert(jn && json_ok(jall));
        printf("worst case %s: state %zu bytes (BUF 7680), largest /api/devices part %d (5621)\n", profiles_id(pi), strlen(s), jmax);
```

  and at the end `assert(max_cfg < 7680 && max_state < 7680);` with its printf saying `(BUF 7680)`.

- [ ] **Step 2: Run to see them fail.** `tests/host.sh 2>&1 | grep -E -A3 '^(devctl|devmqtt)'`:
  `BUILD FAILED` (`device_json_part`, `DEV_PARTS`).

- [ ] **Step 3: Buffers.** `src/net/lwipopts.h`: `#define MQTT_OUTPUT_RINGBUF_SIZE 8192   /* a device's state, up to 7.5 KB (devmqtt.c), and the rest */`.
  `src/app/devmqtt.c`: `#define BUF 7680   /* a device's state or one config: within lwIP MQTT's 8192-byte output ring, with topic and header */`.

- [ ] **Step 4: Discovery.** In `devmqtt.c` replace `comp()`:

```c
static const char *comp(const pf_field_t *f)
{
    switch (f->kind) {
    case PF_BINARY: case PF_CALCBIN: return "binary_sensor";
    case PF_NUMBER: return "number";
    case PF_SELECT: return "select";
    case PF_SWITCH: return "switch";
    case PF_TIME:   return "text";
    default:        return "sensor";
    }
}
```

  In `config()`: `bool bin = f->kind == PF_BINARY || f->kind == PF_CALCBIN || f->kind == PF_SWITCH;`;
  `if (f->sc && !pf_control(f))` for the state class; `if ((f->flags & PFF_DIAG) && !pf_control(f))`
  for the diagnostic category; and before the `,"device":{...}` line add:

```c
    if (pf_control(f)) {
        ADD(",\"command_topic\":\"~/set/%s\",\"entity_category\":\"config\"", id);
        if (f->flags & PFF_HIDDEN) ADD(",\"enabled_by_default\":false");
        if (f->kind == PF_NUMBER) {
            char lo[24], hi[24], st[24];
            int pr = pf_prec(f);
            if (!pf_num(lo, sizeof lo, f->lo, pr) || !pf_num(hi, sizeof hi, f->hi, pr) || !pf_num(st, sizeof st, f->k, pr)) return 0;
            ADD(",\"min\":%s,\"max\":%s,\"step\":%s,\"mode\":\"box\"", lo, hi, st);
        } else if (f->kind == PF_SELECT) {
            ADD(",\"options\":[");
            for (int k = 0; k < f->nvname; k++) {
                char o[96];
                if (!json_str(pf_str(p, p->vname[f->vname + k].name), o, sizeof o)) return 0;
                ADD("%s%s", k ? "," : "", o);
            }
            ADD("]");
        } else if (f->kind == PF_TIME) {
            ADD(",\"pattern\":\"^([01][0-9]|2[0-3]):[0-5][0-9]$\",\"min\":5,\"max\":5");
        }
    }
```

  In `devmqtt_poll()`'s discovery branch replace the three lines from `size_t n = announce ? ...`
  through `if (!pub(topic, announce ? s_buf : "", true)) return false;` with:

```c
            bool show = announce && (!pf_control(f) || d->cfg.controls);      /* controls off: emptied */
            size_t n = show ? config(s_buf, sizeof s_buf, d, s_d[i].disc, base, host) : 0;
            if (show && !n) { s_d[i].disc++; return true; }                  /* will not fit: left out */
            if (!pub(topic, show ? s_buf : "", true)) return false;
```

- [ ] **Step 5: Commands.** In `src/app/mqtt_ha.c` add `#include "device.h"` if absent; beside
  `s_in_ok` declare `static bool s_in_dev, s_in_retained; static char s_dev_name[25], s_dev_ctl[25];`;
  beside `s_sub` declare `s_dsub` (reset to false at each place `s_sub = false` is, lines 174, 263,
  273). In `cb_topic()` replace its first checks (`if (!s_run.control || (s_c.rx_buffer[0] & 1u)) return;`,
  `if (tot >= sizeof s_in_val) return;`, `if (strncmp(topic, s_base, n) || topic[n] != '/') return;`) with:

```c
    s_in_dev = false;
    if (tot >= sizeof s_in_val || strncmp(topic, s_base, n) || topic[n] != '/') return;
    if (!strncmp(topic + n, "/dev/", 5)) {                     /* BASE/dev/<name>/set/<control>: a device's control */
        const char *nm = topic + n + 5, *set = strstr(nm, "/set/");
        size_t nl = set ? (size_t)(set - nm) : 0, cl = set ? strlen(set + 5) : 0;
        if (!nl || nl >= sizeof s_dev_name || !cl || cl >= sizeof s_dev_ctl || strchr(set + 5, '/')) return;
        memcpy(s_dev_name, nm, nl);
        s_dev_name[nl] = 0;
        memcpy(s_dev_ctl, set + 5, cl + 1);
        s_in_retained = s_c.rx_buffer[0] & 1u;
        s_in_ok = s_in_dev = true;
        return;
    }
    if (!s_run.control || (s_c.rx_buffer[0] & 1u)) return;      /* retained: stale */
```

  In `cb_data()`, right after the `if (strlen(s_in_val) != s_in_len || strpbrk(s_in_val, "\r\n")) return;`
  line add `if (s_in_dev) { device_command(s_dev_name, s_dev_ctl, s_in_val, s_in_retained); return; }`.
  In the poll function, after the `if (!s_sub && s_run.control) { ... }` block add:

```c
    if (!s_dsub) {                              /* devices' controls; device.c refuses those not on */
        snprintf(topic, sizeof topic, "%s/dev/+/set/+", s_base);
        if (mqtt_subscribe(&s_c, topic, 0, NULL, NULL) != ERR_OK) return;
        s_dsub = true;
    }
```

- [ ] **Step 6: /api/devices in parts.** In `src/app/device.h` replace the `device_json`
  declaration with:

```c
#define DEV_PARTS 3                 /* /api/devices gives a device in this many parts */
/* Part k of device i's JSON: 0 its settings and state and values 0 to 39; 1 values 40 to 79;
 * 2 the rest, and the end. -1 if it does not fit. A device gone, or without a profile, has
 * nothing in parts 1 and 2 but the end. */
int   device_json_part(int i, int k, char *out, size_t max);
size_t device_json(int i, char *out, size_t max);   /* all of it; 0 if it does not fit */
```

  In `src/app/device.c` replace `device_json()` with:

```c
#define PART_VALUES 40

int device_json_part(int i, int k, char *out, size_t max)
{
    const device_t *d = device_get(i);
    size_t n = 0;
    char s[3][80];
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= max - n) { return -1; } n += (size_t)w_; } while (0)
    if (!max) return -1;
    out[0] = 0;
    if (k == 0) {
        if (!d) return -1;
        if (!json_str(d->cfg.label, s[0], sizeof s[0]) || !json_str(d->error, s[1], sizeof s[1]) || !json_str(d->why, s[2], sizeof s[2])) return -1;
        OUT("{\"n\":%d,\"profile\":\"%s\",\"name\":\"%s\",\"label\":%s,\"port\":%u,\"address\":%u,\"controls\":%s,"
            "\"ids\":\"%s\",\"prefix\":\"%s\",\"error\":%s,\"online\":%s,\"polls\":%lu,\"misses\":%lu,\"why\":%s,\"ago_ms\":%ld",
            i + 1, d->cfg.profile, d->cfg.name, s[0], d->cfg.port, d->cfg.address, d->cfg.controls ? "true" : "false",
            d->cfg.ids ? "esphome" : "own", d->cfg.prefix, s[1], d->online ? "true" : "false",
            (unsigned long)d->polls, (unsigned long)d->misses, s[2],
            d->last_ok ? (long)((HAL_GetTick() - d->last_ok) & 0x7FFFFFFF) : -1L);
        if (!d->p) { OUT("}"); return (int)n; }
        if (!json_str(pf_str(d->p, d->p->name), s[0], sizeof s[0]) || !json_str(pf_str(d->p, d->p->maker), s[1], sizeof s[1])) return -1;
        OUT(",\"model\":%s,\"maker\":%s,\"values\":[", s[0], s[1]);
    }
    int from = k * PART_VALUES, to = !d || !d->p ? 0 : k == DEV_PARTS - 1 ? d->p->nfield : from + PART_VALUES;
    for (int f = from; d && d->p && f < to && f < d->p->nfield; f++) {
        const pf_field_t *x = &d->p->field[f];
        char v[520], nm[96], u[40];
        if (!device_value(d, f, v, sizeof v)) return -1;
        if (!json_str(pf_str(d->p, x->name), nm, sizeof nm) || !json_str(pf_str(d->p, x->unit), u, sizeof u)) return -1;
        OUT("%s[\"%s\",%s,%s,%s]", f ? "," : "", pf_str(d->p, x->id), nm, v, u);
    }
    if (k == DEV_PARTS - 1) OUT("]}");
    return (int)n;
#undef OUT
}

size_t device_json(int i, char *out, size_t max)
{
    size_t n = 0;
    for (int k = 0; k < DEV_PARTS; k++) {
        int w = device_json_part(i, k, out + n, max - n);
        if (w < 0) return 0;
        n += (size_t)w;
        if (k == 0 && !device_get(i)->p) break;              /* closed already */
    }
    return n;
}
```

  In `src/app/api.c` `api_devices()` replace the `part <= DEVS` and `DEVS + 1` branches with:

```c
    static bool open;                                   /* the device's values were left open by its first part */
    if (part <= DEVS * DEV_PARTS) {
        int i = (int)(part - 1) / DEV_PARTS, k = (int)(part - 1) % DEV_PARTS;
        if (!(seen >> i & 1)) return 0;
        if (k) { if (!open) return 0; int w = device_json_part(i, k, out, max); return w < 0 ? -2 : w; }
        if (!device_get(i)) return -2;                  /* removed since part 0: fail, not malformed JSON */
        if (seen & ((1u << i) - 1)) OUT(",");
        int w = device_json_part(i, 0, out + n, max - n);
        if (w < 0) return -2;
        open = device_get(i)->p != NULL;
        return (int)n + w;
    }
    if (part == DEVS * DEV_PARTS + 1) { OUT("]}\n"); return (int)n; }
```

  (Declare `open` beside `seen`.)

- [ ] **Step 7: Run.** `tests/host.sh` all ok; note the printed worst cases. `tests/sim/run.sh`
  and `curl -s http://127.0.0.1:8541/api/devices | python3 -m json.tool` parses.

- [ ] **Step 8: Docs.** In `docs/GATEWAY.md` Devices section: the `devN.controls` row becomes
  "0 or 1: announce the profile's controls to Home Assistant and accept commands for them
  (below)"; the Topics table gains
  `| BASE/dev/<name>/set/<control> | a command: a number, a choice's name, ON or OFF, or HH:MM. Not retained |`
  and its discovery row reads `homeassistant/<sensor, binary_sensor, number, select, switch or text>/HOST_<name>/<field>/config`;
  replace "### Limits in phase 1" with:

```markdown
### Controls

A profile's controls (number, select, switch, time) are settings the device keeps. With
`devN.controls=1` they are announced to Home Assistant and commands for them are obeyed;
with 0 they are still read, and shown as values, but not announced. A command is checked
against the control (its range, its choices), then waits its turn on the port ahead of the
polls, at most 2 a second per device. The gateway reads the register first and writes it
only if that changes it, then reads the control's block back, so Home Assistant shows what
the device holds. Each command is in the event log: written, already so, refused and why,
failed (no reply, the device's error), or written but reading back otherwise.

Do not send settings on a timer. A device may keep them in memory that wears with writing;
the gateway skips a write that changes nothing, but a value that alternates on a schedule
is written each time.

### Limits

- Built-in profiles only; no uploads yet.
- A device has up to about 110 values: its state is one MQTT message of up to 7.5 KB.
- Changing the base topic leaves a device's retained topics under the old base.
- Turning MQTT off leaves the devices' entities in Home Assistant.
```

  In `docs/superpowers/specs/2026-11-27-device-controls-design.md` replace "a device goes in two
  parts, its first 60 values and then the rest" with "a device goes in three parts of 40
  values (the last takes the rest)".

- [ ] **Step 9: Commit.** Message "MQTT: commands for devices' controls, and HA discovery for
  them; /api/devices in parts".

---

### Task 7: The EG4 profile's settings and controls

**Files:**
- Rename: `profiles/eg4-18kpv.txt` -> `profiles/eg4-hybrid.txt` (`git mv`)
- Modify: it; `tests/test_builtin.c`; `profiles/README.md`; `README.md:23-24`
- Test: `tests/test_builtin.c`, `tests/test_devmqtt.c` (its worst case covers it)

- [ ] **Step 1: Write the failing test.** In `tests/test_builtin.c` change
  `profiles_text("eg4-18kpv", &n)` to `profiles_text("eg4-hybrid", &n)`, and add before
  `puts("builtin: ok");`:

```c
    /* Settings, from registers read from a FlexBOSS on 2026-11-27 (values only). */
    static const uint16_t H40[40] = { 16, 2640, 5850, 6120, 30000, 30000, 5650, 6200, 16, 16, 5650, 6200, 16, 16, 44, 2352,
        2208, 2448, 2592, 0, 100, 0, 1000, 1000, 100, 100, 120, 100, 1, 3, 11, 16, 0, 0, 120, 100, 0, 15127, 0, 0 };
    static const uint16_t H80[40] = { 0, 0, 120, 20, 0, 0, 0, 0, 0, 0, 240, 60, 0, 0, 0, 0, 5, 300, 0, 565,
        490, 250, 250, 100, 0, 10, 65336, 550, 0, 400, 1065, 0, 1, 1, 0, 60036, 100, 65486, 500, 0 };
    static const uint16_t H120[40] = { 4, 0, 0, 0, 6304, 0, 0, 0, 0, 0, 0, 0, 0, 0, 59964, 5696, 20, 1420, 0, 0,
        0, 0, 0, 0, 550, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 540, 560 };
    static const uint16_t H160[40] = { 20, 0, 0, 0, 0, 0, 0, 0, 0, 500, 0, 0, 0, 0, 0, 0, 65535, 120, 0, 4160,
        100, 2544, 2640, 10, 20, 2400, 300, 0, 44, 0, 50, 100, 100, 20, 400, 560, 10, 100, 60, 0 };
    id[21] = 0xF355;
    assert(pf_decode(&P, profile_block(&P, "ident"), d, regs(d, id, 40), &S));
    assert(pf_decode(&P, profile_block(&P, "set40"), d, regs(d, H40, 40), &S));
    assert(pf_decode(&P, profile_block(&P, "set80"), d, regs(d, H80, 40), &S));
    assert(pf_decode(&P, profile_block(&P, "set120"), d, regs(d, H120, 40), &S));
    assert(pf_decode(&P, profile_block(&P, "set160"), d, regs(d, H160, 40), &S));
    assert(near(V("ac_charge_power"), 12) && near(V("ac_charge_soc_limit"), 100) && near(V("charge_first_power"), 12));
    assert(!strcmp(text("ac_charge_start_1"), "01:00") && !strcmp(text("ac_charge_end_1"), "03:00"));
    assert(!strcmp(text("ac_charge_start_2"), "11:00") && !strcmp(text("ac_charge_end_2"), "16:00"));
    assert(!strcmp(text("charge_first_end_1"), "23:59") && !strcmp(text("charge_first_start_3"), "00:00"));
    assert(near(V("forced_discharge_power"), 12) && near(V("forced_discharge_soc"), 20));
    assert(near(V("charge_current"), 250) && near(V("discharge_current"), 250) && near(V("export_limit"), 100));
    assert(near(V("discharge_cutoff_soc"), 10) && near(V("off_grid_cutoff_soc"), 0) && near(V("ac_charge_start_soc"), 20));
    assert(!strcmp(text("ac_charge_by"), "Voltage") && !strcmp(text("discharge_control"), "Voltage") && !strcmp(text("output_priority"), "Battery first"));
    assert(V("ac_charge") == 0 && V("charge_first") == 0 && V("forced_discharge") == 0 && V("export") == 1 && V("off_grid_output") == 1 && V("power") == 1);
    assert((P.field[F("power")].flags & PFF_HIDDEN) && (P.field[F("off_grid_output")].flags & PFF_HIDDEN) && !(P.field[F("export")].flags & PFF_HIDDEN));
    /* ... and a command for one: its bit alone. */
    pf_write_t w;
    char why[32];
    uint16_t have[2] = { 0xF355, 0 }, out[2];
    assert(pf_command(&P, F("ac_charge"), "ON", &w, why, sizeof why) && w.addr == 21 && pf_merge(&w, have, out) && out[0] == 0xF3D5);
    assert(pf_command(&P, F("ac_charge_power"), "8.5", &w, why, sizeof why) && w.addr == 66 && w.value[0] == 85);
    assert(!pf_command(&P, F("ac_charge_power"), "26", &w, why, sizeof why));
```

  Add `#include "pfwrite.h"` at the top, and `src/app/pfwrite.c` to the `builtin` line in
  `tests/host.sh`. (`id` is the holding 0-39 array already in the test; `F`, `V`, `text`,
  `regs`, `near` are its helpers.)

- [ ] **Step 2: Run to see it fail.** `tests/host.sh 2>&1 | grep -A3 '^builtin'`: an assertion
  (no profile `eg4-hybrid`).

- [ ] **Step 3: The profile.** `git mv profiles/eg4-18kpv.txt profiles/eg4-hybrid.txt`. In it:
  - Replace the header comment's last two sentences ("That document frames ... Not yet tried
    here." and "Left out, for the room ... and settings.") with:
    "That document frames messages with the serial number in them, as the Wi-Fi dongle does;
    over RS485 the inverters answer plain Modbus RTU, to unit 0 and to their own address
    (holding 15, 1 by default): seen on two FlexBOSS, 2026-11-27. The FlexBOSS reads some
    settings beyond the document's ranges: charge and discharge current to 250 A, and the
    three powers (66, 74, 82) in tenths of a kW where it says percent. Left out: the battery's
    own figures as its BMS reports them (80-119), the generator and each EPS leg (120-138),
    arc fault detection (140-152), the power factor (19), and the settings listed at the end."
  - `profile eg4-18kpv 1` -> `profile eg4-hybrid 1`; `name "18kPV"` -> `name "18kPV / FlexBOSS"`.
  - `block ident  hold  0  40 15m info` -> `block ident  hold  0  40 1m info` and after it:

```
block set40  hold  40  40 1m info
block set80  hold  80  40 1m info
block set120 hold  120 40 1m info
block set160 hold  160 40 1m info
```

  - At the end of the file:

```
# Settings: written from Home Assistant when the device has controls=1. Read once a minute,
# and again at once after a write. A write that would change nothing is not made. Left out:
# register 11 (restore defaults, clear records, reboot), grid protection (25-63), the clock,
# PV input mode, parallel and lead-acid settings, the 0.1 % settings (138-143), AC-first
# windows (152-157), generator charging (194-198).
switch ac_charge               "AC charge"                  ident  21  bit 7
number ac_charge_power         "AC charge power"            set40  66  u16 *0.1 min 0 max 25 kW dc=power
number ac_charge_soc_limit     "AC charge SOC limit"        set40  67  u16 min 0 max 100 %
number ac_charge_start_soc     "AC charge start SOC"        set160 160 u16 min 0 max 90 %
number ac_charge_stop_soc      "AC charge stop SOC"         set160 161 u16 min 20 max 100 %
number ac_charge_current       "AC charge current"          set160 168 u16 min 0 max 250 A dc=current
select ac_charge_by            "AC charge by"               set120 120 mask=0x000E 0=Off 2=Time 4=Voltage 6=SOC
time   ac_charge_start_1       "AC charge 1 start"          set40  68
time   ac_charge_end_1         "AC charge 1 end"            set40  69
time   ac_charge_start_2       "AC charge 2 start"          set40  70
time   ac_charge_end_2         "AC charge 2 end"            set40  71
time   ac_charge_start_3       "AC charge 3 start"          set40  72
time   ac_charge_end_3         "AC charge 3 end"            set40  73
switch charge_first            "Charge first"               ident  21  bit 11
number charge_first_power      "Charge first power"         set40  74  u16 *0.1 min 0 max 25 kW dc=power
number charge_first_soc_limit  "Charge first SOC limit"     set40  75  u16 min 0 max 100 %
time   charge_first_start_1    "Charge first 1 start"       set40  76
time   charge_first_end_1      "Charge first 1 end"         set40  77
time   charge_first_start_2    "Charge first 2 start"       set40  78
time   charge_first_end_2      "Charge first 2 end"         set40  79
time   charge_first_start_3    "Charge first 3 start"       set80  80
time   charge_first_end_3      "Charge first 3 end"         set80  81
switch forced_discharge        "Forced discharge"           ident  21  bit 10
number forced_discharge_power  "Forced discharge power"     set80  82  u16 *0.1 min 0 max 25 kW dc=power
number forced_discharge_soc    "Forced discharge SOC limit" set80  83  u16 min 0 max 100 %
time   forced_discharge_start_1 "Forced discharge 1 start"  set80  84
time   forced_discharge_end_1  "Forced discharge 1 end"     set80  85
time   forced_discharge_start_2 "Forced discharge 2 start"  set80  86
time   forced_discharge_end_2  "Forced discharge 2 end"     set80  87
time   forced_discharge_start_3 "Forced discharge 3 start"  set80  88
time   forced_discharge_end_3  "Forced discharge 3 end"     set80  89
number charge_current          "Charge current limit"       set80  101 u16 min 0 max 250 A dc=current
number discharge_current       "Discharge current limit"    set80  102 u16 min 0 max 250 A dc=current
number export_limit            "Export limit"               set80  103 u16 min 0 max 100 %
number discharge_cutoff_soc    "On-grid cut-off SOC"        set80  105 u16 min 10 max 90 %
number off_grid_cutoff_soc     "Off-grid cut-off SOC"       set120 125 u16 min 0 max 90 %
select discharge_control       "Discharge control"          set120 120 mask=0x0030 0=Voltage 16=SOC 32=Both
switch export                  "Export to grid"             ident  21  bit 15
switch off_grid_output         "Off-grid output"            ident  21  bit 0 hidden
switch power                   "Power"                      ident  21  bit 9 hidden
select output_priority         "Output priority"            set120 145 0="Battery first" 1="PV first" 2="AC first"
```

- [ ] **Step 4: Run.** `tests/host.sh`: `builtin: ok`, and `devmqtt` prints the eg4-hybrid worst
  case under 7680 bytes with every part under 5621. Then `tests/fuzz/run.sh 20 command profile`.

- [ ] **Step 5: Docs.** `profiles/README.md`: add a "Controls" section after the statements
  table:

```markdown
## Controls

Settings a device keeps, which Home Assistant can change when the device has
`controls=1`. Each is read from a `hold` block, as a sensor is, and written to the same
address: one register by function 6, two by function 16. A write reads the register first,
changes only the control's own bits, and is not made if nothing would change.

| Statement | Form |
|---|---|
| `number` | `id "name" block address type [*scale] [+offset] min M max M [step S] [unit] [dc=] [hidden]`; type `u16 s16 u32 s32 u32le s32le`; min and max must fit the register; the step is the scale unless given |
| `select` | `id "name" block address [mask=M] v=name ... [hidden]`; each value as it is in the register under the mask; names unique |
| `switch` | `id "name" block address bit N [hidden]`, or `on V off V` for a whole register |
| `time` | `id "name" block address [hidden]`: HH:MM, the hour in the low byte, the minute in the high |

`hidden`: announced disabled, so it must be enabled in Home Assistant on purpose.
```

  In its "Built in" table and section rename `eg4-18kpv` to `eg4-hybrid` ("EG4 18kPV and
  FlexBOSS hybrid inverters", settings: yes), and say it was read on two FlexBOSS. In
  `README.md` lines 23-24 replace "the EG4 18kPV inverter (not yet tried on one)" with "the EG4
  18kPV and FlexBOSS inverters, whose settings Home Assistant can change". Search for any other
  `eg4-18kpv`: `git grep -n eg4-18kpv` must print nothing but the fuzz seed file, if any.

- [ ] **Step 6: Commit.** Message "Profile: eg4-hybrid, the EG4 inverters' settings as controls".

---

### Task 8: On the gateway, with the user present

No code. Stop and ask before each numbered step that touches the gateway, the Pi or MQTT.

- [ ] **Step 1: Build and check.** Clean tree; reconfigure and build (as in Task 2 Step 9); the
  loader check; `tests/host.sh`; `tests/fuzz/run.sh 60`.
- [ ] **Step 2: Install** with the user's go: `python3 tools/n540.py <gateway> update build/app.bin`;
  after 35 s: crashes 0; port 1 listening, `serial_tx` 0, both clients; MQTT connected; ports 2
  and 3 (if switched at Checkpoint A) still serving SolarAssistant.
- [ ] **Step 3: The write path without a device, and without the real broker.** With the user's
  go: start a broker on this Mac, `mosquitto -p 1883 -v`; note the gateway's `mqtt.host`,
  `mqtt.port`, `mqtt.tls`; set them to this Mac's address, 1883, 0; set port 4 to
  `enabled=1 mode=modbus loopback=1 baud=19200`; add `dev9.profile=eg4-hybrid dev9.name=test
  dev9.port=4 dev9.address=1 dev9.controls=1`. Then
  `mosquitto_pub -t <base>/dev/test/set/ac_charge_soc_limit -m 80`: the event log
  (`/api/events`) shows device 9's command failed with "no reply", and port 4's `serial_tx`
  rose. `mosquitto_sub -t 'homeassistant/number/#' -C 1` shows a number config with
  `command_topic`. Then remove `dev9`, restore port 4 (disabled, server, 9600, loopback 0), and
  restore `mqtt.host`, `mqtt.port`, `mqtt.tls`; MQTT reconnects to the real broker.
- [ ] **Step 4: The inverters, with the user watching.** With ports 2 and 3 in Modbus mode
  (Checkpoint A): add each inverter, controls off first, e.g. `dev1.profile=eg4-hybrid
  dev1.name=flexboss_a dev1.label="FlexBOSS A" dev1.port=2 dev1.address=1` and the same for
  port 3 as `flexboss_b`. Over five minutes both are online, SolarAssistant still reads, and
  `/api/devices` values agree with SolarAssistant's. Compare the settings with the EG4 app,
  the three powers in kW among them; if one disagrees, stop and fix the profile first.
- [ ] **Step 5: One write.** With the user's go: `dev1.controls=1`; in Home Assistant set
  FlexBOSS A's "AC charge SOC limit" up by 1. The event log says it was written, the entity
  shows the new value within a few seconds, and the EG4 app agrees. Set it back the same way.
- [ ] **Step 6: Record.** Update `docs/GATEWAY.md` Tests and `profiles/README.md` with what was
  tried and when, and `README.md`'s profile line if needed; commit; push only if the user says.

---

## Self-review

- Spec coverage: RTU over TCP and `answer` (Tasks 1-2); control statements and decoding
  (3); commands into writes, the skip when unchanged (4, 5); queue, 2 a second, read-back,
  events (5); buffers, commands, discovery, `controls=0`, `/api/devices` parts (6); the EG4
  profile and its 41 controls, hidden ones, ranges, rename (7); tests listed in the spec (each
  task's tests, fuzz targets `modbus`, `profile`, `command`); gateway tests (Checkpoint A, 8).
  Changing settings from the page, buttons, SRNE/SmartGen profiles, parallel copying and the
  traffic view are out of this phase, as the spec says.
- Names used across tasks: `MB_BC_ANSWER`, `modbus_set_rtu`, `cfg_modbus_unit0`, `cfg_modbus_rtu`,
  `pf_control`, `PFF_HIDDEN`, `pf_write_t`, `pf_command`, `pf_merge`, `device_command`,
  `EV_WRITE`, `DEV_PARTS`, `device_json_part`: each defined once, in the task that produces it.
