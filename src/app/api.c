#include "api.h"
#include "addrs.h"
#include "clock.h"
#include "events.h"
#include "cfg.h"
#include "cfg_store.h"
#include "bridge.h"
#include "device.h"
#include "profiles.h"
#include "cli.h"
#include "hwinfo.h"
#include "history.h"
#include "fwlib.h"
#include "tls.h"
#include "acme.h"
#include "uart485.h"
#include "net.h"
#include "mqtt_ha.h"
#include "mqtt_tls.h"
#include "metrics.h"
#include "gcm_hw.h"
#include "eth_mac.h"
#include "sha256.h"
#include "slots.h"
#include "fw_id.h"
#include "wdt.h"
#include "web_page.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_MS 3000u            /* batch settings that arrive one by one */
#define KEEP_MS 120000u          /* unconfirmed network settings revert after this */

static cfg_t     s_cfg;          /* what is saved */
/* A copy to change and then commit, or to show. One for all who need one: each is done
 * with it before it returns, and none calls another (change() below keeps its own, as
 * it is handed this one). 3 KB apiece adds up. */
static cfg_t     s_edit;
static net_cfg_t s_trial;        /* running but not saved */
static bool      s_pending;
static uint32_t  s_deadline;
static bool      s_net_dirty;
static uint32_t  s_net_at;
static bool      s_unsaved;
static bool      s_restart;
static uint32_t  s_restart_at;
static uint32_t  s_save_at;

/* For the event log: what a change touched. The password has its own event. */
static void note(const cfg_t *was, const cfg_t *now)
{
    char what[44] = "";
    uint32_t ports = 0;
#define WHAT(x) snprintf(what + strlen(what), sizeof what - strlen(what), "%s%s", what[0] ? "," : "", x)
    if (strcmp(was->device_name, now->device_name)) WHAT("name");
    if (memcmp(&was->net, &now->net, sizeof was->net)) WHAT("network");
    if (strcmp(was->more.ntp, now->more.ntp)) WHAT("time server");
    if (memcmp(&was->mqtt, &now->mqtt, sizeof was->mqtt) || strcmp(was->more.topic, now->more.topic) ||
        was->more.mqtt_tls != now->more.mqtt_tls) WHAT("mqtt");
#undef WHAT
    for (int i = 0; i < PORTS; i++)
        if (memcmp(&was->port[i], &now->port[i], sizeof was->port[i]) || memcmp(&was->access[i], &now->access[i], sizeof was->access[i]) ||
            cfg_quiet_min(was, i) != cfg_quiet_min(now, i)) ports |= 1u << i;
    if (what[0] || ports) events_add(EV_SETTINGS, 0, ports, what);
}

/* Saves `c` and makes it current. On failure nothing changes. */
static bool commit(const cfg_t *c)
{
    if (!cfg_save(c)) return false;
    note(&s_cfg, c);
    s_cfg = *c;
    s_unsaved = false;
    return true;
}

static void run_net(const net_cfg_t *n)
{
    net_set(n->dhcp, n->ip, n->mask, n->gw, n->dns);
}

void api_init(void)
{
    cfg_load(&s_cfg);
    clock_server(s_cfg.more.ntp);
    if (!s_cfg.net.dhcp) run_net(&s_cfg.net);
    bridge_configure(&s_cfg);
    device_configure(&s_cfg);
    mqtt_ha_configure(&s_cfg);
    hwinfo_init();
}

void api_restart(void)
{
    if (s_unsaved) commit(&s_cfg);
    mqtt_ha_shutdown();
    for (uint32_t t0 = HAL_GetTick(); HAL_GetTick() - t0 < 300;) {
        wdt_feed();
        net_poll();
    }
    events_flush();
    bkp_set(BKP_CRASHES, 0);             /* deliberate, not a crash */
    NVIC_SystemReset();
}

/* The reload button. If flash cannot be written, nothing changes. */
static void reset_access(void)
{
    s_edit = s_cfg;
    cfg_reset_access(&s_edit);
    events_add(EV_BUTTON, 0, 0, NULL);
    if (commit(&s_edit)) api_restart();
}

bool api_set_line(char *line, char *err, size_t errlen)
{
    cfg_t *c = &s_edit;
    *c = s_cfg;
    if (!cfg_parse(c, line, err, errlen) || !cfg_validate(c, err, errlen)) return false;
    if (!memcmp(c, &s_cfg, sizeof *c)) return true;
    note(&s_cfg, c);
    s_cfg = *c;
    s_unsaved = true;
    s_save_at = HAL_GetTick() + SAVE_MS;
    bridge_configure(&s_cfg);
    device_configure(&s_cfg);
    mqtt_ha_configure(&s_cfg);
    clock_server(s_cfg.more.ntp);
    return true;
}

void api_poll(void)
{
    uint32_t now = HAL_GetTick();
    if (hwinfo_button()) reset_access();
    mqtt_ha_poll(&s_cfg);
    if (s_restart && (int32_t)(now - s_restart_at) >= 0) { events_add(EV_RESTART, 0, 0, "console"); api_restart(); }
    if (s_unsaved && (int32_t)(now - s_save_at) >= 0) {
        s_save_at = now + 30000;        /* if it fails, try again later */
        commit(&s_cfg);
    }
    if (s_net_dirty && (int32_t)(now - s_net_at) >= 0) {
        s_net_dirty = false;
        run_net(s_pending ? &s_trial : &s_cfg.net);
    }
    if (s_pending && (int32_t)(now - s_deadline) >= 0) {
        s_pending = false;
        events_add(EV_NET_REVERTED, 0, 0, NULL);
        run_net(&s_cfg.net);
    }
}

const uint8_t *api_web_key(void) { return s_cfg.web_set ? s_cfg.web_key : NULL; }
/* The setting's name is in any firmware that knows it. Older firmware would let
 * clients transmit on a port that must only listen. */
const char *api_fw_need(void)
{
    for (int i = 0; i < PORTS; i++) if (s_cfg.port[i].listen_only) return "listen_only";
    return NULL;
}

const uint8_t *api_page(uint32_t *len) { *len = sizeof WEB_PAGE; return WEB_PAGE; }
const char *api_page_tag(void) { return WEB_PAGE_TAG; }

static void hex(const uint8_t *in, int n, char *out)
{
    for (int i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", in[i]);
}

static bool unhex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return false;
            v = v << 4 | (unsigned)d;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

/* The running firmware has not been confirmed yet. */
static bool on_trial(void)
{
    static bool kept;
    if (!kept) kept = slots_is_confirmed((int)bkp_get(BKP_SLOT));
    return !kept;
}

static int get_one(const char *path, char *out, size_t max)
{
    if (!strcmp(path, "/api/config")) {
        s_edit = s_cfg;
        if (s_pending) s_edit.net = s_trial;
        return (int)cfg_json(&s_edit, out, max);
    }
    if (!strcmp(path, "/api/login")) {
        char salt[33] = "";
        if (s_cfg.web_set) hex(s_cfg.web_salt, 16, salt);
        int n = snprintf(out, max, "{\"password_set\":%s,\"salt\":\"%s\"}\n",
                         s_cfg.web_set ? "true" : "false", salt);
        return n > 0 && (size_t)n < max ? n : 0;
    }
    if (!strncmp(path, "/api/history/", 13) && path[13] >= '1' && path[13] <= '0' + PORTS && !path[14])
        return (int)history_json(path[13] - '1', out, max);
    if (!strncmp(path, "/api/access/", 12) && path[12] >= '1' && path[12] <= '0' + PORTS && !path[13])
        return (int)bridge_access_json(path[12] - '1', out, max);
    if (!strcmp(path, "/api/tls")) return (int)tls_json(out, max);
    if (!strcmp(path, "/api/acme")) return (int)acme_json(out, max);
    if (!strcmp(path, "/api/tls/csr")) return (int)tls_csr(out, max);
    if (!strcmp(path, "/api/library")) return (int)fwlib_json((int)bkp_get(BKP_SLOT), out, max);
    if (!strcmp(path, "/api/hardware")) return (int)hwinfo_json(out, max);
    if (!strcmp(path, "/api/events") || !strncmp(path, "/api/events?before=", 19))
        return (int)events_json(path[11] ? (uint32_t)strtoul(path + 19, NULL, 10) : 0, 30, out, max);
    if (strcmp(path, "/api/state")) return -1;

    char ip[16], mask[16], gw[16];
    net_addrs(ip, mask, gw);
    int left = s_pending ? (int)(s_deadline - HAL_GetTick()) / 1000 : 0;
    size_t n = (size_t)snprintf(out, max,
        "{\"version\":\"%s\",\"uptime_s\":%lu,\"host\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"ip\":\"%s\",\"mask\":\"%s\",\"gateway\":\"%s\",\"speed\":%d,\"full_duplex\":%s,"
        "\"net_pending\":%s,\"net_pending_s\":%d,\"fw_trial\":%s,\"slot\":%d,\"boots\":%lu,\"crashes\":%lu,\"reset\":\"%lx\","
        "\"mqtt\":{\"state\":\"%s\",\"error\":\"%s\",\"tls\":%s,\"tls_version\":\"%s\",\"tls_until\":\"%s\"},\"clock\":",
        g_fw_id.version, (unsigned long)(HAL_GetTick() / 1000), g_hostname,
        g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5], ip, mask, gw,
        g_eth.speed100 ? 100 : 10, g_eth.full_duplex ? "true" : "false",
        s_pending ? "true" : "false", left, on_trial() ? "true" : "false", (int)bkp_get(BKP_SLOT),
        (unsigned long)bkp_get(BKP_BOOTS), (unsigned long)bkp_get(BKP_CRASHES),
        (unsigned long)bkp_get(BKP_RSTLOG), mqtt_ha_state(), mqtt_ha_error(), mqtt_ha_tls() ? "true" : "false",
        mqtt_tls_version() ? mqtt_tls_version() : "", mqtt_tls_until() ? mqtt_tls_until() : "");
    if (n >= max) return 0;
    size_t k = clock_json(out + n, max - n);
    if (!k || n + k + 10 >= max) return 0;
    n += k;
    memcpy(out + n, ",\"ports\":", 10);  /* with its NUL */
    n += 9;
    size_t b = bridge_json(out + n, max - n);
    if (!b) return 0;
    n += b;
    int up[PORTS], all[PORTS];                          /* each port's devices: [online, configured] */
    device_count(up, all);
    for (int i = 0; i < PORTS; i++) {
        int w = snprintf(out + n, max - n, "%s[%d,%d]", i ? "," : ",\"devices\":[", up[i], all[i]);
        if (w < 0 || (size_t)w >= max - n) return 0;
        n += (size_t)w;
    }
    if (n + 2 > max) return 0;
    memcpy(out + n, "],", 2);
    n += 2;
    size_t ad = addrs_json(out + n, max - n);           /* under 200 bytes */
    if (!ad || n + ad + 3 > max) return 0;
    n += ad;
    memcpy(out + n, "}\n", 3);
    return (int)(n + 2);
}

/* Optional "mqtt.password_enc=<128 hex>" line: 64 zero-padded bytes XORed with
 * HMAC(key, "mqttpw0\n" nonce) and HMAC(key, "mqttpw1\n" nonce). Takes the line
 * out of the body. False if it is malformed. */
static bool take_mqtt_password(api_req_t *rq, cfg_t *c, bool *found)
{
    static const char K[] = "mqtt.password_enc=";
    uint8_t v[64], pad[64];
    char *p = strstr(rq->body, K);
    *found = p != NULL;
    if (!p) return true;
    if ((p != rq->body && p[-1] != '\n') || !unhex(p + sizeof K - 1, v, 64)) return false;
    hmac_sha256(rq->key, "mqttpw0\n", 8, rq->nonce, strlen(rq->nonce), pad);
    hmac_sha256(rq->key, "mqttpw1\n", 8, rq->nonce, strlen(rq->nonce), pad + 32);
    for (int i = 0; i < 64; i++) v[i] ^= pad[i];
    char *e = strchr(p, '\n');
    if (e) memmove(p, e + 1, strlen(e + 1) + 1);
    else *p = 0;
    size_t n = 0;
    while (n < 64 && v[n]) {
        if (v[n] < 0x20 || v[n] == 0x7f) return false;
        n++;
    }
    for (size_t i = n; i < 64; i++) if (v[i]) return false;
    if (n > 63) return false;
    memcpy(c->mqtt.password, v, 64);
    return true;
}

/* Validates `body` against what is running and applies it all, or nothing. */
static const char *change(cfg_t *c, char *body, char *out, size_t max);

static const char *post_config(api_req_t *rq, char *out, size_t max)
{
    bool pw;
    s_edit = s_cfg;
    if (!take_mqtt_password(rq, &s_edit, &pw)) {
        snprintf(out, max, "That MQTT password cannot be used");
        return "400 Bad Request";
    }
    const char *st = change(&s_edit, rq->body, out, max);
    if (st[0] == '2') snprintf(out, max, "{\"net_pending\":%s}", s_pending ? "true" : "false");
    return st;
}

static const char *change(cfg_t *cp, char *body, char *out, size_t max)
{
    static cfg_t c;
    c = *cp;
    net_cfg_t shown = s_pending ? s_trial : s_cfg.net;
    c.net = shown;                       /* edit what the page is showing */
    if (!cfg_parse(&c, body, out, max) || !cfg_validate(&c, out, max)) return "400 Bad Request";

    net_cfg_t want = c.net;
    bool net_changed = memcmp(&want, &shown, sizeof want) != 0;
    c.net = s_cfg.net;                   /* network is saved only once kept */
    if (memcmp(&c, &s_cfg, sizeof c)) {
        if (!commit(&c)) {
            snprintf(out, max, "Could not write settings to flash; nothing changed");
            return "500 Internal Server Error";
        }
        bridge_configure(&s_cfg);
        device_configure(&s_cfg);
        mqtt_ha_configure(&s_cfg);
        clock_server(s_cfg.more.ntp);
    }
    if (net_changed) {
        s_trial = want;
        s_pending = memcmp(&want, &s_cfg.net, sizeof want) != 0;
        if (s_pending) events_add(EV_NET_TRIAL, 0, 0, NULL);
        s_deadline = HAL_GetTick() + KEEP_MS;
        s_net_dirty = true;
        s_net_at = HAL_GetTick() + 500;  /* let this reply out first */
    }
    return "200 OK";
}

/* Body: "salt=<32 hex>\nkey=<64 hex>". `key` is the new key XORed with
 * HMAC(signing key, "newkey\n" nonce), so it is not readable on the wire. */
static const char *post_password(api_req_t *rq, char *out, size_t max)
{
    cfg_t *c = &s_edit;
    uint8_t pad[32], key[32], salt[16];
    if (!strcmp(rq->body, "clear")) {
        if (!rq->machine) {
            snprintf(out, max, "Only the update key can clear the password");
            return "403 Forbidden";
        }
        *c = s_cfg;
        c->web_set = 0;
        memset(c->web_salt, 0, sizeof c->web_salt);
        memset(c->web_key, 0, sizeof c->web_key);
        if (!commit(c)) {
            snprintf(out, max, "Could not write to flash; the password is unchanged");
            return "500 Internal Server Error";
        }
        snprintf(out, max, "{}");
        return "200 OK";
    }
    char *s = strstr(rq->body, "salt="), *k = strstr(rq->body, "key=");
    if (!s || !k || !unhex(s + 5, salt, 16) || !unhex(k + 4, key, 32)) {
        snprintf(out, max, "Malformed password request");
        return "400 Bad Request";
    }
    hmac_sha256(rq->key, "newkey\n", 7, rq->nonce, strlen(rq->nonce), pad);
    *c = s_cfg;
    for (int i = 0; i < 32; i++) c->web_key[i] = key[i] ^ pad[i];
    memcpy(c->web_salt, salt, 16);
    c->web_set = 1;
    if (!commit(c)) {
        snprintf(out, max, "Could not write the password to flash; it is unchanged");
        return "500 Internal Server Error";
    }
    events_add(EV_PASSWORD, 0, 0, NULL);
    snprintf(out, max, "{}");
    return "200 OK";
}

/* ---- console hooks (cli.h) ------------------------------------------------ */
const cfg_t *cli_cfg(void) { return &s_cfg; }

bool cli_change(char *lines, char *err, size_t errlen, bool *pending)
{
    bool ok = change(&s_cfg, lines, err, errlen)[0] == '2';
    *pending = s_pending;
    return ok;
}

bool cli_keep(char *err, size_t errlen)
{
    if (!s_pending) return true;
    s_edit = s_cfg;
    s_edit.net = s_trial;
    if (!commit(&s_edit)) {
        snprintf(err, errlen, "Could not write settings to flash");
        return false;
    }
    s_pending = false;
    events_add(EV_NET_KEPT, 0, 0, NULL);
    return true;
}

void cli_port(int i, cli_port_t *o)
{
    bridge_info_t b;
    volatile uart_stats_t *u = &g_uart[i];
    bridge_info(i, &b);
    memset(o, 0, sizeof *o);
    o->state = b.state;
    o->clients = b.clients;
    o->alert = b.alert;
    memcpy(o->peers, b.peers, sizeof o->peers);
    o->serial_rx = b.serial_rx;
    o->serial_tx = b.serial_tx;
    o->net_lost = b.net_lost;
    o->refused = b.refused;
    o->denied = b.denied;
    o->ignored = b.ignored;
    memcpy(o->denied_ip, b.denied_ip, sizeof o->denied_ip);
    o->rx_lost = u->rx_lost;
    o->parity = u->parity;
    o->framing = u->framing;
    o->noise = u->noise;
    o->overrun = u->overrun;
}

void cli_sys(cli_sys_t *o)
{
    memset(o, 0, sizeof *o);
    o->version = g_fw_id.version;
    o->host = g_hostname;
    o->mqtt_state = mqtt_ha_state();
    o->mqtt_error = mqtt_ha_error();
    net_addrs(o->ip, o->mask, o->gateway);
    snprintf(o->mac, sizeof o->mac, "%02x:%02x:%02x:%02x:%02x:%02x",
             g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    o->now_ms = HAL_GetTick();
    o->uptime_s = o->now_ms / 1000;
    o->boots = bkp_get(BKP_BOOTS);
    o->crashes = bkp_get(BKP_CRASHES);
    for (int i = 0; i < RSTLOG_N; i++) o->reset_log[i] = bkp_get(BKP_RSTLOG + i);
    o->slot = (int)bkp_get(BKP_SLOT);
    o->speed = g_eth.speed100 ? 100 : 10;
    o->full_duplex = g_eth.full_duplex;
    o->net_pending_s = s_pending ? (int)(s_deadline - o->now_ms) / 1000 : -1;
}
_Static_assert(RSTLOG_N == sizeof((cli_sys_t *)0)->reset_log / sizeof(uint32_t), "reset log size");

bool cli_send(int i, const uint8_t *d, size_t n, char *err, size_t errlen)
{
    if (!s_cfg.port[i].enabled) snprintf(err, errlen, "port %d is off; run enable %d first", i + 1, i + 1);
    else if (s_cfg.port[i].listen_only) snprintf(err, errlen, "port %d is listen only", i + 1);
    else if (bridge_send(i, d, n)) return true;
    else snprintf(err, errlen, "port %d cannot take that right now", i + 1);
    return false;
}

void cli_restart(void)
{
    s_restart = true;
    s_restart_at = HAL_GetTick() + 500;      /* let the reply out first */
}

/* Only called with every port off, so the UARTs are free. Blocks ~0.4 s. */
void cli_wiretest(char *out, size_t max)
{
    port_cfg_t p = { .baud = 9600, .data_bits = 8, .stop_bits = 1 };
    uint8_t pat[16], got[64];
    size_t n = 0;
    bool any = false;
#define OUT(...) do { int w = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w > 0 && (size_t)w < max - n) n += (size_t)w; } while (0)
    out[0] = 0;
    for (int i = 0; i < PORTS; i++) uart485_open(i, &p);
    for (int i = 0; i < PORTS; i++) {
        for (int k = 0; k < PORTS; k++) while (uart485_read(k, got, sizeof got)) { }
        for (int k = 0; k < 16; k++) pat[k] = (uint8_t)((i + 1) * 0x10 + k) ^ (k & 1 ? 0xFF : 0);
        uart485_write(i, pat, sizeof pat);
        uint32_t t0 = HAL_GetTick();
        while (HAL_GetTick() - t0 < 80) wdt_feed();
        OUT("sent on %d:", i + 1);
        bool heard = false;
        for (int k = 0; k < PORTS; k++) {
            size_t g = uart485_read(k, got, sizeof got);
            if (!g) continue;
            heard = any = true;
            OUT(" port %d %s%s;", k + 1, g == sizeof pat && !memcmp(got, pat, g) ? "heard it correctly" : "heard something garbled",
                k == i ? " (own echo)" : "");
        }
        OUT(heard ? "\n" : " nobody heard it\n");
    }
    for (int i = 0; i < PORTS; i++) uart485_close(i);
    if (!any) OUT("No port heard another. Expected if nothing is wired between ports.\n");
#undef OUT
}

/* GET /api/devices and /api/settings, part by part (http.c's do_parts). */
int api_devices(uint32_t part, char *out, size_t max)
{
    static uint32_t seen;                               /* the devices there were at part 0 */
    size_t n = 0;
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); if (w_ < 0 || (size_t)w_ >= max - n) return -2; n += (size_t)w_; } while (0)
    if (part == 0) {
        seen = 0;
        for (int i = 0; i < DEVS; i++) if (device_get(i)) seen |= 1u << i;
        OUT("{\"profiles\":[");
        for (int i = 0; profiles_id(i); i++) OUT("%s\"%s\"", i ? "," : "", profiles_id(i));
        OUT("],\"line_use\":[%u,%u,%u,%u],\"devices\":[", device_line_use(0), device_line_use(1), device_line_use(2), device_line_use(3));
        return (int)n;
    }
    if (part <= DEVS * DEV_PARTS) {
        int i = (int)(part - 1) / DEV_PARTS, k = (int)(part - 1) % DEV_PARTS;
        if (!(seen >> i & 1)) return 0;
        if (k) { int w = device_json_part(i, k, out, max); return w < 0 ? -2 : w; }
        if (!device_get(i)) return -2;                  /* removed since part 0: fail, not malformed JSON */
        if (seen & ((1u << i) - 1)) OUT(",");
        int w = device_json_part(i, 0, out + n, max - n);
        if (w < 0) return -2;
        return (int)n + w;
    }
    if (part == DEVS * DEV_PARTS + 1) { OUT("]}\n"); return (int)n; }
    return -1;
#undef OUT
}

/* JSON's escapes for s, without the quotes. 0 if it does not fit. */
static size_t escaped(const char *s, char *out, size_t max)
{
    size_t w = json_str(s, out, max);
    if (w < 2) return 0;
    memmove(out, out + 1, w - 2);
    out[w - 2] = 0;
    return w - 2;
}

int api_settings(uint32_t part, char *out, size_t max)
{
    static char text[3456];
    static cfg_t c;                                     /* as it was at part 0 */
    size_t w;
    if (part == 0) {
        char iso[17] = "", when[24] = "";
        c = s_cfg;
        if (s_pending) c.net = s_trial;
        if (clock_iso(iso, false))                      /* 20261126T031043Z: 2026-11-26 03:10 UTC */
            snprintf(when, sizeof when, "%.4s-%.2s-%.2s %.2s:%.2s UTC", iso, iso + 4, iso + 6, iso + 9, iso + 11);
        int h = snprintf(text, sizeof text, "# Settings of %s (%s), firmware %s, %s\n"
                         "# Restore them on its Hardware page. Passwords and keys are not here.\n",
                         c.device_name, g_hostname, g_fw_id.version, when[0] ? when : "time unknown");
        if (h <= 0 || (size_t)h >= sizeof text || !cfg_lines(&c, text + h, sizeof text - (size_t)h) || max < 9) return -2;
        memcpy(out, "{\"text\":\"", 9);
        w = escaped(text, out + 9, max - 9);
        return w ? (int)(9 + w) : -2;
    }
    if (part <= DEVS) {
        if (!cfg_dev_lines(&c, (int)part - 1, text, sizeof text)) return -2;
        w = escaped(text, out, max);
        return w ? (int)w : -2;
    }
    if (part == DEVS + 1) {
        if (max < 4) return -2;
        memcpy(out, "\"}\n", 4);
        return 3;
    }
    return -1;
}

/* GET /metrics, a part at a time (http.c): the figures read afresh for each. */
int api_metrics(uint32_t part, char *out, size_t max)
{
    static metrics_t m;
    static const char *const MODE[] = { "server", "client", "udp", "modbus" };
    memset(&m, 0, sizeof m);
    m.version = g_fw_id.version;
    m.uptime_s = HAL_GetTick() / 1000u;
    m.starts = bkp_get(BKP_BOOTS);
    m.failed_starts = bkp_get(BKP_CRASHES);
    hwinfo_loop_us(&m.loop_recent_us, &m.loop_max_us);
    m.temp_ok = hwinfo_temp(&m.temp_dc);
    m.link = g_eth.link_up;
    m.eth_rx = g_eth.rx_frames; m.eth_tx = g_eth.tx_frames; m.eth_rx_err = g_eth.rx_errors; m.eth_tx_drop = g_eth.tx_drops;
    tls_counts(&m.handshakes, &m.handshakes_failed);
    gcm_hw_counts(&m.aes_hw, &m.aes_sw);
    m.mqtt = s_cfg.mqtt.enabled;
    m.mqtt_connected = !strcmp(mqtt_ha_state(), "connected");
    m.clock_synced = clock_synced();
    for (int i = 0; i < PORTS; i++) {
        bridge_info_t b;
        volatile uart_stats_t *u = &g_uart[i];
        bridge_info(i, &b);
        m.port[i].name = s_cfg.port[i].name;
        m.port[i].mode = s_cfg.port[i].mode < 4 ? MODE[s_cfg.port[i].mode] : "";
        m.port[i].up = b.up;
        m.port[i].clients = (uint32_t)b.clients;
        m.port[i].line_rx = b.serial_rx; m.port[i].line_tx = b.serial_tx;
        m.port[i].net_rx = b.net_rx; m.port[i].net_tx = b.net_tx;
        m.port[i].turned_away = b.denied;
        m.port[i].heard_ms = b.heard_ms;
        m.port[i].fwd = b.fwd;
        const uint32_t e[ERR_KINDS] = { u->parity, u->framing, u->noise, u->overrun, u->rx_lost, b.net_lost, b.refused };
        memcpy(m.port[i].err, e, sizeof e);
        const mb_stats_t *mb = bridge_modbus(i, &m.port[i].mb_waiting);
        if ((m.port[i].modbus = mb != NULL)) m.port[i].mb = *mb;
    }
    for (int i = 0; i < DEVS; i++) {
        const device_t *dv = device_get(i);
        if (!dv || dv->off) continue;                   /* off: not a device to watch */
        m.dev[i] = (typeof(m.dev[i])){ dv->cfg.name, true, dv->online, dv->polls, dv->misses };
    }
    for (int i = 0; i < PORTS; i++) m.line_use[i] = device_line_use(i);
    return metrics_part(&m, part, out, max);
}

/* Each reply timed, for the loop figures: which request makes the main loop wait longest. */
int api_get(const char *path, char *out, size_t max)
{
    uint32_t t = DWT->CYCCNT;
    int n = get_one(path, out, max);
    if (n >= 0) hwinfo_request(path, DWT->CYCCNT - t);
    return n;
}

bool api_is_post(const char *path)
{
    return !strcmp(path, "/api/config") || !strcmp(path, "/api/password") ||
           !strcmp(path, "/api/keep") || !strcmp(path, "/api/check") || !strcmp(path, "/api/cli") ||
           !strcmp(path, "/api/tls") || !strcmp(path, "/api/acme");
}

const char *api_post(const char *path, api_req_t *rq, char *out, size_t max)
{
    if (!strcmp(path, "/api/config")) return post_config(rq, out, max);
    if (!strcmp(path, "/api/cli")) {
        cli_run(rq->body, out, max);
        return "200 OK";
    }
    if (!strcmp(path, "/api/password")) return post_password(rq, out, max);
    if (!strcmp(path, "/api/acme")) {
        /* "secret_enc=<128 hex>": 64 zero-padded bytes XORed with HMAC(key, "acmesec0\n" nonce)
         * and HMAC(key, "acmesec1\n" nonce), as for the MQTT password. */
        static const char K[] = "secret_enc=";
        uint8_t v[64], pad[64];
        char *p = strstr(rq->body, K);
        bool have = p != NULL;
        if (have) {
            if ((p != rq->body && p[-1] != '\n') || !unhex(p + sizeof K - 1, v, 64)) { snprintf(out, max, "That secret cannot be used"); return "400 Bad Request"; }
            hmac_sha256(rq->key, "acmesec0\n", 9, rq->nonce, strlen(rq->nonce), pad);
            hmac_sha256(rq->key, "acmesec1\n", 9, rq->nonce, strlen(rq->nonce), pad + 32);
            for (int i = 0; i < 64; i++) v[i] ^= pad[i];
            char *e = strchr(p, '\n');
            if (e) memmove(p, e + 1, strlen(e + 1) + 1);
            else *p = 0;
            size_t l = strlen(rq->body);
            if (l && rq->body[l - 1] == '\n') rq->body[l - 1] = 0;
        }
        const char *why = acme_command(rq->body, have ? v : NULL, sizeof v);
        memset(v, 0, sizeof v);
        snprintf(out, max, "%s", why ? why : "{}");
        return why ? "400 Bad Request" : "200 OK";
    }
    if (!strcmp(path, "/api/tls")) {
        const char *why = tls_command((const uint8_t *)rq->body, rq->len);
        snprintf(out, max, "%s", why ? why : "{}");
        return why ? "400 Bad Request" : "200 OK";
    }
    if (!strcmp(path, "/api/keep") && !cli_keep(out, max)) return "500 Internal Server Error";
    snprintf(out, max, "{}");            /* /api/check: the signature was the point */
    return "200 OK";
}
