/* events_stub.c — events_add() for host tests that link device.c without events.c: the last
 * one kept for the test to look at, and a count. */
#include "events.h"
#include <stdio.h>

int ev_n, ev_port;
ev_kind_t ev_kind;
uint32_t ev_a;
char ev_s[64];

void events_add(ev_kind_t k, int port, uint32_t a, const char *s)
{
    ev_n++;
    ev_kind = k;
    ev_port = port;
    ev_a = a;
    snprintf(ev_s, sizeof ev_s, "%s", s ? s : "");
}
