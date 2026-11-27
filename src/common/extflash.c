#include "extflash.h"
#include "board.h"
#include "wdt.h"
#include <stdio.h>
#include <string.h>

typedef struct { GPIO_TypeDef *port; uint8_t pin; } pin_t;
typedef struct { pin_t cs, clk, mosi, miso; } bus_t;

static const bus_t BUS[EXTFLASH_COUNT] = {
    { {GPIOC, 11}, {GPIOB, 2}, {GPIOE, 7}, {GPIOE, 8} },
    { {GPIOE, 4},  {GPIOE, 2}, {GPIOE, 6}, {GPIOE, 5} },
};
/* QSPI IO2/IO3 = /WP and /HOLD on chip 0: hold both high. */
static const pin_t HIGH[] = { {GPIOE, 9}, {GPIOE, 10} };

static inline void hi(pin_t p) { p.port->BSRR = 1u << p.pin; }
static inline void lo(pin_t p) { p.port->BSRR = 1u << (p.pin + 16); }

static void mode(pin_t p, uint32_t m, uint32_t pull)
{
    MODIFY_REG(p.port->OSPEEDR, 3u << (2 * p.pin), 2u << (2 * p.pin));
    MODIFY_REG(p.port->PUPDR, 3u << (2 * p.pin), pull << (2 * p.pin));
    MODIFY_REG(p.port->MODER, 3u << (2 * p.pin), m << (2 * p.pin));
}

#ifdef APP_IMAGE
/*
 * Chip 1 is wired to SPI4 (SCK PE2, MISO PE5, MOSI PE6; chip select stays a plain output),
 * and in the application the peripheral does its clocking: 25 MHz (PCLK2 / 4), mode 0,
 * 8 bits, with up to four bytes on their way at once. That is about five times as fast as
 * by hand, which matters where the main loop waits: a 2 KB read, a 256-byte page.
 * The loader clocks both chips by hand, as before; so does the application for chip 0
 * (QUADSPI's pins), and for chip 1 if the check at start-up disagrees or the peripheral
 * ever stops answering.
 */
static bool s_spi;                       /* chip 1 is on SPI4 now */
static const char *s_spi_why = "not tried";
#define SPI_SPIN 20000u                  /* status reads before giving up on a byte: it takes a dozen */

static void spi_pins(bool on)
{
    const bus_t *b = &BUS[1];
    if (on) {
        const pin_t af[] = { b->clk, b->mosi, b->miso };
        for (unsigned i = 0; i < 3; i++) {
            MODIFY_REG(af[i].port->AFR[af[i].pin >> 3], 15u << (4 * (af[i].pin & 7)), 5u << (4 * (af[i].pin & 7)));   /* AF5 */
            MODIFY_REG(af[i].port->MODER, 3u << (2 * af[i].pin), 2u << (2 * af[i].pin));
        }
    } else {
        lo(b->clk); lo(b->mosi);
        MODIFY_REG(b->clk.port->MODER, 3u << (2 * b->clk.pin), 1u << (2 * b->clk.pin));
        MODIFY_REG(b->mosi.port->MODER, 3u << (2 * b->mosi.pin), 1u << (2 * b->mosi.pin));
        MODIFY_REG(b->miso.port->MODER, 3u << (2 * b->miso.pin), 0u);
    }
}

/* Back to clocking by hand, and why: with the peripheral's own account of itself, for
 * /api/hardware, as there is no other way to see it on a gateway in service. */
static void spi_off(const char *why)
{
    static char msg[72];
    snprintf(msg, sizeof msg, "%s (SR %lx, CR1 %lx, CFG2 %lx)", why, (unsigned long)SPI4->SR,
             (unsigned long)SPI4->CR1, (unsigned long)SPI4->CFG2);
    s_spi = false;
    s_spi_why = msg;
    SPI4->CR1 = 0;
    spi_pins(false);
}

static void spi_on(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SPI4EN;
    (void)RCC->APB2ENR;
    RCC->APB2RSTR |= RCC_APB2RSTR_SPI4RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI4RST;
    /* First: its select input is this bit (SSM), and a master that finds it low takes
     * that for another master on the bus, and stops being one (a mode fault). */
    SPI4->CR1 = SPI_CR1_SSI;
    SPI4->CFG1 = (1u << SPI_CFG1_MBR_Pos) | (7u << SPI_CFG1_CRCSIZE_Pos) | (7u << SPI_CFG1_DSIZE_Pos);   /* /4, 8 bits, a byte at a time */
    SPI4->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_AFCNTR;    /* mode 0, MSB first; the pins stay ours between bytes */
    SPI4->CR2 = 0;                       /* no set length: it clocks whenever there is a byte to send */
    SPI4->IFCR = SPI_IFCR_MODFC | SPI_IFCR_OVRC;
    if (!(SPI4->CFG2 & SPI_CFG2_MASTER)) { spi_off("SPI4 would not be the master"); return; }
    SPI4->CR1 |= SPI_CR1_SPE;
    SPI4->CR1 |= SPI_CR1_CSTART;
    spi_pins(true);
    s_spi = true;
    s_spi_why = "";
}

/* n bytes out and as many in. out NULL: zeros go out; in NULL: what comes in is dropped. */
static void spi_io(const uint8_t *out, uint8_t *in, uint32_t n)
{
    uint32_t sent = 0, got = 0, spin = 0;
    while (got < n) {
        uint32_t sr = SPI4->SR;
        if (sent < n && sent - got < 4u && (sr & SPI_SR_TXP)) {
            *(volatile uint8_t *)&SPI4->TXDR = out ? out[sent] : 0;
            sent++;
        }
        if (sr & SPI_SR_RXP) {
            uint8_t v = *(volatile uint8_t *)&SPI4->RXDR;
            if (in) in[got] = v;
            got++;
            spin = 0;
        } else if (++spin > SPI_SPIN || (sr & (SPI_SR_OVR | SPI_SR_MODF))) {
            /* whoever asked checks what it got, and asks again by hand */
            spi_off(sr & SPI_SR_MODF ? "SPI4 mode fault" : sr & SPI_SR_OVR ? "SPI4 overrun" : "SPI4 stopped answering");
            return;
        }
    }
}
#endif

static uint8_t xfer(const bus_t *b, uint8_t out)
{
    uint8_t in = 0;
#ifdef APP_IMAGE
    if (s_spi && b == &BUS[1]) { spi_io(&out, &in, 1); return in; }
#endif
    for (int i = 7; i >= 0; i--) {
        if (out & (1u << i)) hi(b->mosi); else lo(b->mosi);
        (void)b->clk.port->IDR;              /* setup time; also flushes the write */
        hi(b->clk);
        in = (uint8_t)(in << 1) | ((b->miso.port->IDR >> b->miso.pin) & 1u);
        lo(b->clk);
    }
    return in;
}

static void cmd1(const bus_t *b, uint8_t op)
{
    lo(b->cs);
    xfer(b, op);
    hi(b->cs);
    (void)b->cs.port->IDR;
}

#ifdef APP_IMAGE
static void read_some(const bus_t *b, uint32_t addr, uint8_t *p, uint32_t len);

/* A kilobyte of chip 1 as a number, however it is being read. */
static uint32_t sum_of(uint32_t addr)
{
    uint8_t b[64];
    uint32_t h = 2166136261u;
    for (uint32_t k = 0; k < 1024u; k += sizeof b) {
        read_some(&BUS[1], addr + k, b, sizeof b);
        for (unsigned i = 0; i < sizeof b; i++) h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* Chip 1's name and three kilobytes of it (the settings, and the start of each firmware
 * slot: whatever is stored there), read by hand and then through SPI4, which is used only
 * if every bit agrees. That shows both data lines, the clock, the mode and the speed to
 * be right, on this board, at each start. */
static void spi_check(void)
{
    static const uint32_t AT[] = { 0x0F0000u, 0x101000u, 0x181000u };
    uint32_t hand[3], id = extflash_id(1);
    if (id == 0 || id == 0xFFFFFFu) { s_spi_why = "chip 1 does not answer"; return; }
    for (unsigned i = 0; i < 3; i++) hand[i] = sum_of(AT[i]);
    spi_on();
    if (!s_spi) return;
    bool same = extflash_id(1) == id;
    for (unsigned i = 0; i < 3 && same && s_spi; i++) same = sum_of(AT[i]) == hand[i];
    if (s_spi && !same) spi_off("SPI4 read differently");
}

bool extflash_spi(const char **why) { if (why) *why = s_spi_why; return s_spi; }
#endif

void extflash_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIOEEN;
    (void)RCC->AHB4ENR;
    for (unsigned i = 0; i < 2; i++) { hi(HIGH[i]); mode(HIGH[i], 1, 0); }
    for (int d = 0; d < EXTFLASH_COUNT; d++) {
        const bus_t *b = &BUS[d];
        hi(b->cs); lo(b->clk); lo(b->mosi);
        mode(b->cs, 1, 0); mode(b->clk, 1, 0); mode(b->mosi, 1, 0); mode(b->miso, 0, 1);
        HAL_Delay(1);
        /* Leave continuous-read mode, soft-reset, release from power-down. */
        lo(b->cs); hi(b->mosi);
        xfer(b, 0xFF); xfer(b, 0xFF); xfer(b, 0xFF); xfer(b, 0xFF);
        hi(b->cs);
        cmd1(b, 0x66);
        cmd1(b, 0x99);
        cmd1(b, 0xAB);
        HAL_Delay(1);
    }
#ifdef APP_IMAGE
    spi_check();
#endif
}

uint32_t extflash_id(int dev)
{
    const bus_t *b = &BUS[dev];
    lo(b->cs);
    xfer(b, 0x9F);
    uint32_t id = (uint32_t)xfer(b, 0) << 16;
    id |= (uint32_t)xfer(b, 0) << 8;
    id |= xfer(b, 0);
    hi(b->cs);
    return id;
}

static bool wait_idle(const bus_t *b, uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();
    lo(b->cs);
    xfer(b, 0x05);
    bool ok = true;
    while (xfer(b, 0) & 1u) {
        wdt_feed();
        if (HAL_GetTick() - t0 > ms) { ok = false; break; }
    }
    hi(b->cs);
    return ok;
}

static void (*s_meanwhile)(void);
void extflash_meanwhile(void (*fn)(void)) { s_meanwhile = fn; }
static void read_some(const bus_t *b, uint32_t addr, uint8_t *p, uint32_t len);

/* Clocked by hand, so slow: a long read lets the rest run every 1 KB. Only the
 * HTTPS record is that long, read at start-up and straight after an erase. */
void extflash_read(int dev, uint32_t addr, void *dst, uint32_t len)
{
    uint8_t *p = dst;
    if (len <= 2048u || !s_meanwhile) { read_some(&BUS[dev], addr, p, len); return; }
    while (len) {
        uint32_t n = len < 1024u ? len : 1024u;
        read_some(&BUS[dev], addr, p, n);
        addr += n; p += n; len -= n;
        if (len) s_meanwhile();
    }
}

static void read_some(const bus_t *b, uint32_t addr, uint8_t *p, uint32_t len)
{
    wait_idle(b, 1000);          /* an erase or write may have been started */
    lo(b->cs);
    xfer(b, 0x03);
    xfer(b, (uint8_t)(addr >> 16)); xfer(b, (uint8_t)(addr >> 8)); xfer(b, (uint8_t)addr);
#ifdef APP_IMAGE
    while (s_spi && b == &BUS[1] && len) {          /* a run at a time, the watchdog fed between */
        uint32_t n = len < 4096u ? len : 4096u;
        spi_io(NULL, p, n);
        p += n; len -= n;
        wdt_feed();
    }
#endif
    while (len--) {
        *p++ = xfer(b, 0);
        if ((len & 0xFFFu) == 0) wdt_feed();
    }
    hi(b->cs);
}

static void addr_cmd(const bus_t *b, uint8_t op, uint32_t a)
{
    wait_idle(b, 1000);
    cmd1(b, 0x06);
    lo(b->cs);
    xfer(b, op);
    xfer(b, (uint8_t)(a >> 16)); xfer(b, (uint8_t)(a >> 8)); xfer(b, (uint8_t)a);
}

bool extflash_busy(int dev)
{
    const bus_t *b = &BUS[dev];
    lo(b->cs);
    xfer(b, 0x05);
    bool busy = xfer(b, 0) & 1u;
    hi(b->cs);
    return busy;
}

void extflash_erase_start(int dev, uint32_t addr)
{
    const bus_t *b = &BUS[dev];
    addr_cmd(b, 0x20, addr);
    hi(b->cs);
}

void extflash_write_start(int dev, uint32_t addr, const void *src, uint32_t len)
{
    const bus_t *b = &BUS[dev];
    const uint8_t *p = src;
    addr_cmd(b, 0x02, addr);
#ifdef APP_IMAGE
    if (s_spi && b == &BUS[1]) { spi_io(p, NULL, len); len = 0; }
#endif
    for (uint32_t i = 0; i < len; i++) xfer(b, p[i]);
    hi(b->cs);
}

bool extflash_erase(int dev, uint32_t addr, uint32_t len)
{
    for (uint32_t end = addr + len; addr < end; addr += 4096) {
        uint32_t t0 = HAL_GetTick();
        extflash_erase_start(dev, addr);
        while (extflash_busy(dev)) {
            wdt_feed();
            if (s_meanwhile) s_meanwhile();
            if (HAL_GetTick() - t0 > 1000) return false;
        }
    }
    return true;
}

/* A long write lets the rest run between pages, as an erase does: each takes ~0.8 ms,
 * and a 4 KB record is 16 of them. Every long write follows an erase in the same place,
 * so nothing runs where it did not already. */
bool extflash_write(int dev, uint32_t addr, const void *src, uint32_t len)
{
    const uint8_t *p = src;
    bool breathe = len > 1024u && s_meanwhile;
    while (len) {
        uint32_t n = 256u - (addr & 255u), t0 = HAL_GetTick();
        if (n > len) n = len;
        extflash_write_start(dev, addr, p, n);
        if (breathe) {
            while (extflash_busy(dev)) {
                wdt_feed();
                s_meanwhile();
                if (HAL_GetTick() - t0 > 50) return false;
            }
        } else if (!wait_idle(&BUS[dev], 50)) return false;
        addr += n; p += n; len -= n;
    }
    return true;
}
