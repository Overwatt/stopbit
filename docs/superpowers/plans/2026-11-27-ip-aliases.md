# Extra IP Addresses Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A port can listen on an IPv4 address of its own (`portN.ip`), which the gateway takes on beside its main address, so two ports can both be on 502.

**Architecture:** A self-contained module, `src/ipalias/`, uses only lwIP: one extra `struct netif` per address, sharing the main interface's MAC and driver, up but never with its link up (so lwIP routes everything out through the main interface), ARP frames sorted to it by address, and lwIP's ACD checking each address first. `src/app/addrs.c` turns the ports' addresses into the module's list and its callbacks into events. The settings, the bridge's binds, `net.c`, the web page and the docs carry the rest.

**Tech Stack:** C (arm-none-eabi for the gateway, the host compiler for tests), lwIP 2.2 raw API, CMake/Ninja, shell test runners, plain JavaScript in `web/index.html`.

**Spec:** `docs/superpowers/specs/2026-11-27-ip-aliases-design.md`

## Global Constraints

- The loader stays byte-identical. `cmp -l build/loader_flash.bin "$LOADER_BASE"` may differ only at decimal offsets 680-703 (the version text). `$LOADER_BASE` is the loader built from `main` before this work; the controller gives its path. Everything in `src/net` and `lwipopts.h` is built into the loader too, so changes to `net.c` go inside `#ifdef APP_IMAGE`, and new code lives outside `src/net`.
- Nothing contacts a real gateway. No `tools/n540.py` against a real address, no `install.sh`, no flashing. The gateway work at the end is the user's, with them present.
- The repository is public. Examples and tests use 10.20.x.x or 10.0.0.x addresses and `02:00:00:…` MACs. Write no real LAN addresses, host names, serial numbers, e-mail addresses or home-directory paths.
- Every commit message ends with these two lines:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK`
- The gateway's printf has no 64-bit numbers: no `%ll` anywhere (tests/host.sh checks).
- Comments and docs: short, plain and formal, in the style of the files around them.
- Tests: `tests/host.sh` passes after every task. `tests/ipalias/run.sh` (from Task 1) passes after every task that touches it. It needs lwIP's source, which configuring the firmware build fetches: `cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake` (once).

---

### Task 1: The module, `src/ipalias`, and its test against real lwIP

**Files:**
- Create: `src/ipalias/ipalias.h`, `src/ipalias/ipalias.c`
- Create: `tests/ipalias/lwipopts.h`, `tests/ipalias/arch/cc.h`, `tests/ipalias/test_ipalias.c`, `tests/ipalias/run.sh`
- Modify: `.github/workflows/tests.yml` (one step in the `firmware-and-simulator` job)

**Interfaces:**
- Produces (used by Tasks 3 and 5):
  ```c
  #define IPALIAS_MAX 3
  typedef enum { IPALIAS_OFF, IPALIAS_CHECKING, IPALIAS_LIVE, IPALIAS_CONFLICT } ipalias_state_t;
  typedef void (*ipalias_cb)(int i, ipalias_state_t st, const struct eth_addr *other);
  void ipalias_init(struct netif *main, ipalias_cb cb);
  void ipalias_set(const ip4_addr_t *addr, int n);
  void ipalias_link(int up);
  struct netif *ipalias_input_netif(const struct pbuf *p);   /* NULL: the main interface */
  ipalias_state_t ipalias_state(int i, ip4_addr_t *addr);
  ```
- Produces: `tests/ipalias/run.sh` with a `t NAME SOURCES…` helper, which Task 3 adds a line to.

- [ ] **Step 1: The test's lwIP options and arch header**

`tests/ipalias/lwipopts.h`:

```c
/* lwIP for tests/ipalias: IPv4, ARP, ACD, TCP and UDP on the host, frames in and out by the
 * test. Checksums are not checked on input, so the test's frames need none. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_RAW                    0
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_ACD                    1
#define LWIP_DNS                    0
#define LWIP_IGMP                   0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (32 * 1024)
#define CHECKSUM_CHECK_IP           0
#define CHECKSUM_CHECK_TCP          0
#define CHECKSUM_CHECK_UDP          0
#define LWIP_STATS                  0
#endif
```

`tests/ipalias/arch/cc.h`:

```c
/* lwIP on the host, for tests/ipalias. */
#ifndef ARCH_CC_H
#define ARCH_CC_H
#include <stdio.h>
#include <stdlib.h>
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "lwIP: %s (%s:%d)\n", x, __FILE__, __LINE__); abort(); } while (0)
#define LWIP_RAND() ((u32_t)rand())
#endif
```

- [ ] **Step 2: The runner**

`tests/ipalias/run.sh` (then `chmod +x tests/ipalias/run.sh`):

```sh
#!/bin/sh
# src/ipalias, and what uses it, against real lwIP on the host (tests/ipalias/lwipopts.h).
# Needs lwIP's source, which configuring the firmware build fetches (build/_deps/lwip-src).
#   tests/ipalias/run.sh            (CC=gcc tests/ipalias/run.sh to use another compiler)
cd "$(dirname "$0")/../.." || exit 1
CC=${CC:-cc}
L=build/_deps/lwip-src/src
[ -d "$L" ] || { echo "configure the firmware build once first (cmake -B build ...), for lwIP's source"; exit 1; }
O=${TMPDIR:-/tmp}/n540ipalias; mkdir -p "$O"
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -w"
LW="$L/core/init.c $L/core/def.c $L/core/inet_chksum.c $L/core/ip.c $L/core/mem.c $L/core/memp.c
    $L/core/netif.c $L/core/pbuf.c $L/core/tcp.c $L/core/tcp_in.c $L/core/tcp_out.c $L/core/timeouts.c
    $L/core/udp.c $L/core/ipv4/acd.c $L/core/ipv4/etharp.c $L/core/ipv4/icmp.c $L/core/ipv4/ip4.c
    $L/core/ipv4/ip4_addr.c $L/core/ipv4/ip4_frag.c $L/netif/ethernet.c"
fail=0
t() {   # name, then flags and sources
  n=$1; shift
  if ! $CC $SAN -Itests/ipalias -Isrc/ipalias -I$L/include "$@" $LW -o "$O/$n" 2> "$O/$n.build"; then
    echo "$n: BUILD FAILED"; cat "$O/$n.build"; fail=1; return; fi
  if out=$(ASAN_OPTIONS=detect_leaks=0 "$O/$n" 2>&1); then echo "$n: $(echo "$out" | tail -1)"
  else echo "$n: FAILED"; echo "$out" | tail -20; fail=1; fi
}
t ipalias tests/ipalias/test_ipalias.c src/ipalias/ipalias.c
exit $fail
```

- [ ] **Step 3: Write the failing test**

`tests/ipalias/test_ipalias.c`:

```c
/* test_ipalias.c — src/ipalias against real lwIP on the host: frames in by hand, the frames
 * sent caught at the link output. Run by tests/ipalias/run.sh. */
#include "ipalias.h"
#include "lwip/init.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static u32_t s_now;
u32_t sys_now(void) { return s_now; }

static struct netif M;
static const uint8_t MAC[6] = { 2, 0, 0, 0, 0, 0x10 }, H[6] = { 2, 0, 0, 0, 0, 0x50 }, X[6] = { 2, 0, 0, 0, 0, 0x99 };
static ip4_addr_t A, B, C, D, HIP, XIP;      /* main, extra ones, a host, another device */

/* ---- what went out ---- */
static uint8_t out[256][128]; static int outn;
static err_t link_out(struct netif *n, struct pbuf *p)
{
    (void)n;
    assert(outn < 256);
    memset(out[outn], 0, sizeof out[outn]);
    pbuf_copy_partial(p, out[outn++], p->tot_len < 128 ? p->tot_len : 128, 0);
    return ERR_OK;
}
static uint32_t at(const uint8_t *f, int o) { uint32_t v; memcpy(&v, f + o, 4); return v; }
static int u16(const uint8_t *f, int o) { return f[o] << 8 | f[o + 1]; }
/* ARP frames sent since `from`, with this opcode, sender address and target address (0: any). */
static int arps(int from, int op, uint32_t spa, uint32_t tpa)
{
    int k = 0;
    for (int i = from; i < outn; i++)
        if (u16(out[i], 12) == 0x0806 && u16(out[i], 20) == op && at(out[i], 28) == spa && (!tpa || at(out[i], 38) == tpa)) k++;
    return k;
}
/* The last TCP segment sent since `from`; NULL if none. */
static const uint8_t *tcp_seg(int from)
{
    for (int i = outn - 1; i >= from; i--)
        if (u16(out[i], 12) == 0x0800 && out[i][23] == 6) return out[i];
    return NULL;
}

/* ---- in ---- */
static void rx(const uint8_t *f, int len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    assert(p);
    pbuf_take(p, f, (u16_t)len);
    struct netif *to = ipalias_input_netif(p);
    if (!to) to = &M;
    if (to->input(p, to) != ERR_OK) pbuf_free(p);
}
static void arp_in(int op, const uint8_t *sha, uint32_t spa, const uint8_t *tha, uint32_t tpa)
{
    static const uint8_t ALL[6] = { 255, 255, 255, 255, 255, 255 }, NONE[6];
    uint8_t f[42] = { 0 };
    memcpy(f, op == 1 ? ALL : tha, 6); memcpy(f + 6, sha, 6); f[12] = 0x08; f[13] = 0x06;
    f[15] = 1; f[16] = 0x08; f[18] = 6; f[19] = 4; f[21] = (uint8_t)op;
    memcpy(f + 22, sha, 6); memcpy(f + 28, &spa, 4); memcpy(f + 32, op == 1 ? NONE : tha, 6); memcpy(f + 38, &tpa, 4);
    rx(f, sizeof f);
}
static void syn_in(uint32_t dst, int dport, int sport)       /* from the host H */
{
    uint8_t f[54] = { 0 };
    memcpy(f, MAC, 6); memcpy(f + 6, H, 6); f[12] = 0x08;
    f[14] = 0x45; f[17] = 40; f[20] = 0x40; f[22] = 64; f[23] = 6;
    memcpy(f + 26, &HIP.addr, 4); memcpy(f + 30, &dst, 4);
    f[34] = (uint8_t)(sport >> 8); f[35] = (uint8_t)sport; f[36] = (uint8_t)(dport >> 8); f[37] = (uint8_t)dport;
    f[41] = 1; f[46] = 0x50; f[47] = 0x02; f[48] = 0xFF; f[49] = 0xFF;        /* seq 1, SYN, window */
    rx(f, sizeof f);
}

static void run(u32_t ms) { for (u32_t t = 0; t < ms; t += 10) { s_now += 10; sys_check_timeouts(); } }

/* ---- what the module said ---- */
static int told, told_i; static ipalias_state_t told_st; static struct eth_addr told_other;
static void said(int i, ipalias_state_t st, const struct eth_addr *other)
{
    told++; told_i = i; told_st = st;
    assert((st == IPALIAS_CONFLICT) == (other != NULL));
    if (other) told_other = *other;
}
static ipalias_state_t state_of(const ip4_addr_t *want)
{
    for (int i = 0; i < IPALIAS_MAX; i++) {
        ip4_addr_t a;
        ipalias_state_t st = ipalias_state(i, &a);
        if (st != IPALIAS_OFF && ip4_addr_eq(&a, want)) return st;
    }
    return IPALIAS_OFF;
}

static err_t m_init(struct netif *n)
{
    n->name[0] = 'e'; n->name[1] = '0';
    n->output = etharp_output; n->linkoutput = link_out; n->mtu = 1500;
    n->hwaddr_len = 6; memcpy(n->hwaddr, MAC, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}
static int accepted;
static err_t on_accept(void *arg, struct tcp_pcb *p, err_t e) { (void)arg; (void)e; accepted++; tcp_abort(p); return ERR_ABRT; }

int main(void)
{
    srand(1);
    IP4_ADDR(&A, 10, 20, 0, 10); IP4_ADDR(&B, 10, 20, 0, 11); IP4_ADDR(&C, 10, 20, 0, 12); IP4_ADDR(&D, 10, 20, 0, 13);
    IP4_ADDR(&HIP, 10, 20, 0, 50); IP4_ADDR(&XIP, 10, 20, 0, 99);
    ip4_addr_t mask, gw;
    IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 10, 20, 0, 1);
    lwip_init();
    assert(netif_add(&M, &A, &mask, &gw, NULL, m_init, ethernet_input));
    netif_set_default(&M); netif_set_up(&M); netif_set_link_up(&M);
    ipalias_init(&M, said);
    run(100);

    /* Before anything: frames go to the main interface. */
    uint8_t junk[20] = { 0 };
    struct pbuf *p = pbuf_alloc(PBUF_RAW, sizeof junk, PBUF_POOL); pbuf_take(p, junk, sizeof junk);
    assert(!ipalias_input_netif(p)); pbuf_free(p);

    /* An extra address: probed, not answered for, then in use. */
    int o = outn;
    ipalias_set(&B, 1);
    assert(state_of(&B) == IPALIAS_CHECKING && told_st == IPALIAS_CHECKING);
    arp_in(1, H, HIP.addr, NULL, B.addr);
    run(3000);
    assert(arps(o, 2, B.addr, 0) == 0);                       /* no reply yet */
    assert(arps(o, 1, 0, B.addr) >= 1);                       /* probes: sender 0.0.0.0 */
    run(12000);
    assert(state_of(&B) == IPALIAS_LIVE && told_st == IPALIAS_LIVE);
    assert(arps(o, 1, 0, B.addr) == 3 && arps(o, 1, B.addr, B.addr) == 2);   /* 3 probes, 2 announcements */
    puts("ok   probed, then in use");

    /* Asked for: answered with that address and the main MAC; the main address as before. */
    o = outn;
    arp_in(1, H, HIP.addr, NULL, B.addr);
    arp_in(1, H, HIP.addr, NULL, A.addr);
    assert(arps(o, 2, B.addr, HIP.addr) == 1 && arps(o, 2, A.addr, HIP.addr) == 1);
    for (int i = o; i < outn; i++) assert(!memcmp(out[i] + 6, MAC, 6) && !memcmp(out[i] + 22, MAC, 6));
    puts("ok   ARP for both addresses, one MAC");

    /* TCP: a listener on the extra address takes it, and answers from it; the main address
     * there is refused; a connection out has the main address. */
    struct tcp_pcb *l = tcp_new();
    assert(tcp_bind(l, &B, 502) == ERR_OK);
    l = tcp_listen(l); tcp_accept(l, on_accept);
    o = outn;
    syn_in(B.addr, 502, 40000);
    const uint8_t *s = tcp_seg(o);
    assert(s && at(s, 26) == B.addr && at(s, 30) == HIP.addr && u16(s, 34) == 502 && (s[47] & 0x12) == 0x12);   /* SYN-ACK */
    o = outn;
    syn_in(A.addr, 502, 40001);
    s = tcp_seg(o);
    assert(s && at(s, 26) == A.addr && (s[47] & 0x04));                                                   /* RST */
    struct tcp_pcb *c = tcp_new();
    o = outn;
    assert(tcp_connect(c, &HIP, 80, NULL) == ERR_OK);
    s = tcp_seg(o);
    assert(s && at(s, 26) == A.addr && (s[47] & 0x02));
    tcp_abort(c);
    puts("ok   TCP to the extra address, from it; out from the main one");

    /* UDP bound to the extra address: udp_sendto refuses; udp_sendto_if_src through the main
     * interface sends from it. */
    struct udp_pcb *u = udp_new();
    assert(udp_bind(u, &B, 4005) == ERR_OK);
    p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM); memcpy(p->payload, "ping", 4);
    assert(udp_sendto(u, p, &HIP, 9) == ERR_RTE);
    o = outn;
    assert(udp_sendto_if_src(u, p, &HIP, 9, &M, &B) == ERR_OK);
    assert(outn == o + 1 && at(out[o], 26) == B.addr && out[o][23] == 17);
    pbuf_free(p); udp_remove(u);
    puts("ok   UDP from the extra address, through the main interface");

    /* Another device claims it: defended once, given up at the second. */
    o = outn;
    arp_in(2, X, B.addr, MAC, A.addr);
    assert(state_of(&B) == IPALIAS_LIVE && arps(o, 1, B.addr, B.addr) == 1);   /* an announcement */
    run(1000);
    arp_in(2, X, B.addr, MAC, A.addr);
    assert(state_of(&B) == IPALIAS_CONFLICT && told_st == IPALIAS_CONFLICT && !memcmp(told_other.addr, X, 6));
    o = outn;
    arp_in(1, H, HIP.addr, NULL, B.addr);
    run(15000);
    assert(arps(o, 2, B.addr, 0) == 0 && arps(o, 1, 0, B.addr) == 0);         /* not answered, not tried again */
    assert(state_of(&B) == IPALIAS_CONFLICT);
    puts("ok   claimed by another: defended, then given up");

    /* Asked again: checked again. Another device answering the probe for a new one. */
    ip4_addr_t two[2] = { B, C };
    ipalias_set(two, 2);
    assert(state_of(&B) == IPALIAS_CHECKING && state_of(&C) == IPALIAS_CHECKING);
    run(500);
    arp_in(2, X, C.addr, MAC, 0);                            /* X has C: it answers the probe */
    assert(state_of(&C) == IPALIAS_CONFLICT && !memcmp(told_other.addr, X, 6));
    run(15000);
    assert(state_of(&B) == IPALIAS_LIVE && state_of(&C) == IPALIAS_CONFLICT);
    puts("ok   checked again when asked; one already taken found");

    /* The link: unplugged, nothing checked; back, the one in use announced, the rest checked. */
    ip4_addr_t two2[2] = { B, D };
    ipalias_set(two2, 2);                                    /* C given up, D new */
    assert(state_of(&C) == IPALIAS_OFF && state_of(&D) == IPALIAS_CHECKING);
    netif_set_link_down(&M); ipalias_link(0);
    o = outn;
    run(15000);
    assert(state_of(&D) == IPALIAS_CHECKING && arps(o, 1, 0, D.addr) == 0);
    netif_set_link_up(&M); ipalias_link(1);
    assert(arps(o, 1, B.addr, B.addr) >= 1 && state_of(&B) == IPALIAS_LIVE);
    run(15000);
    assert(state_of(&D) == IPALIAS_LIVE);
    puts("ok   unplugged and back");

    /* Unchanged ones stay as they are; one taken off is answered no more. */
    o = outn;
    ipalias_set(&D, 1);
    assert(state_of(&B) == IPALIAS_OFF && state_of(&D) == IPALIAS_LIVE);
    arp_in(1, H, HIP.addr, NULL, B.addr);
    arp_in(1, H, HIP.addr, NULL, D.addr);
    assert(arps(o, 2, B.addr, 0) == 0 && arps(o, 2, D.addr, HIP.addr) == 1 && arps(o, 1, 0, D.addr) == 0);
    ip4_addr_t many[4] = { D, D, C, B };                     /* a repeat, and one too many */
    ipalias_set(many, 4);
    assert(state_of(&D) == IPALIAS_LIVE && state_of(&C) == IPALIAS_CHECKING && state_of(&B) == IPALIAS_OFF);
    ipalias_set(NULL, 0);
    for (int i = 0; i < IPALIAS_MAX; i++) assert(ipalias_state(i, NULL) == IPALIAS_OFF);
    puts("ok   kept, removed, repeats");
    puts("ipalias: ok");
    return 0;
}
```

- [ ] **Step 4: Run it to see it fail**

Run: `tests/ipalias/run.sh`
Expected: `ipalias: BUILD FAILED` (no `ipalias.h` yet). If it says to configure the firmware build first, run `cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake` and run it again.

- [ ] **Step 5: Write the module**

`src/ipalias/ipalias.h`:

```c
/*
 * ipalias.h — more IPv4 addresses for one lwIP Ethernet interface.
 *
 * lwIP gives an interface one IPv4 address. This gives the main interface up to
 * IPALIAS_MAX more, on the same link and MAC address, each checked for a conflict first
 * (lwIP's ACD, RFC 5227). It uses lwIP alone (netif, etharp, acd), under the raw API's
 * rules: NO_SYS, or with the core lock held.
 *
 * Each address has an interface of its own, with the main one's MAC address, MTU and link
 * output, up and never with its link up. lwIP's routing passes over an interface without
 * a link, so everything goes out through the main one, replies to an extra address too,
 * with that address as their source; lwIP's input already takes a packet for any
 * interface that is up. What the driver does:
 *   - each received frame goes to ipalias_input_netif(p), or the main interface if NULL;
 *   - after the main interface's link changes, ipalias_link(up).
 * Needs LWIP_ARP and LWIP_ACD. A UDP PCB bound to an extra address sends with
 * udp_sendto_if_src() and the main interface: udp_sendto() refuses a source address the
 * route's interface does not have (ERR_RTE).
 */
#ifndef IPALIAS_H
#define IPALIAS_H
#include "lwip/netif.h"
#include "lwip/prot/ethernet.h"

#define IPALIAS_MAX 3

typedef enum {
    IPALIAS_OFF,            /* no address in this place */
    IPALIAS_CHECKING,       /* probing for it, or waiting for the link to do so */
    IPALIAS_LIVE,           /* in use, and watched */
    IPALIAS_CONFLICT        /* another device has it: not used until asked again */
} ipalias_state_t;

/* A place's state changed. other: with IPALIAS_CONFLICT, the MAC address of the device
 * that has the address; else NULL. */
typedef void (*ipalias_cb)(int i, ipalias_state_t st, const struct eth_addr *other);

void ipalias_init(struct netif *main, ipalias_cb cb);
/* The addresses wanted, at most IPALIAS_MAX (more are ignored, as are 0 and repeats). One
 * no longer listed is given up; a new one is checked; one kept stays as it is, except one
 * in conflict, which is checked again. */
void ipalias_set(const ip4_addr_t *addr, int n);
/* The main interface's link has changed (call after netif_set_link_up/down). Up: an
 * address in use is announced again; one not in use is checked again. Down: checks stop. */
void ipalias_link(int up);
/* The interface a received frame belongs to: an extra address's for an ARP frame to or
 * from that address; NULL for the main interface (everything else). */
struct netif *ipalias_input_netif(const struct pbuf *p);
/* Place i (0 to IPALIAS_MAX - 1): its state, and its address if not IPALIAS_OFF. */
ipalias_state_t ipalias_state(int i, ip4_addr_t *addr);
#endif
```

`src/ipalias/ipalias.c`:

```c
/* ipalias.c — more IPv4 addresses for one lwIP Ethernet interface. See ipalias.h. */
#include "ipalias.h"
#include "lwip/acd.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include <stdbool.h>
#include <string.h>

typedef struct {
    struct netif    nif;
    struct acd      acd;
    ip4_addr_t      addr;
    ipalias_state_t st;
    struct eth_addr other;      /* the last device heard probing for or claiming it */
    bool            used;       /* nif is on lwIP's list */
} alias_t;

static alias_t s_a[IPALIAS_MAX];
static struct netif *s_main;
static ipalias_cb s_cb;

static void tell(int i)
{
    if (s_cb) s_cb(i, s_a[i].st, s_a[i].st == IPALIAS_CONFLICT ? &s_a[i].other : NULL);
}

static int place_of(const struct netif *n)
{
    for (int i = 0; i < IPALIAS_MAX; i++)
        if (s_a[i].used && &s_a[i].nif == n) return i;
    return -1;
}

static void unset(alias_t *a)
{
    if (!ip4_addr_isany_val(*netif_ip4_addr(&a->nif))) netif_set_ipaddr(&a->nif, IP4_ADDR_ANY4);
}

static void acd_said(struct netif *n, acd_callback_enum_t what)
{
    int i = place_of(n);
    if (i < 0) return;
    alias_t *a = &s_a[i];
    if (what == ACD_IP_OK) {
        netif_set_addr(&a->nif, &a->addr, netif_ip4_netmask(s_main), netif_ip4_gw(s_main));
        a->st = IPALIAS_LIVE;
        tell(i);
    } else if (what == ACD_DECLINE) {
        unset(a);
        a->st = IPALIAS_CONFLICT;
        tell(i);
    } else {                            /* ACD_RESTART_CLIENT: tried again only when asked */
        acd_stop(&a->acd);
    }
}

/* From the start: no address until the probes pass; probing only with a link. */
static void check(int i)
{
    alias_t *a = &s_a[i];
    acd_stop(&a->acd);
    unset(a);
    a->st = IPALIAS_CHECKING;
    if (netif_is_link_up(s_main)) acd_start(&a->nif, &a->acd, a->addr);
    tell(i);
}

static err_t if_init(struct netif *n)
{
    n->name[0] = 'a'; n->name[1] = '0' + (char)place_of(n);
    n->output = etharp_output;
    n->linkoutput = s_main->linkoutput;
    n->state = s_main->state;
    n->mtu = s_main->mtu;
    n->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(n->hwaddr, s_main->hwaddr, ETH_HWADDR_LEN);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

static void add(int i, const ip4_addr_t *addr)
{
    alias_t *a = &s_a[i];
    memset(a, 0, sizeof *a);
    a->addr = *addr;
    a->used = true;                     /* before netif_add: if_init looks itself up */
    if (!netif_add(&a->nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, if_init, ethernet_input)) {
        memset(a, 0, sizeof *a);
        return;
    }
    acd_add(&a->nif, &a->acd, acd_said);
    netif_set_up(&a->nif);
    check(i);
}

static void drop(int i)
{
    alias_t *a = &s_a[i];
    acd_stop(&a->acd);
    acd_remove(&a->nif, &a->acd);
    netif_remove(&a->nif);
    memset(a, 0, sizeof *a);
}

void ipalias_init(struct netif *main, ipalias_cb cb)
{
    memset(s_a, 0, sizeof s_a);
    s_main = main;
    s_cb = cb;
}

void ipalias_set(const ip4_addr_t *addr, int n)
{
    if (!s_main) return;
    if (n > IPALIAS_MAX) n = IPALIAS_MAX;
    for (int i = 0; i < IPALIAS_MAX; i++) {
        if (!s_a[i].used) continue;
        bool kept = false;
        for (int k = 0; k < n; k++) kept = kept || ip4_addr_eq(&s_a[i].addr, &addr[k]);
        if (!kept) drop(i);
        else if (s_a[i].st == IPALIAS_CONFLICT) check(i);
    }
    for (int k = 0; k < n; k++) {
        if (ip4_addr_isany_val(addr[k])) continue;
        bool have = false;
        for (int i = 0; i < IPALIAS_MAX; i++) have = have || (s_a[i].used && ip4_addr_eq(&s_a[i].addr, &addr[k]));
        for (int i = 0; !have && i < IPALIAS_MAX; i++)
            if (!s_a[i].used) { add(i, &addr[k]); have = true; }
    }
}

void ipalias_link(int up)
{
    for (int i = 0; i < IPALIAS_MAX; i++) {
        alias_t *a = &s_a[i];
        if (!a->used) continue;
        if (a->st == IPALIAS_LIVE) { if (up) etharp_gratuitous(&a->nif); }
        else if (up) check(i);
        else acd_stop(&a->acd);
    }
}

struct netif *ipalias_input_netif(const struct pbuf *p)
{
    uint8_t f[42];                      /* Ethernet header, then ARP: sender at 22 and 28, target at 38 */
    if (!s_main || p->tot_len < sizeof f || pbuf_copy_partial(p, f, sizeof f, 0) != sizeof f) return NULL;
    if (f[12] != 0x08 || f[13] != 0x06) return NULL;
    ip4_addr_t s, t;
    memcpy(&s.addr, f + 28, 4);
    memcpy(&t.addr, f + 38, 4);
    for (int i = 0; i < IPALIAS_MAX; i++) {
        alias_t *a = &s_a[i];
        if (!a->used || (!ip4_addr_eq(&s, &a->addr) && !ip4_addr_eq(&t, &a->addr))) continue;
        bool about = ip4_addr_eq(&s, &a->addr) || ip4_addr_isany_val(s);     /* a claim, or a probe */
        if (about && memcmp(f + 22, s_main->hwaddr, ETH_HWADDR_LEN)) memcpy(a->other.addr, f + 22, ETH_HWADDR_LEN);
        return &a->nif;
    }
    return NULL;
}

ipalias_state_t ipalias_state(int i, ip4_addr_t *addr)
{
    if (i < 0 || i >= IPALIAS_MAX || !s_a[i].used) return IPALIAS_OFF;
    if (addr) *addr = s_a[i].addr;
    return s_a[i].st;
}
```

- [ ] **Step 6: Run it to see it pass**

Run: `tests/ipalias/run.sh`
Expected: `ipalias: ipalias: ok`, exit status 0.

- [ ] **Step 7: Check that the test guards what matters**

The key behaviour is that the extra interfaces never have their link up. Break that on purpose and watch the test catch it:

```sh
sed -i.bak 's/NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;/NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;/' src/ipalias/ipalias.c
tests/ipalias/run.sh; echo "exit $?"
mv src/ipalias/ipalias.c.bak src/ipalias/ipalias.c
tests/ipalias/run.sh
```

Expected: the first run reports `ipalias: FAILED` (the connection made out takes the extra address), exit 1. After the file is restored, `ipalias: ipalias: ok`.

- [ ] **Step 8: Run it in CI**

In `.github/workflows/tests.yml`, in the `firmware-and-simulator` job, add this step straight after the `MQTT over TLS, against Mosquitto` step (same indentation):

```yaml
      - name: Extra addresses, against real lwIP
        run: CC=clang tests/ipalias/run.sh
```

- [ ] **Step 9: Run the host tests**

Run: `tests/host.sh`
Expected: every line ends in `ok`, exit 0 (nothing there has changed).

- [ ] **Step 10: Commit**

```bash
git add src/ipalias tests/ipalias .github/workflows/tests.yml
git commit -F - <<'EOF'
ipalias: more IPv4 addresses for one lwIP interface, tested against real lwIP

An interface of its own for each address, never with its link up, so lwIP routes
everything out through the main one; ARP frames to or from an address go to its
interface; lwIP's ACD checks each before use. tests/ipalias/run.sh, in CI's
firmware job, where lwIP's source is fetched.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK
EOF
```

---

### Task 2: The setting, `portN.ip`, and how it is stored

**Files:**
- Modify: `src/app/cfg.h` (`more_cfg_t`, one declaration, one define)
- Modify: `src/app/cfg.c` (accessor, `apply`, `cfg_validate`, `cfg_json`, `cfg_lines`)
- Modify: `src/app/cfg_store.c` (`main_save`, `cfg_save`, `more_load`, `cfg_load`)
- Test: `tests/test_cfg.c`, `tests/test_store.c`

**Interfaces:**
- Produces (used by Tasks 4 and 6):
  - `uint32_t cfg_port_ip(const cfg_t *c, int port);`: the port's own address, network byte order; 0 means every address.
  - `#define EXTRA_IPS 3`: addresses besides the main one, at most.
  - The settings JSON gains `"ip":"10.20.0.11"` (or `"ip":""`) in each port's object, just before `"quiet_min"`. The settings text gains `portN.ip=` as the last line of each port's block.

- [ ] **Step 1: Write the failing settings tests**

In `tests/test_cfg.c`, insert this block just before the line `    /* Worst case must fit the HTTP response buffer alongside its headers. */`:

```c
    /* portN.ip: the port's own address; blank, every address. */
    {
        cfg_t keep = c;
        uint32_t b;
        assert(ip4_parse("10.20.0.11", &b));
        OK("port2.ip=10.20.0.11"); assert(cfg_port_ip(&c, 1) == b && !cfg_port_ip(&c, 0));
        static char jj[4344];
        assert(cfg_json(&c, jj, sizeof jj) && strstr(jj, "\"ip\":\"10.20.0.11\",\"quiet_min\"") && strstr(jj, "\"ip\":\"\",\"quiet_min\""));
        assert(cfg_lines(&c, jj, sizeof jj) && strstr(jj, "port2.ip=10.20.0.11\n") && strstr(jj, "port1.ip=\n"));
        static cfg_t t; cfg_defaults(&t); assert(cfg_parse(&t, jj, err, sizeof err) && cfg_port_ip(&t, 1) == b);
        OK("port2.ip="); assert(!cfg_port_ip(&c, 1));
        BAD("port2.ip=10.20.0"); BAD("port2.ip=10.20.0.256"); BAD("port5.ip=10.20.0.11");
        /* Against a static main address: in its subnet, not its network, broadcast or router. */
        OK("net.mode=static\nnet.ip=10.20.0.10\nnet.mask=255.255.255.0\nnet.gateway=10.20.0.1");
        OK("port1.enabled=0\nport4.enabled=0\nport2.enabled=1\nport2.mode=modbus\nport2.local_port=502\nport3.enabled=1\nport3.mode=modbus\nport3.local_port=4003");
        BAD("port3.local_port=502");                                          /* both on every address */
        BAD("port3.local_port=502\nport3.ip=10.20.0.11");                      /* port 2 is still on every address */
        assert(strstr(err, "addresses of their own"));
        OK("port2.ip=10.20.0.10\nport3.ip=10.20.0.11\nport3.local_port=502");  /* the main one, and an extra one */
        BAD("port3.ip=10.20.0.10");                                            /* one address, one port */
        BAD("port3.ip=10.30.0.11"); assert(strstr(err, "subnet"));
        BAD("port3.ip=10.20.0.0"); BAD("port3.ip=10.20.0.255"); BAD("port3.ip=10.20.0.1");
        BAD("port3.ip=127.0.0.1"); BAD("port3.ip=224.0.0.5"); BAD("port3.ip=0.1.2.3");
        OK("port4.enabled=1\nport4.mode=udp\nport4.local_port=502\nport4.ip=10.20.0.11");   /* UDP beside TCP */
        BAD("port4.mode=client\nport4.remote_host=10.20.0.50\nport4.remote_port=4000"); assert(strstr(err, "client"));
        OK("port4.enabled=0\nport4.mode=client\nport4.remote_host=10.20.0.50\nport4.remote_port=4000");   /* off: kept */
        /* No more than EXTRA_IPS besides the main one. */
        OK("net.mode=dhcp\nport1.enabled=1\nport1.mode=server\nport1.local_port=4001\nport1.ip=10.20.0.21\nport2.ip=10.20.0.22\nport3.ip=10.20.0.23\nport4.ip=");
        BAD("port4.enabled=1\nport4.mode=server\nport4.local_port=4004\nport4.ip=10.20.0.24"); assert(strstr(err, "3"));
        OK("port4.enabled=1\nport4.mode=server\nport4.local_port=4004\nport4.ip=10.20.0.23");     /* at one of them */
        c = keep;
    }
```

In the worst-case block that follows, add this line straight after `memset(c.device_name, '\\', 31);`:

```c
    memset(c.more.ip, 0xFF, sizeof c.more.ip);                   /* "255.255.255.255" each */
```

- [ ] **Step 2: Write the failing store test**

In `tests/test_store.c`, insert this block just before the final `    (void)ops;`:

```c
    /* Ports at addresses of their own, both on 502: the main record has them off, so older
     * firmware, which reads it alone, still accepts it; this firmware reads them back on. */
    {
        char e[96];
        cfg_defaults(&c);
        set(&c, "port2.enabled=1\nport2.mode=modbus\nport2.local_port=502\nport2.ip=10.20.0.12\n"
                "port3.enabled=1\nport3.mode=modbus\nport3.local_port=502\nport3.ip=10.20.0.11\n"
                "port4.enabled=0\nport4.ip=10.20.0.13\nport1.enabled=1\nport1.listen_only=1");
        assert(cfg_validate(&c, e, sizeof e));
        assert(cfg_save(&c));
        uint32_t s0, s1, m0, m1;
        memcpy(&m0, F[1] + 0xF0000, 4); memcpy(&s0, F[1] + 0xF0008, 4);
        memcpy(&m1, F[1] + 0xF1000, 4); memcpy(&s1, F[1] + 0xF1008, 4);
        bool second = m1 == 0x47464335u && (m0 != 0x47464335u || s1 > s0);
        static cfg_t old;
        cfg_defaults(&old);
        memcpy(&old, F[1] + (second ? 0xF1000 : 0xF0000) + 48, 656);  /* what older firmware reads */
        assert(old.port[0].enabled && old.port[0].listen_only && !old.port[1].enabled && !old.port[2].enabled && !old.port[3].enabled);
        assert(cfg_validate(&old, e, sizeof e));
        assert(cfg_load(&d) && !memcmp(&c, &d, sizeof c));
        assert(d.port[1].enabled && d.port[2].enabled && !d.port[3].enabled && cfg_port_ip(&d, 3) && !d.more.ip_enabled);
        assert(cfg_save(&d) && cfg_load(&c) && !memcmp(&c, &d, sizeof c));   /* and again */
    }
```

- [ ] **Step 3: Run them to see them fail**

Run: `tests/host.sh`
Expected: `cfg: BUILD FAILED` and `store: BUILD FAILED` (`cfg_port_ip`, `more.ip` and `ip_enabled` are not there yet). Other tests pass.

- [ ] **Step 4: The setting in `cfg.h`**

In `src/app/cfg.h`, in `more_cfg_t`, replace:

```c
    uint8_t modbus_answer;         /* bit per port: unit 0 is a device that answers (modbus_broadcast=answer);
                                      modbus_broadcast's bit is then clear */
    uint8_t spare[146];            /* for later settings */
```

with:

```c
    uint8_t modbus_answer;         /* bit per port: unit 0 is a device that answers (modbus_broadcast=answer);
                                      modbus_broadcast's bit is then clear */
    uint8_t ip[PORTS][4];          /* each port's own address (portN.ip), network byte order; 0 = every address */
    uint8_t ip_enabled;            /* in the stored record only (cfg_store.c): bit per port with an address, on or
                                      not; the main record has such a port off, for older firmware */
    uint8_t spare[129];            /* for later settings */
```

After the declaration of `cfg_modbus_rtu`, add:

```c
/* The port's own address (portN.ip), network byte order; 0: every address the gateway has. */
uint32_t cfg_port_ip(const cfg_t *c, int port);
#define EXTRA_IPS 3                /* addresses besides the main one, at most (src/ipalias: IPALIAS_MAX) */
```

- [ ] **Step 5: Parse, check and print it in `cfg.c`**

After the line defining `cfg_modbus_rtu` (`bool cfg_modbus_rtu(const cfg_t *c, int i) { … }`), add:

```c
uint32_t cfg_port_ip(const cfg_t *c, int i) { uint32_t a; memcpy(&a, c->more.ip[i], 4); return a; }
```

In `apply()`, in the `port` section, insert before `if (strcmp(k + 6, "quiet_min") || !parse_u32(v, &min)) return false;`:

```c
        if (!strcmp(k + 6, "ip")) {                          /* blank: every address */
            uint32_t a = 0;
            if (*v && !ip4_parse(v, &a)) return false;
            memcpy(c->more.ip[k[4] - '1'], &a, 4);
            return true;
        }
```

In `cfg_validate()`, in the port loop, replace:

```c
        if (!p->enabled) continue;
        if (p->mode == MODE_CLIENT) {
```

with:

```c
        if (!p->enabled) continue;
        uint32_t pip = cfg_port_ip(c, i);
        if (pip && p->mode == MODE_CLIENT) {
            snprintf(err, errlen, "Port %d: a TCP client connects from the main address; clear its address", i + 1);
            return false;
        }
        if (p->mode == MODE_CLIENT) {
```

In the same loop, replace the duplicate check:

```c
        for (int j = 0; j < i; j++) {
            const port_cfg_t *q = &c->port[j];
            if (q->enabled && q->mode != MODE_CLIENT && (q->mode == MODE_UDP) == (p->mode == MODE_UDP) &&
                q->local_port == p->local_port) {
                snprintf(err, errlen, "Ports %d and %d both use local port %u", j + 1, i + 1, p->local_port);
                return false;
            }
        }
```

with:

```c
        if (pip) {
            uint32_t a = be(pip);
            if ((a >> 24) == 0 || (a >> 24) == 127 || (a >> 28) >= 14) {
                snprintf(err, errlen, "Port %d: enter a usable address, or leave it blank", i + 1);
                return false;
            }
            if (!c->net.dhcp) {
                uint32_t ip = be(c->net.ip), m = be(c->net.mask);
                if ((a & m) != (ip & m)) {
                    snprintf(err, errlen, "Port %d: its address must be in the gateway's subnet", i + 1);
                    return false;
                }
                if ((a & ~m) == 0 || (a & ~m) == ~m || (c->net.gw && a == be(c->net.gw))) {
                    snprintf(err, errlen, "Port %d: that address is the subnet's own, or its router's", i + 1);
                    return false;
                }
            }
        }
        for (int j = 0; j < i; j++) {
            const port_cfg_t *q = &c->port[j];
            uint32_t qip = cfg_port_ip(c, j);
            if (q->enabled && q->mode != MODE_CLIENT && (q->mode == MODE_UDP) == (p->mode == MODE_UDP) &&
                q->local_port == p->local_port && !(pip && qip && pip != qip)) {
                snprintf(err, errlen, pip || qip ? "Ports %d and %d both use local port %u: only ports at addresses of their own may share one"
                                                 : "Ports %d and %d both use local port %u", j + 1, i + 1, p->local_port);
                return false;
            }
        }
```

After the port loop's closing brace (just before `    for (int i = 0; i < DEVS; i++) {`), add:

```c
    {
        uint32_t seen[PORTS];
        int n = 0;
        for (int i = 0; i < PORTS; i++) {               /* the extra addresses: ports' own, not the main one */
            uint32_t a = cfg_port_ip(c, i);
            const port_cfg_t *p = &c->port[i];
            if (!a || !p->enabled || p->mode == MODE_CLIENT || (!c->net.dhcp && a == c->net.ip)) continue;
            bool dup = false;
            for (int k = 0; k < n; k++) dup = dup || seen[k] == a;
            if (!dup) seen[n++] = a;
        }
        if (n > EXTRA_IPS) {
            snprintf(err, errlen, "No more than %d addresses besides the gateway's own", EXTRA_IPS);
            return false;
        }
    }
```

In `cfg_json()`, in the port loop, replace:

```c
        char t[ACL_TEXT];
        acl_text(&c->access[i], t);
        ADD(",\"quiet_min\":%u,\"access\":\"%s\",\"modbus_timeout\":%u,\"modbus_broadcast\":\"%s\",\"modbus_framing\":\"%s\",\"modbus_tcp\":%s}", cfg_quiet_min(c, i), t,
            cfg_modbus_timeout(c, i), cfg_modbus_unit0(c, i), cfg_modbus_rtu(c, i) ? "rtu" : "tcp", cfg_modbus_tcp(c, i) ? "true" : "false");
```

with:

```c
        char t[ACL_TEXT], a[16] = "";
        acl_text(&c->access[i], t);
        if (cfg_port_ip(c, i)) ip4_fmt(cfg_port_ip(c, i), a);
        ADD(",\"ip\":\"%s\",\"quiet_min\":%u,\"access\":\"%s\",\"modbus_timeout\":%u,\"modbus_broadcast\":\"%s\",\"modbus_framing\":\"%s\",\"modbus_tcp\":%s}", a, cfg_quiet_min(c, i), t,
            cfg_modbus_timeout(c, i), cfg_modbus_unit0(c, i), cfg_modbus_rtu(c, i) ? "rtu" : "tcp", cfg_modbus_tcp(c, i) ? "true" : "false");
```

In `cfg_lines()`, in the port loop, replace:

```c
        char t[ACL_TEXT];
        acl_text(&c->access[i], t);
        ADD("%s.quiet_min=%u\n%s.access=%s\n%s.modbus_timeout=%u\n%s.modbus_broadcast=%s\n%s.modbus_framing=%s\n%s.modbus_tcp=%u\n", pre, cfg_quiet_min(c, i), pre, t,
            pre, cfg_modbus_timeout(c, i), pre, cfg_modbus_unit0(c, i), pre, cfg_modbus_rtu(c, i) ? "rtu" : "tcp", pre, (unsigned)cfg_modbus_tcp(c, i));
```

with:

```c
        char t[ACL_TEXT], a[16] = "";
        acl_text(&c->access[i], t);
        if (cfg_port_ip(c, i)) ip4_fmt(cfg_port_ip(c, i), a);
        ADD("%s.quiet_min=%u\n%s.access=%s\n%s.modbus_timeout=%u\n%s.modbus_broadcast=%s\n%s.modbus_framing=%s\n%s.modbus_tcp=%u\n", pre, cfg_quiet_min(c, i), pre, t,
            pre, cfg_modbus_timeout(c, i), pre, cfg_modbus_unit0(c, i), pre, cfg_modbus_rtu(c, i) ? "rtu" : "tcp", pre, (unsigned)cfg_modbus_tcp(c, i));
        ADD("%s.ip=%s\n", pre, a);
```

- [ ] **Step 6: Store it so that older firmware still reads the main record**

In `src/app/cfg_store.c`, in `main_save()`, after `memcpy(s_w.body, c, SIZE2);`, add:

```c
    for (int p = 0; p < PORTS; p++)     /* older firmware: a port at an address of its own is off (cfg.h: ip_enabled) */
        if (cfg_port_ip(c, p))
            s_w.body[offsetof(cfg_t, port) + p * sizeof(port_cfg_t) + offsetof(port_cfg_t, enabled)] = 0;
```

Replace `cfg_save()` with:

```c
/* Lists first: a setting that needs its list is never stored ahead of it. If only
 * the second write fails, the new lists still apply from the next start. */
bool cfg_save(const cfg_t *c)
{
    more_cfg_t m = c->more;             /* with what the main record cannot say */
    m.ip_enabled = 0;
    for (int p = 0; p < PORTS; p++)
        if (cfg_port_ip(c, p) && c->port[p].enabled) m.ip_enabled |= (uint8_t)(1u << p);
    if (memcmp(s_saved, c->access, sizeof s_saved) && !access_save(c)) return false;
    if (memcmp(&s_msaved, &m, sizeof s_msaved)) {
        if (!side_save(&s_more, &m)) return false;
        s_msaved = m;
    }
    if (memcmp(s_dsaved, c->dev, sizeof s_dsaved)) {
        if (!side_save(&s_dev, c->dev)) return false;
        memcpy(s_dsaved, c->dev, sizeof s_dsaved);
    }
    return main_save(c);
}
```

In `more_load()`, after `s_msaved = c->more;`, add:

```c
    for (int p = 0; p < PORTS; p++)     /* the main record has it off, for older firmware */
        if (cfg_port_ip(c, p) && (c->more.ip_enabled >> p & 1)) c->port[p].enabled = 1;
    c->more.ip_enabled = 0;             /* only ever in the stored record */
```

In `cfg_load()`, replace:

```c
    access_load(c);
    more_load(c);
    dev_load(c);
    return ok;
```

with:

```c
    access_load(c);
    more_load(c);
    dev_load(c);
    bool ips = false;
    for (int p = 0; p < PORTS; p++) ips = ips || cfg_port_ip(c, p);
    if (ips && !cfg_validate(c, err, sizeof err))       /* the records disagree: those ports stay off */
        for (int p = 0; p < PORTS; p++)
            if (cfg_port_ip(c, p)) c->port[p].enabled = 0;
    return ok;
```

If `offsetof` is not yet available in `cfg_store.c`, add `#include <stddef.h>` with its other includes. (It is already used in its `_Static_assert`s, so it normally is.)

- [ ] **Step 7: Run the tests to see them pass**

Run: `tests/host.sh`
Expected: every line ends in `ok`. The `cfg` line is preceded by `longest settings text N bytes` with N at most 3256, and the worst-case JSON is under 4344.

- [ ] **Step 8: The settings fuzz target still builds and runs**

Run: `CC=clang tests/fuzz/run.sh 10 cfg`
Expected: `cfg: done: … 0 failures`. If clang with libFuzzer is not available on this machine, say so in the report; CI runs it.

- [ ] **Step 9: Commit**

```bash
git add src/app/cfg.h src/app/cfg.c src/app/cfg_store.c tests/test_cfg.c tests/test_store.c
git commit -F - <<'EOF'
Settings: portN.ip, a port's own address

Checked against the main address and its subnet; two ports may share a local
port only at addresses of their own; at most 3 besides the main one; not in
client mode. Such a port is stored as off in the main record, and whether it is
on beside its address, so older firmware, which checks the main record alone,
still accepts it after a rollback.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK
EOF
```

---

### Task 3: `addrs.c`: the list, the events, the state

**Files:**
- Create: `src/app/addrs.h`, `src/app/addrs.c`
- Create: `tests/ipalias/test_addrs.c`
- Modify: `src/app/events.h` (a new kind at the end of `ev_kind_t`)
- Modify: `src/net/net.h` (one declaration), `src/net/net.c` (its definition, application only)
- Modify: `CMakeLists.txt` (`src/ipalias` built into the application)
- Modify: `tests/ipalias/run.sh` (one line)

**Interfaces:**
- Consumes: Task 1's `ipalias_*`; Task 2 is not needed here.
- Produces (used by Tasks 4, 5 and 6):
  ```c
  void addrs_init(void);                         /* after net_init */
  void addrs_want(const uint32_t ip[PORTS]);     /* each port's address, network byte order; 0 blank */
  void addrs_poll(void);                         /* main loop, after net_poll */
  size_t addrs_json(char *out, size_t max);      /* "addresses":[{"ip":"…","state":"…"},…]; 0 if it does not fit */
  ```
  - Event `EV_ADDRESS` (kind 21): port 0; `a` the address (network byte order, as `ipOf()` in the web page reads it); `s` is `live`, `outside`, or `taken ` / `lost ` and then the other device's MAC, `02:00:00:00:00:99`.
  - JSON states: `checking`, `live`, `conflict`, `outside`. The longest list is 160 bytes.
  - `net.h` declares `struct netif *net_netif(void);`, defined at the end of `net.c` for the application only.
  - From here the firmware builds `src/ipalias` into the application (`src/app/addrs.c` needs it: `APP_SRC` globs `src/app`).

- [ ] **Step 1: The event kind and the declaration**

In `src/app/events.h`, replace:

```c
                          s: "control=value", then "; " and why */
} ev_kind_t;
```

with:

```c
                          s: "control=value", then "; " and why */
    EV_ADDRESS,        /* an extra address (portN.ip, addrs.c): a: the address; s: "live", "outside",
                          or "taken " / "lost " and the other device's MAC */
} ev_kind_t;
```

In `src/net/net.h`, after the `net_set` declaration, add:

```c
/* The main interface (the application's: src/app/addrs.c). */
struct netif;
struct netif *net_netif(void);
```

- [ ] **Step 2: Write the failing test**

`tests/ipalias/test_addrs.c`:

```c
/* test_addrs.c — src/app/addrs.c with src/ipalias and real lwIP on the host: which ports'
 * addresses become extra ones, the events, the JSON. Run by tests/ipalias/run.sh. */
#include "addrs.h"
#include "events.h"
#include "ipalias.h"
#include "lwip/init.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern int ev_n; extern ev_kind_t ev_kind; extern uint32_t ev_a; extern char ev_s[64];   /* events_stub.c */

static u32_t s_now;
u32_t sys_now(void) { return s_now; }
static struct netif M;
struct netif *net_netif(void) { return &M; }

static const uint8_t MAC[6] = { 2, 0, 0, 0, 0, 0x10 }, X[6] = { 2, 0, 0, 0, 0, 0x99 };
static err_t link_out(struct netif *n, struct pbuf *p) { (void)n; (void)p; return ERR_OK; }
static err_t m_init(struct netif *n)
{
    n->name[0] = 'e'; n->name[1] = '0';
    n->output = etharp_output; n->linkoutput = link_out; n->mtu = 1500;
    n->hwaddr_len = 6; memcpy(n->hwaddr, MAC, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}
static void run(u32_t ms) { for (u32_t t = 0; t < ms; t += 10) { s_now += 10; sys_check_timeouts(); addrs_poll(); } }
static uint32_t ip(int a, int b, int c, int d) { ip4_addr_t x; IP4_ADDR(&x, a, b, c, d); return ip4_addr_get_u32(&x); }
static void claim(uint32_t spa)                        /* X says it has spa */
{
    uint8_t f[42] = { 0 };
    memcpy(f, MAC, 6); memcpy(f + 6, X, 6); f[12] = 0x08; f[13] = 0x06;
    f[15] = 1; f[16] = 0x08; f[18] = 6; f[19] = 4; f[21] = 2;
    memcpy(f + 22, X, 6); memcpy(f + 28, &spa, 4); memcpy(f + 32, MAC, 6);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, sizeof f, PBUF_POOL);
    pbuf_take(p, f, sizeof f);
    struct netif *to = ipalias_input_netif(p);
    if (!to) to = &M;
    if (to->input(p, to) != ERR_OK) pbuf_free(p);
}
static const char *json(void) { static char b[400]; assert(addrs_json(b, sizeof b)); return b; }

int main(void)
{
    srand(1);
    ip4_addr_t a, mask, gw;
    IP4_ADDR(&a, 10, 20, 0, 10); IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 10, 20, 0, 1);
    lwip_init();
    assert(netif_add(&M, &a, &mask, &gw, NULL, m_init, ethernet_input));
    netif_set_default(&M); netif_set_up(&M); netif_set_link_up(&M);
    addrs_init();
    run(100);
    assert(!strcmp(json(), "\"addresses\":[]") && ev_n == 0);

    /* Port 1 blank; 2 and 3 share an address; 4 is outside the subnet. */
    const uint32_t B = ip(10, 20, 0, 11), O = ip(10, 30, 0, 5);
    const uint32_t w1[PORTS] = { 0, B, B, O };
    addrs_want(w1);
    run(10);
    assert(ev_n == 1 && ev_kind == EV_ADDRESS && ev_a == O && !strcmp(ev_s, "outside"));
    assert(!strcmp(json(), "\"addresses\":[{\"ip\":\"10.20.0.11\",\"state\":\"checking\"},{\"ip\":\"10.30.0.5\",\"state\":\"outside\"}]"));
    run(15000);
    assert(ev_n == 2 && ev_a == B && !strcmp(ev_s, "live"));
    assert(strstr(json(), "\"10.20.0.11\",\"state\":\"live\""));
    puts("ok   ports' addresses: one in use, one outside");

    /* Saved again, unchanged: nothing new to say, nothing checked again. */
    addrs_want(w1);
    run(15000);
    assert(ev_n == 2 && strstr(json(), "\"state\":\"live\""));
    /* The main address in a port: not an extra one. */
    const uint32_t w2[PORTS] = { ip(10, 20, 0, 10), B, 0, 0 };
    addrs_want(w2);
    run(10);
    assert(!strcmp(json(), "\"addresses\":[{\"ip\":\"10.20.0.11\",\"state\":\"live\"}]"));
    puts("ok   unchanged; the main address left out");

    /* Lost to another device, then taken by one before use. */
    claim(B); run(500); claim(B);
    assert(ev_n == 3 && ev_a == B && !strcmp(ev_s, "lost 02:00:00:00:00:99"));
    assert(strstr(json(), "\"state\":\"conflict\""));
    const uint32_t C = ip(10, 20, 0, 12), w3[PORTS] = { C, 0, 0, 0 };
    addrs_want(w3);
    run(500);
    claim(C);
    assert(ev_n == 4 && ev_a == C && !strcmp(ev_s, "taken 02:00:00:00:00:99"));
    puts("ok   lost, and taken");

    /* The main address moves to another subnet: the list made again for it. */
    const uint32_t w4[PORTS] = { B, O, 0, 0 };
    addrs_want(w4);
    run(10);
    ip4_addr_t a2, m2;
    IP4_ADDR(&a2, 10, 30, 0, 10); IP4_ADDR(&m2, 255, 255, 255, 0);
    netif_set_addr(&M, &a2, &m2, &gw);
    int e = ev_n;
    run(15000);
    assert(ev_n == e + 2);                                  /* B outside now, O in use */
    const char *j = json();
    assert(strstr(j, "{\"ip\":\"10.30.0.5\",\"state\":\"live\"}") && strstr(j, "{\"ip\":\"10.20.0.11\",\"state\":\"outside\"}"));
    /* No main address (DHCP not yet answered): none. */
    netif_set_addr(&M, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    run(10);
    assert(!strcmp(json(), "\"addresses\":[]"));
    puts("ok   the main address changed, and gone");

    /* At its longest: 3 in the subnet, 1 outside. */
    netif_set_addr(&M, &a, &mask, &gw);
    const uint32_t w5[PORTS] = { ip(10, 20, 0, 111), ip(10, 20, 0, 112), ip(10, 20, 0, 113), ip(10, 30, 0, 114) };
    addrs_want(w5);
    run(15000);
    static char big[400];
    size_t k = addrs_json(big, sizeof big);
    printf("longest list %zu bytes\n", k);
    assert(k && k <= 220 && !addrs_json(big, k));          /* and refused when it does not fit */
    puts("addrs: ok");
    return 0;
}
```

In `tests/ipalias/run.sh`, after the `t ipalias …` line, add:

```sh
t addrs -Itests/stub -Isrc/app -Isrc/net tests/ipalias/test_addrs.c src/app/addrs.c src/ipalias/ipalias.c tests/stub/events_stub.c src/app/cfg.c src/app/acl.c
```

(The `-Itests/ipalias` that `t` puts first means lwIP finds the test's `lwipopts.h` and `arch/cc.h`, not those in `src/net`.)

- [ ] **Step 3: Run it to see it fail**

Run: `tests/ipalias/run.sh`
Expected: `ipalias: ipalias: ok`, then `addrs: BUILD FAILED` (no `addrs.h`).

- [ ] **Step 4: Write `addrs.h` and `addrs.c`**

`src/app/addrs.h`:

```c
/* addrs.h — the gateway's extra addresses: each port's own address (portN.ip) that is not
 * the main one, kept by src/ipalias; their events, and their state for /api/state. */
#ifndef ADDRS_H
#define ADDRS_H
#include "cfg.h"

void addrs_init(void);                          /* after net_init */
/* Each port's address, network byte order; 0 blank (bridge_configure). Taken up by
 * addrs_poll. */
void addrs_want(const uint32_t ip[PORTS]);
/* Main loop, after net_poll: the list again if it or the main address has changed. */
void addrs_poll(void);
/* "addresses":[{"ip":"…","state":"checking|live|conflict|outside"},…]; 0 if it does not fit. */
size_t addrs_json(char *out, size_t max);
#endif
```

`src/app/addrs.c`:

```c
/* addrs.c — the gateway's extra addresses. See addrs.h. */
#include "addrs.h"
#include "events.h"
#include "ipalias.h"
#include "net.h"
#include "lwip/ip4_addr.h"
#include <stdio.h>
#include <string.h>

_Static_assert(EXTRA_IPS <= IPALIAS_MAX, "cfg.c allows no more extra addresses than ipalias keeps");

static uint32_t s_want[PORTS];          /* each port's; 0 blank */
static uint32_t s_ip, s_mask;           /* the main address the list was last made for */
static bool s_dirty = true;
static uint32_t s_out[PORTS];           /* outside the main subnet, and told so */
static uint8_t s_last[IPALIAS_MAX];     /* each place's state last told: taken, or lost */

static void said(int i, ipalias_state_t st, const struct eth_addr *other)
{
    ip4_addr_t a;
    ipalias_state(i, &a);
    if (st == IPALIAS_LIVE) events_add(EV_ADDRESS, 0, ip4_addr_get_u32(&a), "live");
    if (st == IPALIAS_CONFLICT) {
        const uint8_t *m = other->addr;
        char s[24];
        snprintf(s, sizeof s, "%s %02x:%02x:%02x:%02x:%02x:%02x", s_last[i] == IPALIAS_LIVE ? "lost" : "taken",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        events_add(EV_ADDRESS, 0, ip4_addr_get_u32(&a), s);
    }
    s_last[i] = (uint8_t)st;
}

void addrs_init(void)
{
    ipalias_init(net_netif(), said);
}

void addrs_want(const uint32_t ip[PORTS])
{
    memcpy(s_want, ip, sizeof s_want);
    s_dirty = true;
}

static bool in(const uint32_t *l, int n, uint32_t a)
{
    for (int k = 0; k < n; k++)
        if (l[k] == a) return true;
    return false;
}

void addrs_poll(void)
{
    const struct netif *m = net_netif();
    uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(m)), mask = ip4_addr_get_u32(netif_ip4_netmask(m));
    if (!s_dirty && ip == s_ip && mask == s_mask) return;
    s_dirty = false;
    s_ip = ip;
    s_mask = mask;
    uint32_t live[IPALIAS_MAX], out[PORTS] = { 0 };
    int n = 0, no = 0;
    for (int i = 0; i < PORTS && ip; i++) {             /* no main address yet (DHCP): none */
        uint32_t a = s_want[i];
        if (!a || a == ip || in(live, n, a) || in(out, no, a)) continue;
        if ((a & mask) != (ip & mask)) {
            if (!in(s_out, PORTS, a)) events_add(EV_ADDRESS, 0, a, "outside");
            out[no++] = a;
        } else if (n < IPALIAS_MAX) {
            live[n++] = a;
        }
    }
    memcpy(s_out, out, sizeof s_out);
    ip4_addr_t l[IPALIAS_MAX];
    for (int k = 0; k < n; k++) ip4_addr_set_u32(&l[k], live[k]);
    ipalias_set(l, n);
}

#define ADD(...) do { int w = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= max - n) return 0; n += (size_t)w; } while (0)

size_t addrs_json(char *out, size_t max)
{
    static const char *const NAME[] = { "off", "checking", "live", "conflict" };
    size_t n = 0;
    char t[16];
    const char *sep = "";
    if (!max) return 0;
    ADD("\"addresses\":[");
    for (int i = 0; i < IPALIAS_MAX; i++) {
        ip4_addr_t a;
        ipalias_state_t st = ipalias_state(i, &a);
        if (st == IPALIAS_OFF) continue;
        ADD("%s{\"ip\":\"%s\",\"state\":\"%s\"}", sep, ip4addr_ntoa_r(&a, t, sizeof t), NAME[st]);
        sep = ",";
    }
    for (int k = 0; k < PORTS; k++) {
        if (!s_out[k]) continue;
        ip4_addr_t a;
        ip4_addr_set_u32(&a, s_out[k]);
        ADD("%s{\"ip\":\"%s\",\"state\":\"outside\"}", sep, ip4addr_ntoa_r(&a, t, sizeof t));
        sep = ",";
    }
    ADD("]");
    return n;
}
```

`EXTRA_IPS` comes from Task 2's `cfg.h`. If this task runs before Task 2 is merged, stop and report it: the order is Task 2, then Task 3.

- [ ] **Step 5: Run it to see it pass**

Run: `tests/ipalias/run.sh`
Expected: `ipalias: ipalias: ok`, `addrs: addrs: ok` (with `longest list 160 bytes` printed before it), exit 0.

- [ ] **Step 6: Run the host tests**

Run: `tests/host.sh`
Expected: every line ends in `ok`. `events.h` gained a kind at the end; nothing else changed.

- [ ] **Step 7: The firmware still builds: `src/ipalias` into the application, `net_netif()`**

`APP_SRC` globs `src/app`, so the firmware now compiles `addrs.c`, which needs `ipalias.h`, `ipalias.c` and `net_netif()`.

In `CMakeLists.txt`, after `file(GLOB APP_SRC    "${CMAKE_SOURCE_DIR}/src/app/*.c")`, add:

```cmake
file(GLOB IPALIAS_SRC "${CMAKE_SOURCE_DIR}/src/ipalias/*.c")   # the application's only: src/net goes into the loader too
```

In the `n540_image(app app_ram.ld …)` call, add `${IPALIAS_SRC}` after `${APP_SRC}`. After the line `target_include_directories(app.elf SYSTEM PRIVATE ${MBEDTLS_PUBLIC})`, add:

```cmake
target_include_directories(app.elf PRIVATE ${CMAKE_SOURCE_DIR}/src/ipalias)
```

At the end of `src/net/net.c`, add:

```c
#ifdef APP_IMAGE
struct netif *net_netif(void) { return &s_if; }
#endif
```

Run (configuring again, so the globs pick up the new files):

```sh
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake > /dev/null && cmake --build build 2>&1 | grep -E "warning|error" | head; cmake --build build 2>&1 | tail -1
```

Expected: no warnings or errors from `ipalias.c`, `addrs.c` or `net.c`, and the build finishes. Nothing calls `addrs_init` yet (Task 5); `--gc-sections` drops what is unused.

- [ ] **Step 8: Commit**

```bash
git add src/app/addrs.h src/app/addrs.c src/app/events.h src/net/net.h src/net/net.c CMakeLists.txt tests/ipalias/test_addrs.c tests/ipalias/run.sh
git commit -F - <<'EOF'
addrs: the ports' own addresses as the gateway's extra ones

The list for ipalias: each port's address that is not the main one, in its
subnet, made again when settings or the main address change; one outside the
subnet left out and reported. Events for an address in use, taken or lost (with
the other device's MAC), and the list for /api/state.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK
EOF
```

---

### Task 4: The bridge: listening on a port's address

**Files:**
- Modify: `src/app/bridge.c`
- Modify: `tests/stub_net/lwip/tcp.h` (the fake lwIP)
- Test: `tests/test_bridge.c`

**Interfaces:**
- Consumes: `cfg_port_ip()` (Task 2); `addrs_want()` (Task 3, `addrs.h`).
- Produces: `bridge_configure()` calls `addrs_want()` with, for each port, its address if it is enabled and not in client mode, else 0. Each port's listener or UDP socket is bound to its address (every address when 0), and bound again, with the port restarted, when the address changes.

- [ ] **Step 1: Teach the fake lwIP to record addresses**

In `tests/stub_net/lwip/tcp.h`:

- In the error enum, add `ERR_RTE = -4`: `enum { ERR_OK = 0, ERR_MEM = -1, ERR_RTE = -4, ERR_INPROGRESS = -5, ERR_VAL = -6, ERR_ABRT = -13 };`
- After `#define IP_ADDR_ANY NULL`, add `#define IPADDR4_INIT(v) { v }`.
- In `struct tcp_pcb`, add `uint32_t local;` after `ip_addr_t remote_ip; u16_t remote_port, port;`, so the first line reads `ip_addr_t remote_ip; u16_t remote_port, port; uint32_t local; uint32_t keep_idle, keep_intvl, keep_cnt;`.
- Replace `struct udp_pcb { bool used; udp_fn rx; void *arg; };` with `struct udp_pcb { bool used; udp_fn rx; void *arg; uint32_t local; };`.
- After the `udp_sendto` declaration, add:
  ```c
  struct netif;
  struct netif *ip_route(const ip_addr_t *src, const ip_addr_t *dst);
  err_t udp_sendto_if_src(struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t, struct netif *, const ip_addr_t *);
  ```

In `tests/test_bridge.c`:

- Replace the `tcp_bind` stub with
  `err_t tcp_bind(struct tcp_pcb *p, const void *a, u16_t port) { p->port = port; p->local = a ? ((const ip_addr_t *)a)->addr : 0; return ERR_OK; }`
- Replace the `udp_bind` stub with
  `err_t udp_bind(struct udp_pcb *u, const void *a, u16_t p) { u->local = a ? ((const ip_addr_t *)a)->addr : 0; return ERR_OK; }`
- Replace `static struct { uint32_t ip; u16_t port; int n; } US[64]; static int USn;` with
  `static struct { uint32_t ip, src; u16_t port; int n; } US[64]; static int USn;`
- Replace the `udp_sendto` stub with one that records no source address:
  `err_t udp_sendto(struct udp_pcb *u, struct pbuf *q, const ip_addr_t *ip, u16_t port) { US[USn].ip = ip->addr; US[USn].src = 0; US[USn].port = port; US[USn++].n = q->tot_len; return ERR_OK; }`
- After the `udp_sendto` stub, add:
  ```c
  static int s_netif;                                                     /* where ip_route sends: anywhere */
  struct netif *ip_route(const ip_addr_t *src, const ip_addr_t *dst) { (void)src; (void)dst; return (struct netif *)&s_netif; }
  err_t udp_sendto_if_src(struct udp_pcb *u, struct pbuf *q, const ip_addr_t *ip, u16_t port, struct netif *n, const ip_addr_t *src)
  { assert(n == (struct netif *)&s_netif); US[USn].ip = ip->addr; US[USn].src = src->addr; US[USn].port = port; US[USn++].n = q->tot_len; return ERR_OK; }
  #include "addrs.h"
  static uint32_t want_ip[PORTS]; static int wants;
  void addrs_want(const uint32_t ip[PORTS]) { memcpy(want_ip, ip, sizeof want_ip); wants++; }
  ```
- After the `listener()` helper, add:
  ```c
  static struct tcp_pcb *listener_at(u16_t port, uint32_t local)
  { for (int i = 0; i < 256; i++) if (T[i].used && T[i].listening && !T[i].closed && T[i].port == port && T[i].local == local) return &T[i]; return NULL; }
  static struct tcp_pcb *from_at(struct tcp_pcb *l, const char *src)
  {
      static u16_t n = 60000;
      struct tcp_pcb *p = tcp_new();
      p->remote_ip.addr = ip(src); p->remote_port = n++;
      err_t e = l->acc(l->arg, p, ERR_OK);
      assert((e == ERR_ABRT) == p->aborted);
      return p;
  }
  ```

- [ ] **Step 2: Write the failing test**

In `tests/test_bridge.c`, insert this section just before the final `    puts("ok");`:

```c
    /* ---- a port at an address of its own (portN.ip) ---- */
    {
    set("port1.enabled=0\nport1.ip=10.0.0.12\n"
        "port2.enabled=1\nport2.mode=modbus\nport2.local_port=502\nport2.ip=10.0.0.10\nport2.access=\n"
        "port3.enabled=1\nport3.mode=modbus\nport3.local_port=502\nport3.ip=10.0.0.11\nport3.access=\n"
        "port4.enabled=1\nport4.mode=client\nport4.remote_host=h.lan\nport4.remote_port=7\nport4.ip=");
    assert(!want_ip[0] && want_ip[1] == ip("10.0.0.10") && want_ip[2] == ip("10.0.0.11") && !want_ip[3]);   /* off, client: none */
    struct tcp_pcb *l2 = listener_at(502, ip("10.0.0.10")), *l3 = listener_at(502, ip("10.0.0.11"));
    assert(l2 && l3 && l2 != l3);
    struct tcp_pcb *c3 = from_at(l3, "10.0.0.50");
    assert(!c3->aborted);
    int o3 = opened[2];
    set("port3.ip=10.0.0.13");                                          /* moved: listening there, its client gone */
    assert(c3->closed || c3->aborted);
    assert(!listener_at(502, ip("10.0.0.11")) && listener_at(502, ip("10.0.0.13")) && listener_at(502, ip("10.0.0.10")) == l2);
    assert(opened[2] == o3 + 1 && want_ip[2] == ip("10.0.0.13"));
    set("port3.ip=\nport3.local_port=4003");                            /* blank: every address */
    assert(listener_at(4003, 0) && !want_ip[2]);
    /* UDP from its address goes out through the main interface; without one, as before. */
    set("port4.mode=udp\nport4.remote_host=\nport4.remote_port=0\nport4.local_port=4004\nport4.ip=10.0.0.10\nport4.access=10.0.0.9 write");
    struct udp_pcb *u4 = udp();
    assert(u4 && u4->local == ip("10.0.0.10") && want_ip[3] == ip("10.0.0.10"));
    USn = 0; dgram(u4, "10.0.0.9", 99, "w"); heard(3, "r");
    assert(line(3, "w") && USn == 1 && US[0].src == ip("10.0.0.10") && US[0].ip == ip("10.0.0.9") && US[0].port == 99);
    set("port4.ip=");
    u4 = udp(); USn = 0; dgram(u4, "10.0.0.9", 99, "w"); heard(3, "r");
    assert(line(3, "w") && u4->local == 0 && USn == 1 && US[0].src == 0);
    }
```

- [ ] **Step 3: Run it to see it fail**

Run: `tests/host.sh`
Expected: `bridge: BUILD FAILED` or `bridge: FAILED`, because `bridge.c` does not use the address yet. The rest pass.

- [ ] **Step 4: Bind to the port's address, and send UDP from it**

In `src/app/bridge.c`:

- Add `#include "addrs.h"` after `#include "modbus.h"`.
- In `port_t`, after `port_cfg_t cfg;`, add `uint32_t ip;                /* the address it listens on (portN.ip); 0 every one */`.
- After `static port_cfg_t s_want[PORTS];`, add `static uint32_t s_want_ip[PORTS];`.
- In `start_listen()`, replace `if (tcp_bind(l, IP_ADDR_ANY, p->cfg.local_port) == ERR_OK) {` with:
  ```c
        ip_addr_t a = IPADDR4_INIT(p->ip);
        if (tcp_bind(l, p->ip ? &a : IP_ADDR_ANY, p->cfg.local_port) == ERR_OK) {
  ```
- In `start_udp()`, replace
  `if (!p->udp || udp_bind(p->udp, IP_ADDR_ANY, p->cfg.local_port) != ERR_OK) {` with:
  ```c
    ip_addr_t a = IPADDR4_INIT(p->ip);
    if (!p->udp || udp_bind(p->udp, p->ip ? &a : IP_ADDR_ANY, p->cfg.local_port) != ERR_OK) {
  ```
- Before `static void serial_to_net(int i)`, add:
  ```c
  /* From the port's own address, through the main interface: udp_sendto refuses a source the
   * route's interface does not have (src/ipalias/ipalias.h). */
  static err_t udp_out(port_t *p, struct pbuf *q)
  {
      if (!p->ip) return udp_sendto(p->udp, q, &p->udp_peer, p->udp_peer_port);
      ip_addr_t src = IPADDR4_INIT(p->ip);
      struct netif *n = ip_route(&src, &p->udp_peer);
      return n ? udp_sendto_if_src(p->udp, q, &p->udp_peer, p->udp_peer_port, n, &src) : ERR_RTE;
  }
  ```
- In `serial_to_net()`, replace `if (udp_sendto(p->udp, q, &p->udp_peer, p->udp_peer_port) == ERR_OK) {` with `if (udp_out(p, q) == ERR_OK) {`.
- In `bridge_configure()`, before `memcpy(s_want, c->port, sizeof s_want);`, add:
  ```c
    uint32_t want[PORTS];
    for (int i = 0; i < PORTS; i++) {
        s_want_ip[i] = cfg_port_ip(c, i);
        want[i] = c->port[i].enabled && c->port[i].mode != MODE_CLIENT ? s_want_ip[i] : 0;
    }
    addrs_want(want);
  ```
- In `bridge_poll()`, replace `if (same_wiring(&s_p[i].cfg, &s_want[i])) {` with
  `if (same_wiring(&s_p[i].cfg, &s_want[i]) && s_p[i].ip == s_want_ip[i]) {`, and replace
  ```c
            port_stop(i);
            s_p[i].cfg = s_want[i];
            port_start(i);
  ```
  with
  ```c
            port_stop(i);
            s_p[i].cfg = s_want[i];
            s_p[i].ip = s_want_ip[i];
            port_start(i);
  ```

`ip_route` and `udp_sendto_if_src` come from `lwip/ip.h` and `lwip/udp.h`, already included. `struct netif` is declared through them.

- [ ] **Step 5: Run the tests to see them pass**

Run: `tests/host.sh`
Expected: every line ends in `ok`.

- [ ] **Step 6: Commit**

```bash
git add src/app/bridge.c tests/stub_net/lwip/tcp.h tests/test_bridge.c
git commit -F - <<'EOF'
Bridge: a port listens on its own address, and tells addrs which

A listener or UDP socket bound to portN.ip, or to every address when blank, and
bound again when it changes. UDP from such a port goes out with
udp_sendto_if_src through the main interface. Enabled ports not in client mode
give their addresses to addrs_want.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK
EOF
```

---

### Task 5: Wiring it into the firmware

**Files:**
- Modify: `src/net/net.c` (inside `APP_IMAGE` only)
- Modify: `src/app/main.c`, `src/app/api.c`

**Interfaces:**
- Consumes: `ipalias_input_netif`, `ipalias_link` (Task 1); `addrs_init`, `addrs_poll`, `addrs_json`, `net_netif()` (Task 3).
- Produces: `/api/state` ends `…,"devices":[…],"addresses":[…]}`.

- [ ] **Step 1: The firmware builds before the change**

Run: `cmake --build build 2>&1 | tail -1`
Expected: the build finishes (Task 3 left it building).

- [ ] **Step 2: Frames and link changes through the module, in `net.c`**

In `src/net/net.c`, after its last `#include`, add:

```c
#ifdef APP_IMAGE
#include "ipalias.h"
#endif
```

In `net_poll()`, in the `#ifdef APP_IMAGE` receive loop, replace:

```c
        if (s_if.input(p, &s_if) != ERR_OK) pbuf_free(p);
    }
    if (!eth_rx_spare()) pbuf_free_ooseq_pending = 1;  /* as lwIP does when its pool runs dry */
```

with:

```c
        struct netif *to = ipalias_input_netif(p);           /* ARP to or from an extra address: its own */
        if (!to) to = &s_if;
        if (to->input(p, to) != ERR_OK) pbuf_free(p);
    }
    if (!eth_rx_spare()) pbuf_free_ooseq_pending = 1;  /* as lwIP does when its pool runs dry */
```

(The `#else` loop below it, the loader's, stays exactly as it is.)

Replace:

```c
        if (up && !netif_is_link_up(&s_if)) netif_set_link_up(&s_if);
        else if (!up && netif_is_link_up(&s_if)) netif_set_link_down(&s_if);
```

with:

```c
        if (up && !netif_is_link_up(&s_if)) {
            netif_set_link_up(&s_if);
#ifdef APP_IMAGE
            ipalias_link(1);
#endif
        } else if (!up && netif_is_link_up(&s_if)) {
            netif_set_link_down(&s_if);
#ifdef APP_IMAGE
            ipalias_link(0);
#endif
        }
```

- [ ] **Step 3: Start and poll it, in `main.c`**

In `src/app/main.c`, add `#include "addrs.h"` with the other includes. Replace `    CRUMB(1); net_init();        /* marks the Ethernet memory uncacheable */` with:

```c
    CRUMB(1); net_init();        /* marks the Ethernet memory uncacheable */
    addrs_init();                    /* before api_init gives it the ports' addresses */
```

Replace `        STEP(11, "network"); net_poll();` with `        STEP(11, "network"); net_poll(); addrs_poll();`.

- [ ] **Step 4: The list in `/api/state`**

In `src/app/api.c`, add `#include "addrs.h"` with the other includes. At the end of the `/api/state` writer, replace:

```c
    if (n + 4 > max) return 0;
    memcpy(out + n, "]}\n", 4);
    return (int)(n + 3);
}
```

with:

```c
    if (n + 2 > max) return 0;
    memcpy(out + n, "],", 2);
    n += 2;
    size_t ad = addrs_json(out + n, max - n);           /* at most 160 bytes */
    if (!ad || n + ad + 3 > max) return 0;
    n += ad;
    memcpy(out + n, "}\n", 3);
    return (int)(n + 2);
}
```

(This replaces only the last four lines of that function, the ones that close the `devices` array; the code before them is unchanged.)

- [ ] **Step 5: Build, and check the loader**

Run:

```sh
cmake --build build 2>&1 | grep -E "warning|error|FLASH|RAM|ITCM" | head -20
arm-none-eabi-size build/app.elf
cmp -l build/loader_flash.bin "$LOADER_BASE" | awk '$1 < 680 || $1 > 703' | head
```

Expected:
- the build finishes with no new warnings from `ipalias.c`, `addrs.c`, `net.c`, `main.c` or `api.c`;
- the app's size is printed (record it in the report);
- the `cmp | awk` line prints nothing (the loader differs only in its version text). If `$LOADER_BASE` was not given, say so in the report and do not guess.

- [ ] **Step 6: Run everything CI runs**

Run, one after the other:

```sh
tests/host.sh
tests/ipalias/run.sh
CC=clang ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0 tests/sim/run.sh > "${TMPDIR:-/tmp}/sim.log" 2>&1 &
for i in $(seq 600); do curl -s -m 2 http://127.0.0.1:8541/status >/dev/null && break; sleep 1; done
python3 tests/test_tls.py 127.0.0.1:8541 8543 && python3 tests/test_install.py 127.0.0.1:8541 && python3 tests/test_metrics_http.py 127.0.0.1:8541
pkill -f n540sim
CC=clang tests/mqtt_tls/run.sh
CC=clang FUZZ_OUT="${TMPDIR:-/tmp}/fuzz" tests/fuzz/run.sh 30 cfg http hclient acme modbus profile
```

Expected: every one ends `ok` or `all ok`, and the fuzz targets report `0 failures`. The simulator does not build `bridge.c`, `api.c` or `net.c`, so it only checks that nothing else moved. If clang with libFuzzer, or mosquitto, is missing, say which in the report.

- [ ] **Step 7: Commit**

```bash
git add src/net/net.c src/app/main.c src/app/api.c
git commit -F - <<'EOF'
Firmware: extra addresses wired in

Received frames go through ipalias_input_netif and link changes to ipalias_link
(application only: the loader is unchanged, byte for byte); addrs starts after
the network and is polled with it; /api/state gives the addresses.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK
EOF
```

---

### Task 6: The web page and the docs

**Files:**
- Modify: `web/index.html`
- Modify: `docs/GATEWAY.md`, `proxy/README.md`

**Interfaces:**
- Consumes: settings JSON `ports[i].ip` (Task 2); `/api/state` `addresses` (Task 5); event kind 21 (Task 3).

- [ ] **Step 1: The port form's Address field**

In `web/index.html`, in the port form's Network fieldset (the line that has `inp("local_port","Local port",{type:"number",min:0,max:65535}),`), insert straight after that `inp(…)` call:

```js
inp("ip","Address",{maxLength:15,placeholder:"every address"}),
```

Replace `  f.querySelector("[data-f=local_port]").hidden=v==="client";` with:

```js
  f.querySelector("[data-f=local_port]").hidden=f.querySelector("[data-f=ip]").hidden=v==="client";
```

The form already sends each changed field as `portN.<name>=<value>`, so nothing else is needed for saving.

- [ ] **Step 2: Where a port listens, and the extra addresses**

Replace:

```js
 return "Port "+p.local_port+(p.mode==="udp"&&p.remote_host?" to "+p.remote_host+":"+p.remote_port:"")}
```

with:

```js
 return (p.ip&&p.mode!=="client"?p.ip+", port ":"Port ")+p.local_port+(p.mode==="udp"&&p.remote_host?" to "+p.remote_host+":"+p.remote_port:"")}
```

Replace:

```js
 else L.push(p.mode==="udp"?"Ready on UDP port "+p.local_port+".":"Waiting for a client on port "+p.local_port+".");
```

with:

```js
 else L.push((p.mode==="udp"?"Ready on UDP port ":"Waiting for a client on port ")+p.local_port+(p.ip?" at "+p.ip:"")+".");
```

Replace the start of the `netinfo` line:

```js
  fillDl($("netinfo"),[["Name",s.host+".local"],["MAC",s.mac],
```

with:

```js
  fillDl($("netinfo"),[["Name",s.host+".local"],["MAC",s.mac],...(s.addresses&&s.addresses.length?[["Extra addresses",s.addresses.map(a=>a.ip+" ("+(ASTATE[a.state]||a.state)+")").join(", ")]]:[]),
```

Replace:

```js
 if(st.net_pending)L.push(["warn","Network settings are on trial","#network"]);
```

with:

```js
 if(st.net_pending)L.push(["warn","Network settings are on trial","#network"]);
 if((st.addresses||[]).some(a=>a.state==="conflict"||a.state==="outside"))L.push(["warn","An extra address is not in use","#network"]);
```

Just before `function evText(e){`, add:

```js
const ASTATE={checking:"checking",live:"in use",conflict:"in use elsewhere",outside:"outside the subnet"};
```

- [ ] **Step 3: The events**

In `evText`, before `default:return "Event "+k}}`, add:

```js
 case 21:{const w=(s||"").split(" ");return "Address "+ipOf(a)+(w[0]==="live"?" in use":w[0]==="outside"?" not used: outside the gateway's subnet":w[0]==="taken"?" not used: "+w[1]+" has it":" given up: "+w[1]+" claims it")}
```

Replace the start of `evBad`:

```js
const evBad=e=>[2,7,9,16,19].includes(e[4])||
```

with:

```js
const evBad=e=>[2,7,9,16,19].includes(e[4])||(e[4]===21&&e[7]!=="live")||
```

- [ ] **Step 4: Check the page's script still parses**

Run:

```sh
python3 -c "import re,sys; s=open('web/index.html').read(); open(sys.argv[1],'w').write('\n'.join(re.findall(r'<script[^>]*>(.*?)</script>', s, re.S)))" "${TMPDIR:-/tmp}/page.js"
node --check "${TMPDIR:-/tmp}/page.js" && echo parses
cmake --build build 2>&1 | tail -1
```

Expected: `parses`, and the build (which embeds the page) finishes.

- [ ] **Step 5: `docs/GATEWAY.md`**

In the Ports table, after the `modbus_tcp` row, add:

```markdown
| ip | the port's own address. Blank (the default): it listens on every address the gateway has. An address: it listens only there, and if that is not the main address, the gateway takes it on as well ([Extra addresses](#extra-addresses)). Not in client mode. Firmware older than this sees such a port as off |
```

After the paragraph that ends `The\nHardware page shows how many have been in use at once.` (just before `### Modbus TCP gateway`), add:

```markdown
### Extra addresses

A port with an address of its own (`portN.ip`) listens only there, so two ports can both
be on 502, each at its own address: two inverters, each found by a program that looks for
one device per address on 502. An address that is not the gateway's main one is taken on
beside it, up to 3:

- It is static and in the main address's subnet, with the same mask and router; with DHCP
  it is checked when the main address is known. One outside the subnet is not used.
- Before it is used, the gateway checks that nothing else on the LAN has it (ARP probes,
  about 6 to 9 s). If something has, it is not used, and the event names that device's MAC
  address. If another device claims it later, the gateway answers once, and gives it up at
  a second claim within 10 s. It is tried again at a restart, at the next settings save,
  or when the cable is back; one in use is announced again then, and kept.
- The web page, and ports left blank, answer on every address. What the gateway starts
  itself (MQTT, NTP, DNS, renewals, client-mode ports) keeps the main address.
- Two ports may share a local port only if each has an address of its own, and the two
  differ.
- `/api/state` lists them (`addresses`: `checking`, `live`, `conflict`, `outside`), and so
  does the Network section of the page.
- Older firmware knows nothing of them: after a rollback, a port with an address is off
  until it is set again there.

```sh
tools/n540.py $H set port3.mode=modbus port3.local_port=502 port3.ip=10.20.0.211
```
```

In the Events section, in the sentence listing what is kept, add `extra addresses taken on, refused or given up,` after `addresses turned away (once per address per ten minutes),`.

In the Tests section, add one sentence in the style of the others there: `tests/ipalias/run.sh` runs the extra-address module and `addrs.c` against real lwIP on the host, with lwIP's source from the configured firmware build; CI runs it in the firmware job.

- [ ] **Step 6: `proxy/README.md`**

In `## Things to know`, add this bullet after the one that begins `- **The port must be in server mode, or a Modbus gateway with`:

```markdown
- **SolarAssistant can reach a Modbus gateway port without this.** Its Modbus TCP connection
  finds devices only by a scan: every host on its network, on port 502 alone, asking units
  0 to 15 in turn and stopping at the first that answers, so it finds one device per
  address. For a second inverter on another port, give that port an address of its own on
  502 ([extra addresses](../docs/GATEWAY.md#extra-addresses)).
```

- [ ] **Step 7: Check the docs for private details**

Read the diff (`git diff`) for anything besides example addresses (10.20.x.x, 10.0.0.x) and example MACs (`02:00:00:…`): no real addresses, host names, serial numbers, e-mail addresses or home-directory paths. The controller runs its own scan before anything is pushed.

- [ ] **Step 8: Commit**

```bash
git add web/index.html docs/GATEWAY.md proxy/README.md
git commit -F - <<'EOF'
Web and docs: a port's own address, and the gateway's extra ones

The port form's Address field (not in client mode); where a port listens; the
extra addresses under Network, a warning when one is not in use, and their
events. GATEWAY.md: the setting and how extra addresses behave. proxy/README:
what SolarAssistant's scan looks for.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Hdxzpr89WqZMTi7gea33YK
EOF
```

---

### Checkpoint: on the gateway (only with the user present)

Not for subagents. The controller does this with the user, who says when to go. SolarAssistant reads ports 2 and 3, and port 1 is live on the Tigo bus (never transmit on it).

1. Install with no address set; the usual checks: port 1 has sent nothing and both its clients are back, SolarAssistant reconnects, `/api/state` has `"addresses":[]`.
2. The user picks a free address outside the router's DHCP range; nothing answers it, checked from another machine (ARP or ping).
3. `port3.mode=modbus port3.local_port=502 port3.ip=<address>`. SolarAssistant's socat link to inverter B ends. Within about 10 s `/api/state` shows the address `live` and an event says it is in use. From another machine: it answers ARP, and 502 accepts a TCP connection (no Modbus request sent: nothing on port 3's line).
4. A port is set to an address that a spare machine on the LAN already has. The "taken" event appears, the address is listed as in conflict, and that machine still reaches the gateway's page. Then it is set back.
5. SolarAssistant scans and finds inverter B at the new address, unit 1; port 3's counts are watched meanwhile.
6. The socat unit for port 3 on SolarAssistant's machine is retired (`n540-tty@`), by the user or with their go-ahead.
7. Back at any point: `port3.ip= port3.local_port=4003 port3.mode=server`; the firmware before stays in the other slot.
