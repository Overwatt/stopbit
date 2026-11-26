/* devmqtt.h — Devices over MQTT, with Home Assistant discovery (devmqtt.c). mqtt_ha.c owns
 * the connection and calls these. */
#ifndef DEVMQTT_H
#define DEVMQTT_H
#include <stdbool.h>
#include <stdint.h>

typedef bool (*devmqtt_pub_t)(const char *topic, const char *payload, bool retain);
void devmqtt_init(void);                  /* after device_init() */
void devmqtt_restart(void);               /* a new connection, base topic or discovery setting: all again */
/* One thing published: a discovery config, a status, a state, or a removed device's
 * topics emptied. False if pub had no room: the same again next time. */
bool devmqtt_poll(devmqtt_pub_t pub, const char *base, const char *host, bool announce, uint32_t now);

#endif
