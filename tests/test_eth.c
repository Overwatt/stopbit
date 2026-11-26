/* The application's receive path in eth_mac.c, against a fake MAC: buffers lent with
 * their frames and given back later, in any order, while the DMA keeps filling.
 *   cc -DAPP_IMAGE '-DDMA_MEM=__attribute__((aligned(32)))' -Itests/stub_eth -Isrc/net \
 *      tests/test_eth.c src/net/eth_mac.c
 * The fake DMA works as RM0433 says: descriptors in ring order; it stops at one it
 * does not own and waits for a new tail pointer; write-back overwrites the buffer
 * address. Every frame says its own number, so a buffer the DMA reused while lent,
 * a frame lost or repeated, or one out of order shows. */
#include "eth_mac.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

t_eth_t t_eth;
t_rcc_t t_rcc;
t_syscfg_t t_syscfg;
uint32_t t_mpu_base, t_mpu_rasr;

typedef struct { volatile uint32_t d0, d1, d2, d3; } desc_t;
#define OWN (1u << 31)
#define FD  (1u << 29)
#define LD  (1u << 28)
#define ES  (1u << 15)

/* Descriptors hold 32-bit addresses; the rest comes from where this program is. */
static void *at(uint32_t a) { return (void *)(((uintptr_t)&t_eth & ~(uintptr_t)0xFFFFFFFFu) | a); }

static uint32_t dma_i;                  /* the descriptor the DMA is at */
static bool dma_stopped;
static uint32_t ring_n(void) { return t_eth.DMACRDRLR + 1; }

/* A frame arriving: into the DMA's descriptor, or dropped if it has none. */
static bool arrive(uint32_t seq, uint32_t len, bool bad)
{
    desc_t *r = at(t_eth.DMACRDLAR);
    if (dma_stopped) {
        if (!t_eth.DMACRDTPR) return false;          /* no new tail pointer yet */
        dma_stopped = false;
    }
    desc_t *d = &r[dma_i];
    if (!(d->d3 & OWN)) { dma_stopped = true; t_eth.DMACRDTPR = 0; return false; }
    uint8_t *b = at(d->d0);
    for (uint32_t k = 0; k < len; k++) b[k] = (uint8_t)(seq * 7 + k);
    memcpy(b, &seq, 4);
    d->d0 = 0xDEADBEEFu; d->d1 = 0x5A5A5A5Au; d->d2 = 0;   /* write-back: the address is gone */
    d->d3 = FD | LD | (bad ? ES : 0) | len;
    dma_i = (dma_i + 1) % ring_n();
    return true;
}

static bool frame_ok(const uint8_t *b, uint32_t seq, uint32_t len)
{
    uint32_t s;
    memcpy(&s, b, 4);
    if (s != seq) return false;
    for (uint32_t k = 4; k < len; k++) if (b[k] != (uint8_t)(seq * 7 + k)) return false;
    return true;
}

typedef struct { uint8_t *b; uint32_t buf, seq, len; } lent_t;
static lent_t L[64];
static int nl;

static void give(int i)
{
    assert(frame_ok(L[i].b, L[i].seq, L[i].len));      /* untouched while lent */
    eth_rx_give(L[i].buf);
    L[i] = L[--nl];
}

int main(void)
{
    static const uint8_t mac[6] = { 2, 0x54, 1, 2, 3, 4 };
    eth_init(mac);
    assert(ring_n() == 12 && t_mpu_base == 0x30000000u && t_mpu_rasr == (0xE0u << 8 | 15u << 1));   /* 40 of 64 KB uncached */
    desc_t *r = at(t_eth.DMACRDLAR);
    for (uint32_t i = 0; i < ring_n(); i++) assert(r[i].d3 & OWN);
    assert(eth_rx_spare() == ETH_RX_BUFS - 12);
    uint32_t len, buf, lent, peak, starved;

    /* one at a time, given straight back */
    for (uint32_t s = 1; s <= 40; s++) {
        assert(arrive(s, 60 + s, false));
        uint8_t *b = eth_rx_take(&len, &buf);
        assert(b && len == 60 + s && frame_ok(b, s, len));
        assert(!eth_rx_take(&len, &buf));
        eth_rx_give(buf);
    }
    eth_rx_stats(&lent, &peak, &starved);
    assert(lent == 0 && peak == 1 && starved == 0 && eth_rx_spare() == ETH_RX_BUFS - 12);

    /* all kept: the spares go first, then the ring runs dry and the MAC drops */
    uint32_t seq = 100;
    for (int k = 0; k < (int)ETH_RX_BUFS; k++) {
        assert(arrive(seq, 1514, false));
        L[nl].b = eth_rx_take(&L[nl].len, &L[nl].buf);
        assert(L[nl].b && frame_ok(L[nl].b, seq, L[nl].len));
        L[nl++].seq = seq++;
        assert(eth_rx_spare() == (k < (int)ETH_RX_BUFS - 12 ? ETH_RX_BUFS - 13 - (uint32_t)k : 0));
    }
    assert(!arrive(seq, 100, false) && !arrive(seq, 100, false));        /* no buffer for the DMA */
    eth_rx_stats(&lent, &peak, &starved);
    assert(lent == ETH_RX_BUFS && peak == ETH_RX_BUFS && starved > 0);
    eth_rx_give(L[0].buf);                                              /* a buffer given back twice */
    uint32_t again = L[0].buf;
    L[0] = L[--nl];
    eth_rx_give(again);
    eth_rx_stats(&lent, &peak, &starved);
    assert(lent == ETH_RX_BUFS - 1);                                    /* the second is ignored */
    assert(arrive(seq, 100, false));                                    /* and the DMA goes on */
    L[nl].b = eth_rx_take(&L[nl].len, &L[nl].buf);
    assert(L[nl].b && frame_ok(L[nl].b, seq, 100));
    L[nl++].seq = seq++;
    assert(!arrive(seq, 100, false));
    while (nl) give(nl - 1);
    assert(eth_rx_spare() == ETH_RX_BUFS - 12);

    /* bad frames are passed over, their buffers kept for the DMA */
    assert(arrive(seq, 80, true) && arrive(seq + 1, 80, false));
    uint8_t *b = eth_rx_take(&len, &buf);
    assert(b && frame_ok(b, seq + 1, 80));
    eth_rx_give(buf);
    seq += 2;

    /* at random: frames arrive, some bad, and are taken, kept and given back in any order */
    srand(540);
    uint32_t sent = seq, next = seq, dropped = 0, bad = 0, got = 0;
    for (int step = 0; step < 2000000; step++) {
        int what = rand() % 10;
        if (what < 4) {
            bool is_bad = rand() % 50 == 0;
            if (arrive(sent, 64 + (uint32_t)rand() % 1450, is_bad)) {
                if (is_bad) bad++;
                sent++;
            } else {
                dropped++;
            }
        } else if (what < 7) {
            uint8_t *t = eth_rx_take(&len, &buf);
            if (!t) continue;
            uint32_t s;
            memcpy(&s, t, 4);
            assert(s >= next);                  /* in order; a bad one between is skipped */
            next = s + 1;
            assert(frame_ok(t, s, len));
            for (int i = 0; i < nl; i++) assert(L[i].buf != buf && L[i].b != t);   /* not lent twice */
            got++;
            if (nl < (int)ETH_RX_BUFS && rand() % 3) { L[nl].b = t; L[nl].buf = buf; L[nl].seq = s; L[nl++].len = len; }
            else eth_rx_give(buf);
        } else if (nl) {
            give(rand() % nl);
        }
        eth_rx_stats(&lent, &peak, &starved);
        assert(lent == (uint32_t)nl && lent + eth_rx_spare() <= ETH_RX_BUFS);
    }
    while (nl) give(nl - 1);
    while ((b = eth_rx_take(&len, &buf))) { got++; eth_rx_give(buf); }
    assert(got + bad == sent - seq);                                    /* every frame the DMA took came out */
    assert(eth_rx_spare() == ETH_RX_BUFS - 12);
    eth_rx_stats(&lent, &peak, &starved);
    printf("%u frames, %u dropped while the ring was dry, %u bad; at most %u lent\n", got, dropped, bad, peak);
    puts("ok");
    return 0;
}
