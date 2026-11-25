/* button.h — Reload button timing. Pure logic, so it is tested on the host. */
#ifndef BUTTON_H
#define BUTTON_H
#include <stdint.h>
#include <stdbool.h>

#define BUTTON_DEBOUNCE_MS  30u
#define BUTTON_HOLD_MS    3000u      /* release from here on resets access... */
#define BUTTON_CANCEL_MS 15000u      /* ...until here; the stock firmware's limits */

typedef struct {
    bool     raw, down;
    uint32_t raw_at, down_at;        /* ms */
    uint32_t presses, held_ms;       /* held_ms: the last finished press */
} button_t;

/* Call often. True once, when a press of the right length is released. */
bool button_step(button_t *b, bool raw, uint32_t now);
/* Held long enough, not too long: releasing now would act. */
bool button_armed(const button_t *b, uint32_t now);
#endif
