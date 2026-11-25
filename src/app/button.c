#include "button.h"

static bool in_window(uint32_t ms) { return ms >= BUTTON_HOLD_MS && ms < BUTTON_CANCEL_MS; }

bool button_step(button_t *b, bool raw, uint32_t now)
{
    if (raw != b->raw) {
        b->raw = raw;
        b->raw_at = now;
    }
    if (raw == b->down || now - b->raw_at < BUTTON_DEBOUNCE_MS) return false;
    b->down = raw;
    if (raw) {
        b->presses++;
        b->down_at = b->raw_at;
        return false;
    }
    b->held_ms = b->raw_at - b->down_at;
    return in_window(b->held_ms);
}

bool button_armed(const button_t *b, uint32_t now)
{
    return b->down && b->raw && in_window(now - b->down_at);
}
