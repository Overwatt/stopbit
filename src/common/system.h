#ifndef SYSTEM_H
#define SYSTEM_H
#include <stdint.h>
#include <stdbool.h>
/* Load area for staged images, and for apps once running. */
#define STAGE_ADDR 0x24000000u
#define STAGE_SIZE (384u * 1024u)

bool SystemClock_Config(void);     /* true = HSE, false = fell back to HSI */
/* Why app image at `addr` is not bootable, or NULL if its vectors look sane. */
const char *image_check_ram(uint32_t addr, uint32_t size);
const char *image_check_loader(const uint8_t *img, uint32_t size);
void jump_to_image(uint32_t addr) __attribute__((noreturn));
/* Replace internal flash with `size` staged bytes at `src`, then reset. */
void self_flash(const uint8_t *src, uint32_t size) __attribute__((noreturn));
#endif
