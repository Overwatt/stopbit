#include "cfg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char err[96];
static int n;
static bool set(cfg_t *c, const char *text)
{
    char b[2048];
    cfg_t t = *c;
    strcpy(b, text);
    err[0] = 0;
    n++;
    if (!cfg_parse(&t, b, err, sizeof err) || !cfg_validate(&t, err, sizeof err)) return false;
    *c = t;
    return true;
}
#define OK(s)  do { if (!set(&c, s)) { printf("FAIL should accept %s: %s\n", s, err); return 1; } } while (0)
#define BAD(s) do { if (set(&c, s))  { printf("FAIL should reject %s\n", s); return 1; } assert(err[0]); } while (0)

int main(void)
{
    cfg_t c;
    char j[4344];
    cfg_defaults(&c);
    assert(cfg_validate(&c, err, sizeof err));
    for (int i = 0; i < PORTS; i++) assert(!c.port[i].enabled);

    assert(!c.mqtt.no_announce); OK("mqtt.announce=0"); assert(c.mqtt.no_announce == 1); BAD("mqtt.announce=2"); BAD("mqtt.no_announce=1");
    { static char jj[4344]; assert(cfg_json(&c, jj, sizeof jj) && strstr(jj, "\"announce\":false")); assert(cfg_lines(&c, jj, sizeof jj) && strstr(jj, "mqtt.announce=0\n")); }
    OK("mqtt.announce=1"); assert(!c.mqtt.no_announce);
    OK("port3.quiet_min=1\nport1.quiet_min=1440"); assert(cfg_quiet_min(&c, 2) == 1 && cfg_quiet_min(&c, 0) == 1440 && !cfg_quiet_min(&c, 1) && !cfg_quiet_min(&c, 3));
    BAD("port1.quiet_min=4"); BAD("port1.quiet_min=-1"); BAD("port1.quiet_min="); BAD("port5.quiet_min=1");
    OK("port3.quiet_min=0"); assert(cfg_quiet_min(&c, 2) == 0 && cfg_quiet_min(&c, 0) == 1440); OK("port1.quiet_min=0"); assert(c.quiet == 0);
    OK("port2.listen_only=1"); assert(c.port[1].listen_only == 1 && !c.port[0].listen_only && !c.port[2].listen_only);
    OK("port2.access=10.0.0.5 read, 10.0.0.0/24 write"); assert(c.access[1].n == 2 && !c.access[0].n && !c.access[2].n);
    BAD("port2.access=10.0.0.5"); assert(strstr(err, "Access:")); BAD("port2.access=10.0.0.5 read, 10.0.0.5 write"); BAD("port5.access=10.0.0.5 read"); BAD("mqtt.access="); BAD("device.access=");
    { static char jj[4344]; assert(cfg_json(&c, jj, sizeof jj) && strstr(jj, "\"access\":\"10.0.0.5 read, 10.0.0.0/24 write\"")); assert(cfg_lines(&c, jj, sizeof jj) && strstr(jj, "port2.access=10.0.0.5 read, 10.0.0.0/24 write\n") && strstr(jj, "port1.access=\n"));
      static cfg_t t; cfg_defaults(&t); assert(cfg_parse(&t, jj, err, sizeof err) && !memcmp(t.access, c.access, sizeof t.access)); }
    OK("port2.access="); assert(!c.access[1].n);
    BAD("port2.loopback=1"); BAD("port3.listen_only=1\nport3.loopback=1"); BAD("port1.listen_only=2");
    OK("port2.listen_only=0\nport2.loopback=1"); OK("port2.loopback=0");
    OK("port1.baud=115200\r\nport1.enabled=1\n\nport1.parity=even\n");
    assert(c.port[0].baud == 115200 && c.port[0].parity == PARITY_EVEN && c.port[0].enabled);
    OK("port4.name=Pump \"A\" \\ x");
    OK("device.name=");
    BAD("port1.baud=0"); BAD("port1.baud=299"); BAD("port1.baud=2000001");
    BAD("port1.baud=4294976896");              /* wraps to 9600 in 32 bits */
    BAD("port1.baud=-1"); BAD("port1.baud=9600x"); BAD("port1.baud=");
    BAD("port0.baud=9600"); BAD("port5.baud=9600"); BAD("port1.nope=1"); BAD("port1.baud"); BAD("=");
    BAD("port1.mode=Server"); BAD("port1.data_bits=9"); BAD("port1.enabled=2");
    BAD("port1.packet_len=0"); BAD("port1.packet_len=1401"); BAD("port1.gap_ms=1001");
    BAD("port1.name=123456789012345678901234"); /* 24 chars, no room for NUL */
    OK("port1.name=12345678901234567890123");
    BAD("port1.name=tab\there"); BAD("port1.name=caf\xc3\xa9");
    BAD("port1.remote_host=a b"); BAD("port1.remote_host=<script>"); BAD("port1.remote_host=a_b");
    BAD("port2.data_bits=7");                   /* 7N1 */
    OK("port2.data_bits=7\nport2.parity=odd");
    OK("port2.parity=none\nport2.stop_bits=2");
    OK("port2.data_bits=8\nport2.stop_bits=1");
    BAD("port1.local_port=80"); BAD("port1.local_port=0"); BAD("port1.local_port=65536");
    BAD("port2.enabled=1\nport2.local_port=4001");
    OK("port2.enabled=1\nport2.mode=udp\nport2.local_port=4001");   /* other protocol */
    BAD("port2.local_port=5353"); BAD("port2.remote_host=10.0.0.1");
    OK("port2.remote_host=10.0.0.1\nport2.remote_port=99");
    BAD("port3.enabled=1\nport3.mode=client");
    BAD("port3.enabled=1\nport3.mode=client\nport3.remote_host=h");
    OK("port3.enabled=1\nport3.mode=client\nport3.remote_host=my-host.lan\nport3.remote_port=502");
    /* A rejected set must not change anything. */
    cfg_t before = c;
    BAD("port1.baud=19200\nport1.baud=bogus");
    assert(!memcmp(&before, &c, sizeof c));

    BAD("net.mode=static"); BAD("net.mode=x"); BAD("net.ip=1.2.3"); BAD("net.ip=1.2.3.256");
    BAD("net.ip=1.2.3.4.5"); BAD("net.ip=1.2.3.4 "); BAD("net.ip=0001.2.3.4"); BAD("net.ip=1..2.3");
    OK("net.mode=static\nnet.ip=10.20.1.210\nnet.mask=255.255.254.0\nnet.gateway=10.20.0.1");
    BAD("net.mask=255.0.255.0"); BAD("net.mask=255.255.255.255"); BAD("net.mask=0.0.0.0"); BAD("net.mask=255.255.255.254");
    BAD("net.ip=10.20.0.0"); BAD("net.ip=10.20.1.255"); BAD("net.ip=127.0.0.1"); BAD("net.ip=224.0.0.5"); BAD("net.ip=");
    BAD("net.gateway=10.76.0.1"); BAD("net.gateway=10.20.1.210");
    OK("net.gateway="); OK("net.dns=1.1.1.1"); OK("net.mode=dhcp");

    /* MQTT */
    assert(c.mqtt.port == 1883 && !c.mqtt.enabled && !c.mqtt.control);
    BAD("mqtt.enabled=1"); BAD("mqtt.port=0"); BAD("mqtt.port=65536"); BAD("mqtt.host=a/b");
    BAD("mqtt.password=x"); BAD("mqtt.password_enc=00");
    OK("mqtt.enabled=1\nmqtt.host=homeassistant.local\nmqtt.user=n540\nmqtt.control=1");
    strcpy(c.mqtt.password, "hunter2-secret");
    BAD("mqtt.user=");                          /* password without user */
    assert(offsetof(cfg_t, mqtt) == 472);       /* schema 1 size: stored configs depend on it */

    size_t l = cfg_json(&c, j, sizeof j);
    assert(l && l == strlen(j));
    assert(strstr(j, "\"name\":\"Pump \\\"A\\\" \\\\ x\""));
    assert(!strstr(j, "web_key") && !strstr(j, "salt") && !strstr(j, "hunter2"));
    assert(strstr(j, "\"password_set\":true},\"password_set\":false"));
    for (size_t k = 0; k < l; k++) assert(cfg_json(&c, j, k) == 0);     /* never overruns */
    /* Worst case must fit the HTTP response buffer alongside its headers. */
    for (int i = 0; i < PORTS; i++) {
        memset(c.port[i].name, '"', 23); memset(c.port[i].remote_host, 'a', 47);
        c.port[i].baud = 2000000;
        assert(acl_parse(&c.access[i], "255.255.255.240/31 write, 255.255.255.242/31 write, 255.255.255.244/31 write, 255.255.255.246/31 write, 255.255.255.248/31 write, 255.255.255.250/31 write, 255.255.255.252/31 write, 255.255.255.254/31 write"));
    }
    memset(c.device_name, '\\', 31);
    l = cfg_json(&c, j, sizeof j);
    printf("ok: %d change sets, worst-case JSON %zu bytes\n", n, l);
    assert(l && l < 4344);                      /* http.c: RESP_MAX - 256 */
    { static char ll[3600]; size_t k = cfg_lines(&c, ll, sizeof ll); printf("longest settings text %zu bytes\n", k); assert(k); }
    return 0;
}
