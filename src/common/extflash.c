#include "extflash.h"
#include "board.h"
#include "wdt.h"

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

static uint8_t xfer(const bus_t *b, uint8_t out)
{
    uint8_t in = 0;
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

void extflash_read(int dev, uint32_t addr, void *dst, uint32_t len)
{
    const bus_t *b = &BUS[dev];
    uint8_t *p = dst;
    wait_idle(b, 1000);          /* an erase or write may have been started */
    lo(b->cs);
    xfer(b, 0x03);
    xfer(b, (uint8_t)(addr >> 16)); xfer(b, (uint8_t)(addr >> 8)); xfer(b, (uint8_t)addr);
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
    for (uint32_t i = 0; i < len; i++) xfer(b, p[i]);
    hi(b->cs);
}

bool extflash_erase(int dev, uint32_t addr, uint32_t len)
{
    for (uint32_t end = addr + len; addr < end; addr += 4096) {
        extflash_erase_start(dev, addr);
        if (!wait_idle(&BUS[dev], 1000)) return false;
    }
    return true;
}

bool extflash_write(int dev, uint32_t addr, const void *src, uint32_t len)
{
    const uint8_t *p = src;
    while (len) {
        uint32_t n = 256u - (addr & 255u);
        if (n > len) n = len;
        extflash_write_start(dev, addr, p, n);
        if (!wait_idle(&BUS[dev], 50)) return false;
        addr += n; p += n; len -= n;
    }
    return true;
}
