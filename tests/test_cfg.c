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
    for (int i = 0; i < PORTS; i++)     /* older firmware refuses the whole record, and loads defaults */
        assert(!t.port[i].enabled || t.port[i].mode == MODE_CLIENT || t.port[i].local_port);
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
    assert(!c.more.ntp[0]); OK("net.ntp=time.example.com"); assert(!strcmp(c.more.ntp, "time.example.com"));
    OK("net.ntp=10.20.0.1"); BAD("net.ntp=a b"); BAD("net.ntp=x/y"); BAD("net.ntp=123456789012345678901234567890123456789012345678");
    { static char jj[4344]; assert(cfg_json(&c, jj, sizeof jj) && strstr(jj, "\"ntp\":\"10.20.0.1\"")); assert(cfg_lines(&c, jj, sizeof jj) && strstr(jj, "net.ntp=10.20.0.1\n"));
      static cfg_t t; cfg_defaults(&t); assert(cfg_parse(&t, jj, err, sizeof err) && !strcmp(t.more.ntp, "10.20.0.1")); }
    OK("net.ntp="); assert(!c.more.ntp[0]);
    assert(!c.more.topic[0]); OK("mqtt.topic=solar/gateway"); assert(!strcmp(c.more.topic, "solar/gateway"));
    OK("mqtt.topic=a.b-c_d/e1"); BAD("mqtt.topic=/lead"); BAD("mqtt.topic=trail/"); BAD("mqtt.topic=a//b"); BAD("mqtt.topic=a/+/b");
    BAD("mqtt.topic=a/#"); BAD("mqtt.topic=$SYS"); BAD("mqtt.topic=a b"); BAD("mqtt.topic=123456789012345678901234567890123456789012345678");
    { static char jj[4344]; assert(cfg_json(&c, jj, sizeof jj) && strstr(jj, "\"topic\":\"a.b-c_d/e1\"")); assert(cfg_lines(&c, jj, sizeof jj) && strstr(jj, "mqtt.topic=a.b-c_d/e1\n"));
      static cfg_t t; cfg_defaults(&t); assert(cfg_parse(&t, jj, err, sizeof err) && !strcmp(t.more.topic, "a.b-c_d/e1")); }
    OK("mqtt.topic="); assert(!c.more.topic[0]);
    assert(!c.more.mqtt_tls); OK("mqtt.tls=1"); assert(c.more.mqtt_tls == 1); BAD("mqtt.tls=2"); BAD("mqtt.tls="); BAD("mqtt.tls=on");
    { static char jj[4344]; assert(cfg_json(&c, jj, sizeof jj) && strstr(jj, "\"tls\":true,")); assert(cfg_lines(&c, jj, sizeof jj) && strstr(jj, "mqtt.tls=1\n"));
      static cfg_t t; cfg_defaults(&t); assert(cfg_parse(&t, jj, err, sizeof err) && t.more.mqtt_tls == 1); }
    OK("mqtt.enabled=1\nmqtt.host=broker.example.com");
    BAD("mqtt.host=10.0.0.5"); assert(strstr(err, "name, not its address"));     /* its certificate names the host */
    OK("mqtt.tls=0\nmqtt.host=10.0.0.5"); BAD("mqtt.tls=1");
    OK("mqtt.enabled=0\nmqtt.tls=1"); OK("mqtt.tls=0\nmqtt.host=");
    { char o[32];                                                  /* UTF-8 passes; anything else >= 0x80 is '?' */
      assert(json_str("\xC2\xB0" "C", o, sizeof o) && !strcmp(o, "\"\xC2\xB0" "C\""));
      assert(json_str("\xE2\x82\xAC\xF0\x9F\x98\x80", o, sizeof o) && !strcmp(o, "\"\xE2\x82\xAC\xF0\x9F\x98\x80\""));
      assert(json_str("a\xB0", o, sizeof o) && !strcmp(o, "\"a?\""));
      assert(json_str("a\xC2", o, sizeof o) && !strcmp(o, "\"a?\""));
      assert(json_str("\xC0\x80", o, sizeof o) && !strcmp(o, "\"??\""));
      assert(json_str("\xF5\x80\x80\x80", o, sizeof o) && !strcmp(o, "\"????\""));
      assert(json_str("\xE2\x82", o, sizeof o) && !strcmp(o, "\"??\"")); }
    { char o[32]; assert(json_str("a\nb\t\"\x01", o, sizeof o) && !strcmp(o, "\"a\\nb\\t\\\"?\"")); }   /* a file's line breaks survive */
    OK("# a settings file's note\nport3.baud=4800\n#port3.baud=x\n"); assert(c.port[2].baud == 4800);      /* notes are skipped */
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
    /* A Modbus gateway: TCP like a server, never listen-only, its own timeout. */
    OK("port3.enabled=1\nport3.mode=modbus\nport3.local_port=502"); assert(c.port[2].mode == MODE_MODBUS && cfg_modbus_timeout(&c, 2) == MB_TIMEOUT_MS);
    BAD("port3.listen_only=1"); BAD("port3.local_port=80"); BAD("port3.mode=gateway");
    OK("port3.modbus_timeout=250"); assert(cfg_modbus_timeout(&c, 2) == 250 && cfg_modbus_timeout(&c, 3) == MB_TIMEOUT_MS);
    BAD("port3.modbus_timeout=49"); BAD("port3.modbus_timeout=10001"); BAD("port3.modbus_timeout=");
    OK("port3.modbus_timeout=1000"); assert(c.more.modbus_timeout[2] == 0);          /* the default stays 0 */
    assert(!c.more.modbus_broadcast);
    OK("port3.modbus_broadcast=1"); assert(c.more.modbus_broadcast == 4);
    OK("port1.modbus_broadcast=1\nport3.modbus_broadcast=0"); assert(c.more.modbus_broadcast == 1);
    BAD("port3.modbus_broadcast=2"); BAD("port3.modbus_broadcast=");
    OK("port1.modbus_broadcast=0");
    OK("port3.enabled=1\nport4.enabled=1\nport4.mode=udp\nport4.local_port=502");     /* UDP 502 beside TCP 502 */
    BAD("port4.mode=server");                                                          /* TCP 502 twice */
    OK("port3.enabled=0\nport4.enabled=0\nport3.mode=server\nport3.local_port=4003\nport4.mode=server\nport4.local_port=4004");
    /* Every enabled port but a TCP client needs a local port, as in older firmware, which
     * otherwise refuses the whole record on a downgrade (set() checks each accepted one). */
    {
        static const char *const M[] = { "server", "udp", "modbus" };
        for (int m = 0; m < 3; m++) {
            char t[80];
            snprintf(t, sizeof t, "port4.enabled=1\nport4.mode=%s\nport4.local_port=0", M[m]);
            BAD(t); assert(strstr(err, "Port 4: enter a local port"));
        }
        OK("port4.enabled=1\nport4.mode=client\nport4.remote_host=h\nport4.remote_port=1\nport4.local_port=0");  /* unused */
        OK("port4.enabled=0\nport4.mode=server\nport4.remote_host=\nport4.remote_port=0\nport4.local_port=4004");
    }
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
    /* Devices: devN.* (N = 1..16). Their port need not be a Modbus gateway (checked when they run). */
    OK("port2.enabled=1\nport2.mode=modbus\nport2.local_port=502");
    BAD("port2.local_port=0"); assert(strstr(err, "Port 2: enter a local port"));   /* older firmware would load defaults */
    BAD("port1.local_port=0");                            /* a TCP server needs one */
    OK("dev1.profile=eg4-bms\ndev1.name=eg4m\ndev1.port=2\ndev1.address=16");
    assert(!strcmp(c.dev[0].profile, "eg4-bms") && !strcmp(c.dev[0].name, "eg4m") && c.dev[0].port == 2 && c.dev[0].address == 16);
    BAD("dev2.profile=eg4-bms");                                                   /* no name */
    BAD("dev2.profile=eg4-bms\ndev2.name=eg4m\ndev2.port=2\ndev2.address=1");      /* the name is taken */
    BAD("dev2.profile=eg4-bms\ndev2.name=p1\ndev2.port=2\ndev2.address=16");       /* so is the address */
    OK("dev2.profile=eg4-bms\ndev2.name=p1\ndev2.port=1\ndev2.address=1");        /* port 1 is no Modbus gateway: accepted */
    OK("dev2.profile=");
    BAD("dev3.name=lonely");                                                       /* no profile */
    BAD("dev2.profile=EG4"); BAD("dev2.name=Big Name"); BAD("dev2.address=248"); BAD("dev2.port=5");
    BAD("dev17.name=x"); BAD("dev0.name=x"); BAD("dev2.ids=maybe"); BAD("dev2.x=1");
    BAD("dev1.ids=esphome");                                                       /* needs the ESP32's prefix */
    OK("dev1.ids=esphome\ndev1.prefix=eg4_bms");
    OK("dev16.profile=eg4-bms\ndev16.name=p1\ndev16.port=2\ndev16.address=1\ndev16.label=Pack 1");
    OK("port2.mode=server");                                                       /* devices on it: accepted */
    OK("port2.mode=modbus");
    {
        char l[600];
        cfg_t t2 = c;
        assert(cfg_dev_lines(&c, 0, l, sizeof l) && strstr(l, "dev1.profile=eg4-bms\n") && strstr(l, "dev1.ids=esphome\n") &&
               strstr(l, "dev1.prefix=eg4_bms\n") && strstr(l, "dev1.address=16\n"));
        assert(cfg_dev_lines(&c, 5, l, sizeof l) == strlen("dev6.profile=\n") && !strcmp(l, "dev6.profile=\n"));
        memset(t2.dev, 0, sizeof t2.dev);
        cfg_dev_lines(&c, 15, l, sizeof l);
        assert(cfg_parse(&t2, l, err, sizeof err) && !memcmp(&t2.dev[15], &c.dev[15], sizeof c.dev[15]));
        for (size_t k = 0; k < 40; k++) assert(!cfg_dev_lines(&c, 0, l, k));      /* never cut short */
    }
    OK("dev16.profile=");
    assert(!c.dev[15].name[0] && !c.dev[15].port && !c.dev[15].label[0]);
    OK("dev1.profile=\nport2.mode=server\nport2.local_port=4002\nport2.enabled=0");

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
