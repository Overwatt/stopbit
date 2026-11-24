/* led.h — Heartbeat/status LED. Pin comes from board.h. */
#ifndef LED_H
#define LED_H

void led_init(void);
void led_on(void);
void led_off(void);
void led_toggle(void);

#endif /* LED_H */
