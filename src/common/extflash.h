/*
 * extflash.h — The two on-board GD25Q16 SPI NOR chips, bit-banged.
 *   0: QUADSPI bank 2 pins (stock firmware's XIP code flash)
 *   1: SPI4 pins, CS = PE4
 * Bit-banging keeps the loader tiny and identical for both chips; ~1 MB/s is
 * plenty for loading an image at boot.
 */
#ifndef EXTFLASH_H
#define EXTFLASH_H
#include <stdint.h>
#include <stdbool.h>
#define EXTFLASH_COUNT 2
#define EXTFLASH_SIZE  (2u * 1024u * 1024u)
void     extflash_init(void);
uint32_t extflash_id(int dev);        /* JEDEC: 0xC84015 = GD25Q16 */
/* Both feed the external watchdog while they wait. `addr` is 4 KB aligned. */
bool     extflash_erase(int dev, uint32_t addr, uint32_t len);
bool     extflash_write(int dev, uint32_t addr, const void *src, uint32_t len);
void     extflash_read(int dev, uint32_t addr, void *dst, uint32_t len);
/* Start one 4 KB erase, or one write within a 256-byte page, and return at once.
 * Everything else waits for the chip first, so these may overlap other use. */
void     extflash_erase_start(int dev, uint32_t addr);
void     extflash_write_start(int dev, uint32_t addr, const void *src, uint32_t len);
bool     extflash_busy(int dev);
#endif
