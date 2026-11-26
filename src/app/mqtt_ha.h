/*
 * mqtt_ha.h — MQTT client with Home Assistant discovery.
 *
 *   <host>/status              online | offline (last will)
 *   <host>/portN, <host>/device                       JSON state
 *   <host>/portN/set/<field>, <host>/device/set/name  commands
 *   <host>/device/set/restart                         "PRESS"
 *
 * Commands work only when mqtt.control is on, reach only port settings, the
 * device name and restart, and are ignored if they arrive retained.
 */
#ifndef MQTT_HA_H
#define MQTT_HA_H
#include "cfg.h"

/* Call after any configuration change (cheap; compares with what is running). */
void mqtt_ha_configure(const cfg_t *c);
void mqtt_ha_poll(const cfg_t *c);
/* Tell the broker we are leaving, so HA shows it at once. Follow with a moment
 * of net_poll() before resetting. */
void mqtt_ha_shutdown(void);
const char *mqtt_ha_state(void);       /* off, connecting, connected, ... */
const char *mqtt_ha_error(void);
bool mqtt_ha_tls(void);                 /* the connection is (to be) over TLS */       /* last connection problem, or "" */
#endif
