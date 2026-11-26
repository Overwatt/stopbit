/* hwinfo.h — What the board is and how it is doing, for GET /api/hardware. */
#ifndef HWINFO_H
#define HWINFO_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

void   hwinfo_init(void);
bool   hwinfo_button(void);                  /* poll; true: reset access now */
bool   hwinfo_button_armed(void);            /* held long enough; blink fast */
void   hwinfo_loop(void);                    /* once per pass: how long since the last? */
void   hwinfo_step(const char *name);        /* the main loop starts this step: which takes longest? */
void   hwinfo_request(const char *path, uint32_t cycles);   /* an API reply took this long to make */
size_t hwinfo_json(char *out, size_t max);   /* length, or 0 if it does not fit */
void   hwinfo_loop_us(uint32_t *recent, uint32_t *since_start);   /* longest pauses */
bool   hwinfo_temp(int32_t *decic);          /* the processor's temperature, tenths of a degree */
#endif
