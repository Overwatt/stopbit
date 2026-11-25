/* flash_sim.h — The two NOR chips, for host tests. Writes only clear bits, erases
 * take time, anything touching a protected range fails the test, and the power can
 * be cut in the middle of any erase or write. Include once. */
#include "extflash.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t t_now;
t_regs_t t_regs;
t_rtc_t  t_rtc;
uint32_t SystemCoreClock = 400000000u;

static uint8_t  F[2][EXTFLASH_SIZE];
static int      f_busy[2];
static long     f_ops, f_cut = -1;          /* cut the power during this operation */
static jmp_buf  f_power;
static int      f_stuck = -1;               /* this chip never finishes */
static uint32_t f_flip;                     /* corrupt one byte of this write */
typedef struct { int dev; uint32_t from, to; } range_t;
static range_t  f_no[8];
static int      f_nno;

#ifdef FLASH_REAL
#define F_ERASE ((int)t_now + 45)
#define F_WRITE ((int)t_now + 1)
#else
#define F_ERASE 5
#define F_WRITE 1
#endif

static void f_check(int dev, uint32_t a, uint32_t n)
{
    if (a + n > EXTFLASH_SIZE) { printf("write past the chip: %d %x\n", dev, a); abort(); }
    for (int i = 0; i < f_nno; i++)
        if (f_no[i].dev == dev && a < f_no[i].to && a + n > f_no[i].from) {
            printf("PROTECTED range written: chip %d %06x\n", dev, a);
            abort();
        }
}
static void f_wait(int dev) { f_busy[dev] = 0; }

void extflash_init(void) { }
uint32_t extflash_id(int d) { (void)d; return 0xC84015; }
bool extflash_busy(int d)
{
#ifdef FLASH_REAL                           /* typical GD25Q16 times, real clock */
    return (int32_t)(t_now - (uint32_t)f_busy[d]) < 0;
#endif
    t_now++;
    if (d == f_stuck) return true;
    if (f_busy[d]) { f_busy[d]--; return true; }
    return false;
}
void extflash_read(int d, uint32_t a, void *out, uint32_t n)
{
    f_wait(d);
    if (a + n > EXTFLASH_SIZE) { printf("read past the chip\n"); abort(); }
    memcpy(out, F[d] + a, n);
}
void extflash_erase_start(int d, uint32_t a)
{
    f_wait(d);
    if (a & 4095) { printf("unaligned erase %x\n", a); abort(); }
    f_check(d, a, 4096);
    if (f_ops++ == f_cut) {                 /* half erased: some bits back to 1 */
        for (int i = 0; i < 4096; i++) F[d][a + i] |= (uint8_t)rand();
        longjmp(f_power, 1);
    }
    memset(F[d] + a, 0xFF, 4096);
    f_busy[d] = F_ERASE;
}
void extflash_write_start(int d, uint32_t a, const void *src, uint32_t n)
{
    const uint8_t *p = src;
    f_wait(d);
    if ((a & 255) + n > 256) { printf("write crosses a page: %x+%u\n", a, n); abort(); }
    f_check(d, a, n);
    bool cut = f_ops++ == f_cut;
    uint32_t k = cut ? (uint32_t)rand() % n : n;
    for (uint32_t i = 0; i < k; i++) F[d][a + i] &= p[i];
    if (cut) longjmp(f_power, 1);
    if (f_flip && f_ops == (long)f_flip) F[d][a] ^= 0x40;
    f_busy[d] = F_WRITE;
}
bool extflash_erase(int d, uint32_t a, uint32_t n)
{
    for (uint32_t e = a + n; a < e; a += 4096) extflash_erase_start(d, a);
    return true;
}
bool extflash_write(int d, uint32_t a, const void *src, uint32_t n)
{
    const uint8_t *p = src;
    while (n) {
        uint32_t k = 256 - (a & 255);
        if (k > n) k = n;
        extflash_write_start(d, a, p, k);
        a += k; p += k; n -= k;
    }
    return true;
}
