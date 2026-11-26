/* test_devmqtt_long.c — a state whose text is longer than 200 characters still goes out whole
 * (devmqtt.c's value buffer). Own profile source in place of the built-ins. */
#include "devmqtt.h"
#include "device.h"
#include "../tests/fuzz/json_ok.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t t_now = 1000;
uint32_t SystemCoreClock = 400000000;
modbus_t *bridge_modbus_port(int p) { (void)p; return NULL; }

static char T[2048];
const char *profiles_id(int i) { return i == 0 ? "longtext" : NULL; }
const char *profiles_text(const char *id, size_t *len)
{
    if (strcmp(id, "longtext")) return NULL;
    int n = snprintf(T, sizeof T,
        "profile longtext 1\nname \"Long\"\nmaker \"ACME\"\nline 9600 8N1\nstale 5\ntimeout 1000ms\n"
        "block b hold 0x0000 2 10s\nsensor v \"Volts\" b 0x0000 u16 V dc=voltage\ntext w \"Warnings\" b 0x0001 bits");
    for (int k = 0; k < 16; k++) n += snprintf(T + n, sizeof T - (size_t)n, " %d=\"Alarm condition %02d\"", k, k);
    snprintf(T + n, sizeof T - (size_t)n, "\n");
    *len = strlen(T);
    return T;
}

static char out[4096];
static size_t got;
static bool pub(const char *t, const char *p, bool r)
{
    (void)r;
    if (strstr(t, "/state")) { snprintf(out, sizeof out, "%s", p); got = strlen(p); }
    return true;
}

int main(void)
{
    static cfg_t c;
    uint8_t pdu[8] = { 3, 4, 0, 5, 0xFF, 0xFF };
    cfg_defaults(&c);
    c.port[1].mode = MODE_MODBUS;
    c.dev[0] = (dev_cfg_t){ "longtext", "lt", "Long", "", 2, 1, 0, 0 };
    device_init();
    devmqtt_init();
    device_configure(&c);
    if (!device_get(0)->p) puts(device_get(0)->error);
    assert(device_get(0)->p);
    device_reply(1, (uint32_t)device_get(0)->gen << 16, 0, pdu, 6);
    assert(device_get(0)->online);
    for (int k = 0; k < 50; k++) assert(devmqtt_poll(pub, "solar", "h", true, t_now += 1000));
    assert(got && json_ok(out) && strstr(out, "\"v\":5") && strstr(out, "Alarm condition 15"));
    const char *w = strstr(out, "\"w\":\"");
    assert(w && strchr(w + 5, '"') - (w + 5) > 300);       /* well over the old 200-byte buffer */
    printf("long text state: %zu bytes\n", got);
    puts("devmqtt_long: ok");
    return 0;
}
