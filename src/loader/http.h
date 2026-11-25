#ifndef HTTP_H
#define HTTP_H
#include <stdbool.h>
#include <stdint.h>
void http_init(void);
/* Runs deferred actions (run / reboot / self-update) once their reply is sent. */
void http_poll(void);
#endif
