/*
 * events.h — What happened, and when: kept in external flash (chip 1, 0x0FC000, four
 * sectors), the last 192 to 256, through restarts and power cuts. Notes are cut to 39
 * characters.
 *
 * events_add() only queues, so it may be called from anywhere, network callbacks
 * included; events_poll() writes from the main loop. Writing is rationed (a burst of
 * 40, then one every 90 s), so a flood of anything cannot wipe the history; what is not
 * kept is counted, and the count is kept.
 */
#ifndef EVENTS_H
#define EVENTS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    EV_START = 1,      /* a: reset cause (RCC_RSR), s: version */
    EV_FAULT,          /* a: where (PC), s: which fault */
    EV_FW_KEPT,        /* s: version */
    EV_SETTINGS,       /* a: ports changed (bit per port), s: what else: "network,mqtt" */
    EV_NET_TRIAL,      /* new network settings on trial */
    EV_NET_KEPT,
    EV_NET_REVERTED,
    EV_MQTT_UP,        /* s: broker */
    EV_MQTT_DOWN,      /* s: why */
    EV_ALERT,          /* port, s: "silent", "no_client", "turned_away", or "" when it clears */
    EV_TURNED_AWAY,    /* port, a: address (network order), s: count so far, as text */
    EV_RENEWAL,        /* a: 0 renewed, 1 test passed, 2 failed; s: why it failed */
    EV_CERT,           /* s: the name on a certificate installed by hand */
    EV_CLOCK,          /* s: the time server that set it */
    EV_BUTTON,         /* the reload button: network to DHCP, password cleared */
    EV_AUTH_FAIL,      /* a: how many since the last one kept */
    EV_PASSWORD,       /* the web password was set or changed */
    EV_RESTART,        /* s: who asked: "web", "mqtt" */
    EV_DROPPED,        /* a: events not kept, too many at once */
} ev_kind_t;

void events_init(void);            /* finds where the log ends; after extflash_init() */
void events_poll(void);            /* main loop: writes what is queued */
void events_flush(void);           /* before a deliberate restart */
void events_add(ev_kind_t k, int port, uint32_t a, const char *s);
/* Up to `max_n` events older than `before` (0: the newest), newest first, as JSON. */
size_t events_json(uint32_t before, int max_n, char *out, size_t max);
#endif
