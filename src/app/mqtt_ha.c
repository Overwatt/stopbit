#include "devmqtt.h"
#include "mqtt_ha.h"
#include "api.h"
#include "bridge.h"
#include "net.h"
#include "fw_id.h"
#include "events.h"
#include "mqtt_tls.h"
#include "clock.h"
#include "stm32h7xx_hal.h"
#include "lwip/apps/mqtt.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/dns.h"
#include <stdio.h>
#include <string.h>

#define COUNTER_MS 10000u          /* traffic counters at most this often */

/* ---- entities ------------------------------------------------------------ */
/* HA reads a select's "none" as "unknown", so parity none needs another word. */
#define NO_PARITY "no parity"
enum { CTRL = 1, DIAG = 2, HIDE = 4, BOOL = 8, PAR = 16, WHO = 32 };   /* HIDE: disabled by default in HA */
typedef struct { const char *comp, *key, *name, *extra; uint8_t flags; } ent_t;

#define TOTAL ",\"state_class\":\"total_increasing\""
#define SIZE  ",\"device_class\":\"data_size\",\"unit_of_measurement\":\"B\"" TOTAL
#define NUM(lo, hi) ",\"min\":" #lo ",\"max\":" #hi ",\"step\":1,\"mode\":\"box\""
static const ent_t PORT_E[] = {
    { "switch", "enabled",     "enabled",          "", CTRL | BOOL },
    { "select", "mode",        "mode",             ",\"options\":[\"server\",\"client\",\"udp\"]", CTRL },
    { "number", "baud",        "baud rate",        NUM(300, 2000000), CTRL },
    { "select", "data_bits",   "data bits",        ",\"options\":[\"7\",\"8\"]", CTRL },
    { "select", "parity",      "parity",           ",\"options\":[\"" NO_PARITY "\",\"even\",\"odd\"]", CTRL | PAR },
    { "select", "stop_bits",   "stop bits",        ",\"options\":[\"1\",\"2\"]", CTRL },
    { "number", "local_port",  "local port",       NUM(0, 65535), CTRL },
    { "text",   "remote_host", "remote host",      ",\"min\":0,\"max\":47", CTRL },
    { "number", "remote_port", "remote port",      NUM(0, 65535), CTRL },
    { "number", "gap_ms",      "packet silence",   NUM(0, 1000) ",\"unit_of_measurement\":\"ms\"", CTRL | HIDE },
    { "number", "packet_len",  "longest packet",   NUM(1, 1400) ",\"unit_of_measurement\":\"B\"", CTRL | HIDE },
    { "switch", "echo_cancel", "ignore own echo",  "", CTRL | BOOL | HIDE },
    { "switch", "loopback",    "loopback test",    "", CTRL | BOOL | HIDE },
    { "text",   "name",        "name",             ",\"min\":0,\"max\":23", CTRL | HIDE },
    { "sensor", "state",       "status",           "", 0 },
    { "sensor", "clients",     "connections",      ",\"state_class\":\"measurement\"", DIAG },
    { "sensor", "serial_rx",   "from serial",      SIZE, 0 },
    { "sensor", "serial_tx",   "to serial",        SIZE, 0 },
    { "sensor", "errors",      "errors",           TOTAL, DIAG },
    { "binary_sensor", "alert", "alert",           ",\"device_class\":\"problem\"", BOOL },
    { "sensor", "turned_away", "turned away",      TOTAL, DIAG | WHO },
};
static const ent_t DEV_E[] = {
    { "text",   "name",       "Name",       ",\"min\":0,\"max\":31", CTRL },
    { "button", "restart",    "Restart",    ",\"device_class\":\"restart\"", CTRL },
    { "sensor", "ip",         "IP address", "", DIAG },
    { "sensor", "last_error", "Last rejected change", "", DIAG },
};
#define NPE (int)(sizeof PORT_E / sizeof PORT_E[0])
#define NDE (int)(sizeof DEV_E / sizeof DEV_E[0])
#define NENT (PORTS * NPE + NDE)

/* ---- state --------------------------------------------------------------- */
enum { M_OFF, M_RESOLVING, M_CONNECTING, M_UP, M_WAIT, M_REMOVING, M_MOVING };
static const char *const NAMES[] = { "off", "connecting", "connecting", "connected", "retrying", "removing", "connected" };

static mqtt_client_t s_c;
static mqtt_cfg_t s_run;                /* what the connection was made with */
static bool     s_tls;                  /* and whether over TLS */
static char     s_name[32];             /* device name last announced */
static uint8_t  s_state;
static uint32_t s_retry_at, s_backoff, s_gen;
static const char *s_err = "";
static int      s_disc;                 /* next entity to announce; NENT = done */
static bool     s_online, s_sub, s_lost, s_leaving;
static bool     s_withdrawn;            /* not announcing, and HA has been told once */
static mqtt_connection_status_t s_lost_why;
static uint32_t s_port_hash[PORTS], s_port_cnt[PORTS], s_port_at[PORTS], s_dev_hash;
static char     s_reject[96];           /* last refused command */
static char     s_cmd[160];             /* command waiting for the main loop */
static bool     s_cmd_ready, s_restart;
static char     s_buf[1400];
/* Topics start with s_base: the setting, or the host name. Home Assistant's IDs stay
 * the host name, so its entities follow a move. s_old: the base being cleared up. */
static char     s_base[48], s_old[48];
static int      s_clear;                /* retained topics under s_old cleared so far */

const char *mqtt_ha_state(void) { return NAMES[s_state]; }
const char *mqtt_ha_error(void) { return s_err; }
bool mqtt_ha_tls(void) { return s_tls; }

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h | 1u;                      /* 0 means "never published" */
}

/* ---- incoming commands --------------------------------------------------- */
static char s_in_key[48], s_in_val[96];
static uint32_t s_in_len;
static bool s_in_ok;

static void cb_topic(void *arg, const char *topic, u32_t tot)
{
    size_t n = strlen(s_base);
    (void)arg;
    s_in_ok = false;
    s_in_len = 0;
    if (!s_run.control || (s_c.rx_buffer[0] & 1u)) return;      /* retained: stale */
    if (tot >= sizeof s_in_val) return;
    if (strncmp(topic, s_base, n) || topic[n] != '/') return;
    const char *obj = topic + n + 1, *set = strstr(obj, "/set/");
    if (!set) return;
    const char *field = set + 5;
    int olen = (int)(set - obj);
    bool port = olen == 5 && !strncmp(obj, "port", 4), dev = olen == 6 && !strncmp(obj, "device", 6);
    if (!port && !(dev && (!strcmp(field, "name") || !strcmp(field, "restart")))) return;
    if (!strcmp(field, "listen_only") || !strcmp(field, "access")) return;   /* not over MQTT */
    for (const char *p = field; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || *p == '_')) return;
    if (snprintf(s_in_key, sizeof s_in_key, "%.*s.%s", olen, obj, field) >= (int)sizeof s_in_key) return;
    s_in_ok = true;
}

static void cb_data(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    (void)arg;
    if (!s_in_ok) return;
    if (len > sizeof s_in_val - 1 - s_in_len) { s_in_ok = false; return; }
    if (len) memcpy(s_in_val + s_in_len, data, len);
    s_in_len += len;
    if (!(flags & MQTT_DATA_FLAG_LAST)) return;
    s_in_ok = false;
    s_in_val[s_in_len] = 0;
    if (strlen(s_in_val) != s_in_len || strpbrk(s_in_val, "\r\n")) return;
    if (!strcmp(s_in_key, "device.restart")) {
        if (!strcmp(s_in_val, "PRESS")) s_restart = true;
        return;
    }
    const char *v = !strcmp(s_in_val, "ON") ? "1" : !strcmp(s_in_val, "OFF") ? "0" :
                    !strcmp(s_in_val, NO_PARITY) ? "none" : s_in_val;
    /* HA sends whole numbers as "9600", but some clients send "9600.0". */
    size_t l = strlen(s_in_val);
    if (l > 2 && !strcmp(s_in_val + l - 2, ".0")) s_in_val[l - 2] = 0;
    if (s_cmd_ready) {                  /* one per loop pass; a flood gets dropped */
        snprintf(s_reject, sizeof s_reject, "Too many changes at once; %s ignored", s_in_key);
        return;
    }
    snprintf(s_cmd, sizeof s_cmd, "%s=%s", s_in_key, v);
    s_cmd_ready = true;
}

/* ---- connection ---------------------------------------------------------- */
static uint8_t s_told;                  /* the log knows: 1 up, 2 down */
static void retry(const char *why)
{
    if (s_told != 2) { events_add(EV_MQTT_DOWN, 0, 0, why); s_told = 2; }
    s_err = why;
    s_backoff = s_backoff ? s_backoff * 2 : 2000;
    if (s_backoff > 60000) s_backoff = 60000;
    s_retry_at = HAL_GetTick() + s_backoff;
    s_state = M_WAIT;
}

static void cb_conn(mqtt_client_t *c, void *arg, mqtt_connection_status_t st)
{
    (void)c;
    if ((uint32_t)arg != s_gen) return;         /* from a connection we dropped */
    if (st == MQTT_CONNECT_ACCEPTED) {
        if (s_told != 1) { events_add(EV_MQTT_UP, 0, 0, s_run.host); s_told = 1; }
        s_state = M_UP;
        s_err = "";
        s_backoff = 0;
        s_disc = s_run.no_announce && s_withdrawn ? NENT : 0;
        s_online = s_sub = false;
        s_dev_hash = 0;
        devmqtt_restart();
        memset(s_port_hash, 0, sizeof s_port_hash);
        memset(s_port_at, 0, sizeof s_port_at);
        return;
    }
    s_lost = true;                              /* lwIP is mid-close: act in poll */
    s_lost_why = st;
}

static const char *why(mqtt_connection_status_t st)
{
    switch ((int)st) {
    case MQTT_CONNECT_REFUSED_PROTOCOL_VERSION: return "Broker refused the protocol version";
    case MQTT_CONNECT_REFUSED_IDENTIFIER:       return "Broker refused the client name";
    case MQTT_CONNECT_REFUSED_SERVER:           return "Broker is unavailable";
    case MQTT_CONNECT_REFUSED_USERNAME_PASS:    return "Wrong user name or password";
    case MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_:  return "Broker did not authorise this user";
    case MQTT_CONNECT_TIMEOUT:                  return "Broker did not respond";
    default:                                    return "Could not connect, or the connection was lost";
    }
}

static void connect_to(const ip_addr_t *ip)
{
    static char will[56];
    static struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof ci);
    snprintf(will, sizeof will, "%s/status", s_base);
    ci.client_id = g_hostname;
    ci.client_user = s_run.user[0] ? s_run.user : NULL;
    ci.client_pass = s_run.password[0] ? s_run.password : NULL;
    ci.keep_alive = 30;
    ci.will_topic = will;
    ci.will_msg = "offline";
    ci.will_retain = 1;
    ci.tls_config = s_tls ? mqtt_tls_config(s_run.host) : NULL;
    s_lost = false;
    s_state = M_CONNECTING;
    s_gen++;
    mqtt_set_inpub_callback(&s_c, cb_topic, cb_data, NULL);
    if (mqtt_client_connect(&s_c, ip, (u16_t)s_run.port, cb_conn, (void *)s_gen, &ci) != ERR_OK)
        retry("Could not start the connection");
    else
        mqtt_set_inpub_callback(&s_c, cb_topic, cb_data, NULL);
}

static void cb_dns(const char *name, const ip_addr_t *ip, void *arg)
{
    (void)name;
    if (s_state != M_RESOLVING || (uint32_t)arg != s_gen) return;
    if (ip) connect_to(ip);
    else retry("Broker name not found");
}

static void start(void)
{
    ip_addr_t ip;
    if (s_tls && !clock_known()) { retry("Waiting for the time, to check the broker's certificate"); return; }
    s_state = M_RESOLVING;
    s_gen++;
    err_t e = dns_gethostbyname(s_run.host, &ip, cb_dns, (void *)s_gen);
    if (e == ERR_OK) connect_to(&ip);
    else if (e != ERR_INPROGRESS) retry("Broker name not found");
}

static void drop(void)
{
    s_gen++;                                    /* orphan pending callbacks */
    mqtt_disconnect(&s_c);
    s_lost = false;
    s_state = M_OFF;
}

void mqtt_ha_configure(const cfg_t *c)
{
    const mqtt_cfg_t *m = &c->mqtt;
    char base[sizeof s_base];
    snprintf(base, sizeof base, "%s", c->more.topic[0] ? c->more.topic : g_hostname);
    if (strcmp(base, s_base)) {
        /* Up: clear what is retained under the old base, then reconnect, so the last
         * will moves too. Otherwise it waits for the next connection. */
        if (s_base[0] && s_state == M_UP && s_run.enabled && m->enabled) {
            memcpy(s_old, s_base, sizeof s_old);
            s_clear = 0;
            s_state = M_MOVING;
        }
        memcpy(s_base, base, sizeof s_base);
        s_sub = false;
        devmqtt_restart();
    }
    bool tls = c->more.mqtt_tls;
    bool same_conn = m->port == s_run.port && !strcmp(m->host, s_run.host) && tls == s_tls &&
                     !strcmp(m->user, s_run.user) && !strcmp(m->password, s_run.password);

    if (strcmp(s_name, c->device_name) || m->control != s_run.control || m->no_announce != s_run.no_announce) {
        if (m->no_announce != s_run.no_announce) s_withdrawn = false;
        s_disc = m->no_announce && s_withdrawn ? NENT : 0;      /* announce again */
        s_sub = false;
        devmqtt_restart();
        memcpy(s_name, c->device_name, sizeof s_name);
    }
    if (s_run.enabled && !m->enabled && (s_state == M_UP || s_state == M_MOVING)) {
        s_run.enabled = 0;                      /* take our entities out of HA first */
        s_run.control = 0;
        s_state = M_REMOVING;
        s_disc = 0;
        return;
    }
    bool restart = m->enabled != s_run.enabled || !same_conn;
    s_run = *m;
    s_tls = tls;
    if (!restart) return;
    if (s_state != M_OFF) drop();
    s_err = "";
    s_backoff = 0;
    if (m->enabled) start();
}

/* ---- publishing ---------------------------------------------------------- */
static bool pub(const char *topic, const char *payload, bool retain)
{
    return mqtt_publish(&s_c, topic, payload, (u16_t)strlen(payload), 0, retain, NULL, NULL) == ERR_OK;
}

/* Announces entity `n`, or withdraws it with an empty payload. */
static bool announce(int n, bool remove)
{
    char topic[96], obj[40], dname[72], ip[16];
    const ent_t *e;
    int port = n / NPE + 1;
    bool dev = n >= PORTS * NPE;
    e = dev ? &DEV_E[n - PORTS * NPE] : &PORT_E[n % NPE];
    if (dev) snprintf(obj, sizeof obj, "%s", e->key);
    else snprintf(obj, sizeof obj, "port%d_%s", port, e->key);
    snprintf(topic, sizeof topic, "homeassistant/%s/%s/%s/config", e->comp, g_hostname, obj);
    if (remove || s_run.no_announce || ((e->flags & CTRL) && !s_run.control)) return pub(topic, "", true);

    char st[16], label[40];
    if (dev) { strcpy(st, "device"); snprintf(label, sizeof label, "%s", e->name); }
    else {
        snprintf(st, sizeof st, "port%d", port);
        snprintf(label, sizeof label, "Port %d %s", port, e->name);
    }
    json_str(s_name[0] ? s_name : g_hostname, dname, sizeof dname);
    net_ip_str(ip);
    size_t n0 = 0, max = sizeof s_buf;
#define ADD(...) do { int w = snprintf(s_buf + n0, max - n0, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= max - n0) { return true; } n0 += (size_t)w; } while (0)
    ADD("{\"~\":\"%s\",\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"availability_topic\":\"~/status\"",
        s_base, label, g_hostname, obj);
    if (strcmp(e->comp, "button")) {
        ADD(",\"state_topic\":\"~/%s\"", st);
        if (e->flags & BOOL) ADD(",\"value_template\":\"{{'ON' if value_json.%s else 'OFF'}}\"", e->key);
        else if (e->flags & PAR) ADD(",\"value_template\":\"{{'" NO_PARITY "' if value_json.%s == 'none' else value_json.%s}}\"", e->key, e->key);
        else ADD(",\"value_template\":\"{{value_json.%s}}\"", e->key);
    }
    if (e->flags & CTRL) ADD(",\"command_topic\":\"~/%s/set/%s\",\"entity_category\":\"config\"", st, e->key);
    if (e->flags & WHO)
        ADD(",\"json_attributes_topic\":\"~/%s\",\"json_attributes_template\":\"{{ {'address': value_json.turned_away_ip} | tojson }}\"", st);
    if (e->flags & DIAG) ADD(",\"entity_category\":\"diagnostic\"");
    if (e->flags & HIDE) ADD(",\"enabled_by_default\":false");
    ADD("%s,\"device\":{\"identifiers\":[\"%s\"],\"name\":%s,\"manufacturer\":\"USR (open firmware)\","
        "\"model\":\"N540\",\"sw_version\":\"%s\",\"configuration_url\":\"http://%s/\"}}",
        e->extra, g_hostname, dname, g_fw_id.version, ip);
    return pub(topic, s_buf, true);
}

/* What a port's message is made from, as a number: making the message costs some thirty
 * times this, and the main loop asks every pass. */
static uint32_t port_mark(const port_cfg_t *p, const bridge_brief_t *b)
{
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < sizeof *p; k++) h = (h ^ ((const uint8_t *)p)[k]) * 16777619u;
    for (const char *s = b->state; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (const char *s = b->alert; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    h = (h ^ (uint32_t)b->clients) * 16777619u;
    return h | 1u;                              /* 0 means "never published" */
}

static bool publish_port(const cfg_t *c, int i)
{
    bridge_brief_t q;
    bridge_info_t b;
    char topic[64];
    bridge_brief(i, &q);
    uint32_t h = port_mark(&c->port[i], &q), cnt = q.serial_rx * 31u + q.serial_tx * 17u + q.errors * 7u + q.denied;
    uint32_t now = HAL_GetTick();
    bool first = s_port_hash[i] == 0;
    if (!first && h == s_port_hash[i] && (cnt == s_port_cnt[i] || now - s_port_at[i] < COUNTER_MS)) return true;
    bridge_info(i, &b);
    size_t n0 = cfg_port_json(&c->port[i], s_buf, sizeof s_buf), max = sizeof s_buf;
    if (!n0) return true;
    n0--;                                       /* reopen the object */
    ADD(",\"state\":\"%s\",\"clients\":%d,\"alert\":\"%s\"", b.state, b.clients, b.alert);
    ADD(",\"serial_rx\":%lu,\"serial_tx\":%lu,\"errors\":%lu,\"turned_away\":%lu,\"turned_away_ip\":\"%s\"}",
        (unsigned long)b.serial_rx, (unsigned long)b.serial_tx, (unsigned long)b.errors, (unsigned long)b.denied, b.denied_ip);
    snprintf(topic, sizeof topic, "%s/port%d", s_base, i + 1);
    if (!pub(topic, s_buf, true)) return false;
    s_port_hash[i] = h;
    s_port_cnt[i] = cnt;
    s_port_at[i] = now;
    return true;
}
#undef ADD

static bool publish_device(void)
{
    char topic[64], ip[16], name[72], rej[200];
    net_ip_str(ip);
    uint32_t h = hash(s_name) * 31u + hash(s_reject) * 17u + hash(ip);     /* as above: not the message itself */
    h |= 1u;
    if (h == s_dev_hash) return true;
    json_str(s_name, name, sizeof name);
    json_str(s_reject[0] ? s_reject : "Nothing", rej, sizeof rej);
    snprintf(s_buf, sizeof s_buf, "{\"name\":%s,\"ip\":\"%s\",\"last_error\":%s}", name, ip, rej);
    snprintf(topic, sizeof topic, "%s/device", s_base);
    if (!pub(topic, s_buf, true)) return false;
    s_dev_hash = h;
    return true;
}

void mqtt_ha_shutdown(void)
{
    char topic[64];
    if (s_state == M_UP && mqtt_client_is_connected(&s_c)) {
        snprintf(topic, sizeof topic, "%s/status", s_base);
        pub(topic, "offline", true);
    }
    if (s_state != M_OFF) drop();       /* queued data still goes out before the FIN */
    s_leaving = true;
}

void mqtt_ha_poll(const cfg_t *c)
{
    char topic[64];
    if (s_leaving) return;
    mqtt_tls_poll();
    if (s_lost) {
        s_lost = false;
        if (s_state == M_REMOVING) { drop(); return; }
        if (s_state == M_MOVING) s_state = M_UP;      /* the retry connects under the new base */
        if (s_state != M_OFF) retry(s_tls && mqtt_tls_error()[0] ? mqtt_tls_error() : why(s_lost_why));
    }
    if (s_state == M_WAIT && (int32_t)(HAL_GetTick() - s_retry_at) >= 0) start();

    if (s_cmd_ready) {
        char err[96];
        s_cmd_ready = false;
        if (s_run.control && !api_set_line(s_cmd, err, sizeof err)) {
            snprintf(s_reject, sizeof s_reject, "%s", err);
            memset(s_port_hash, 0, sizeof s_port_hash);     /* put HA's display right */
            s_dev_hash = 0;
        } else {
            s_reject[0] = 0;
        }
    }
    if (s_restart) {
        s_restart = false;
        if (s_run.control) { events_add(EV_RESTART, 0, 0, "mqtt"); api_restart(); }
    }

    if (s_state == M_REMOVING) {
        while (s_disc < NENT) {
            if (!announce(s_disc, true)) return;
            s_disc++;
        }
        snprintf(topic, sizeof topic, "%s/status", s_base);
        if (s_disc == NENT && !pub(topic, "offline", true)) return;
        s_disc = NENT + 1;
        if (s_c.output.get != s_c.output.put) return;       /* let it drain */
        drop();
        return;
    }
    if (s_state == M_MOVING) {                  /* empty and retained: the broker forgets it */
        static const char *const OLD[] = { "port1", "port2", "port3", "port4", "device", "status" };
        while (s_clear < (int)(sizeof OLD / sizeof OLD[0])) {
            snprintf(topic, sizeof topic, "%s/%s", s_old, OLD[s_clear]);
            if (!pub(topic, "", true)) return;
            s_clear++;
        }
        if (s_c.output.get != s_c.output.put) return;       /* let it drain */
        drop();
        s_err = "";
        s_backoff = 0;
        start();
        return;
    }
    if (s_state != M_UP || !mqtt_client_is_connected(&s_c)) return;

    if (s_disc < NENT) {                        /* a few per pass: keep the bridge live */
        if (announce(s_disc, false) && ++s_disc == NENT) s_withdrawn = s_run.no_announce;
        return;
    }
    for (int i = 0; i < PORTS; i++)
        if (!publish_port(c, i)) return;
    if (!publish_device()) return;
    if (!s_online) {
        snprintf(topic, sizeof topic, "%s/status", s_base);
        if (!pub(topic, "online", true)) return;
        s_online = true;
    }
    if (!s_sub && s_run.control) {
        snprintf(topic, sizeof topic, "%s/+/set/+", s_base);
        if (mqtt_subscribe(&s_c, topic, 0, NULL, NULL) != ERR_OK) return;
        s_sub = true;
    }
    if (!devmqtt_poll(pub, s_base, g_hostname, !s_run.no_announce, HAL_GetTick())) return;
}
