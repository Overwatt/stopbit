/* hwinfo.h — What the board is and how it is doing, for GET /api/hardware. */
#ifndef HWINFO_H
#define HWINFO_H
#include <stddef.h>
#include <stdbool.h>

void   hwinfo_init(void);
bool   hwinfo_button(void);                  /* poll; true: reset access now */
bool   hwinfo_button_armed(void);            /* held long enough; blink fast */
size_t hwinfo_json(char *out, size_t max);   /* length, or 0 if it does not fit */
#endif
