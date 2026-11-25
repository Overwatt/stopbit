#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_BOOL, T_NBOOL, T_U8, T_U16, T_U32, T_STR, T_HOST, T_ENUM };

typedef struct {
    const char *key;
    uint8_t  type;
    uint16_t off, size;             /* size: string capacity */
    uint32_t min, max;
    const char *const *names;       /* T_ENUM */
} field_t;

static const char *const MODES[]    = { "server", "client", "udp", NULL };
static const char *const PARITIES[] = { "none", "even", "odd", NULL };

#define F(k, t, lo, hi, n) { #k, t, offsetof(port_cfg_t, k), sizeof(((port_cfg_t *)0)->k), lo, hi, n }
static const field_t PORT_F[] = {
    F(name,        T_STR,  0, 0, NULL),
    F(enabled,     T_BOOL, 0, 1, NULL),
    F(mode,        T_ENUM, 0, 2, MODES),
    F(baud,        T_U32,  300, 2000000, NULL),
    F(data_bits,   T_U8,   7, 8, NULL),
    F(parity,      T_ENUM, 0, 2, PARITIES),
    F(stop_bits,   T_U8,   1, 2, NULL),
    F(gap_ms,      T_U16,  0, 1000, NULL),
    F(packet_len,  T_U16,  1, 1400, NULL),
    F(local_port,  T_U16,  0, 65535, NULL),
    F(remote_host, T_HOST, 0, 0, NULL),
    F(remote_port, T_U16,  0, 65535, NULL),
    F(echo_cancel, T_BOOL, 0, 1, NULL),
    F(loopback,    T_BOOL, 0, 1, NULL),
    F(listen_only, T_BOOL, 0, 1, NULL),
};
#define NPORT_F (sizeof PORT_F / sizeof PORT_F[0])

/* The password is not here: it arrives encrypted, see api.c. */
#define M(k, t, lo, hi) { #k, t, offsetof(mqtt_cfg_t, k), sizeof(((mqtt_cfg_t *)0)->k), lo, hi, NULL }
static const field_t MQTT_F[] = {
    M(enabled, T_BOOL, 0, 1),
    M(host,    T_HOST, 0, 0),
    M(port,    T_U32,  1, 65535),
    M(user,    T_STR,  0, 0),
    M(control, T_BOOL, 0, 1),
    { "announce", T_NBOOL, offsetof(mqtt_cfg_t, no_announce), 1, 0, 1, NULL },   /* stored inverted: 0 = announce */
};
#define NMQTT_F (sizeof MQTT_F / sizeof MQTT_F[0])

static const uint16_t QUIET_MIN[16] = { 0, 1, 2, 3, 5, 10, 15, 20, 30, 60, 120, 180, 240, 360, 720, 1440 };

uint16_t cfg_quiet_min(const cfg_t *c, int i) { return QUIET_MIN[c->quiet >> (4 * i) & 15]; }

void cfg_defaults(cfg_t *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->device_name, sizeof c->device_name, "N540 gateway");
    c->net.dhcp = 1;
    c->mqtt.port = 1883;
    for (int i = 0; i < PORTS; i++) {
        port_cfg_t *p = &c->port[i];
        snprintf(p->name, sizeof p->name, "Port %d", i + 1);
        p->mode = MODE_SERVER;
        p->baud = 9600;
        p->data_bits = 8;
        p->stop_bits = 1;
        p->packet_len = 1024;
        p->local_port = (uint16_t)(4001 + i);
    }
}

void cfg_reset_access(cfg_t *c)
{
    memset(&c->net, 0, sizeof c->net);
    c->net.dhcp = 1;
    c->web_set = 0;
    memset(c->web_salt, 0, sizeof c->web_salt);
    memset(c->web_key, 0, sizeof c->web_key);
}

/* ---- text helpers -------------------------------------------------------- */
static bool parse_u32(const char *s, uint32_t *out)
{
    uint64_t v = 0;
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 0xFFFFFFFFu) return false;
    }
    *out = (uint32_t)v;
    return true;
}

bool ip4_parse(const char *s, uint32_t *out)
{
    uint8_t b[4];
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0;
        int n = 0;
        while (*s >= '0' && *s <= '9' && n < 4) { v = v * 10 + (uint32_t)(*s++ - '0'); n++; }
        if (n == 0 || n > 3 || v > 255) return false;
        b[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    memcpy(out, b, 4);
    return true;
}

void ip4_fmt(uint32_t ip, char out[16])
{
    uint8_t b[4];
    memcpy(b, &ip, 4);
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static uint32_t be(uint32_t n)
{
    uint8_t b[4];
    memcpy(b, &n, 4);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static bool set_str(char *dst, size_t cap, const char *v, bool host)
{
    size_t n = strlen(v);
    if (n >= cap) return false;
    for (const char *p = v; *p; p++) {
        bool alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (host ? !(alnum || *p == '.' || *p == '-') : (*p < 0x20 || *p > 0x7e)) return false;
    }
    memset(dst, 0, cap);             /* keep structs memcmp-able */
    memcpy(dst, v, n);
    return true;
}

static bool set_field(void *base, const field_t *f, const char *v)
{
    uint8_t *p = (uint8_t *)base + f->off;
    uint32_t n;
    switch (f->type) {
    case T_STR:  return set_str((char *)p, f->size, v, false);
    case T_HOST: return set_str((char *)p, f->size, v, true);
    case T_ENUM:
        for (n = 0; f->names[n]; n++)
            if (!strcmp(f->names[n], v)) { *p = (uint8_t)n; return true; }
        return false;
    }
    if (!parse_u32(v, &n) || n < f->min || n > f->max) return false;
    switch (f->type) {
    case T_U32: memcpy(p, &n, 4); break;
    case T_U16: { uint16_t h = (uint16_t)n; memcpy(p, &h, 2); break; }
    case T_NBOOL: *p = !n; break;
    default:    *p = (uint8_t)n;
    }
    return true;
}

static bool apply(cfg_t *c, const char *k, const char *v)
{
    if (!strncmp(k, "port", 4) && k[4] >= '1' && k[4] < '1' + PORTS && k[5] == '.') {
        for (size_t i = 0; i < NPORT_F; i++)
            if (!strcmp(PORT_F[i].key, k + 6))
                return set_field(&c->port[k[4] - '1'], &PORT_F[i], v);
        uint32_t min;
        if (!strcmp(k + 6, "access")) return acl_parse(&c->access[k[4] - '1'], v);
        if (strcmp(k + 6, "quiet_min") || !parse_u32(v, &min)) return false;
        for (unsigned q = 0; q < 16; q++)
            if (QUIET_MIN[q] == min) {
                int sh = 4 * (k[4] - '1');
                c->quiet = (uint16_t)((c->quiet & ~(15u << sh)) | q << sh);
                return true;
            }
        return false;
    }
    if (!strncmp(k, "mqtt.", 5)) {
        for (size_t i = 0; i < NMQTT_F; i++)
            if (!strcmp(MQTT_F[i].key, k + 5)) return set_field(&c->mqtt, &MQTT_F[i], v);
        return false;
    }
    if (!strcmp(k, "device.name")) return set_str(c->device_name, sizeof c->device_name, v, false);
    if (!strcmp(k, "net.mode")) {
        bool d = !strcmp(v, "dhcp");
        if (!d && strcmp(v, "static")) return false;
        c->net.dhcp = d;
        return true;
    }
    uint32_t *ip = !strcmp(k, "net.ip") ? &c->net.ip : !strcmp(k, "net.mask") ? &c->net.mask :
                   !strcmp(k, "net.gateway") ? &c->net.gw : !strcmp(k, "net.dns") ? &c->net.dns : NULL;
    if (!ip) return false;
    if (!*v) { *ip = 0; return true; }
    return ip4_parse(v, ip);
}

bool cfg_parse(cfg_t *c, char *body, char *err, size_t errlen)
{
    for (char *line = body, *next; line && *line; line = next) {
        if ((next = strchr(line, '\n'))) *next++ = 0;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        if (!*line) continue;
        char *eq = strchr(line, '=');
        if (!eq) { snprintf(err, errlen, "Expected key=value: %.40s", line); return false; }
        *eq = 0;
        if (!apply(c, line, eq + 1)) {
            if (!strncmp(line, "port", 4) && !strcmp(line + 5, ".access"))
                snprintf(err, errlen, "Access: up to %d different addresses, like 10.0.0.5 read, 10.0.0.0/24 write", ACL_MAX);
            else
                snprintf(err, errlen, "Not accepted: %.24s = %.32s", line, eq + 1);
            return false;
        }
    }
    return true;
}

bool cfg_validate(const cfg_t *c, char *err, size_t errlen)
{
    if (!c->net.dhcp) {
        uint32_t ip = be(c->net.ip), m = be(c->net.mask), g = be(c->net.gw);
        uint32_t host = ~m;
        if (!ip || (ip >> 24) == 127 || (ip >> 28) >= 14) {
            snprintf(err, errlen, "Enter a usable IP address");
            return false;
        }
        if (!m || (host & (host + 1)) || host < 3) {      /* contiguous, /30 or wider */
            snprintf(err, errlen, "Enter a valid subnet mask, such as 255.255.255.0");
            return false;
        }
        if ((ip & host) == 0 || (ip & host) == host) {
            snprintf(err, errlen, "That IP is the network or broadcast address of its subnet");
            return false;
        }
        if (g && ((g & m) != (ip & m) || g == ip)) {
            snprintf(err, errlen, "The gateway must be another address in the same subnet");
            return false;
        }
    }
    if (c->mqtt.enabled && !c->mqtt.host[0]) {
        snprintf(err, errlen, "Enter the MQTT broker's host name or address");
        return false;
    }
    if (c->mqtt.password[0] && !c->mqtt.user[0]) {
        snprintf(err, errlen, "MQTT needs a user name with that password");
        return false;
    }
    for (int i = 0; i < PORTS; i++) {
        const port_cfg_t *p = &c->port[i];
        if (p->data_bits == 7 && p->parity == PARITY_NONE && p->stop_bits == 1) {
            snprintf(err, errlen, "Port %d: 7N1 is not supported; add parity or a second stop bit", i + 1);
            return false;
        }
        if (p->listen_only && p->loopback) {
            snprintf(err, errlen, "Port %d: the loopback test transmits, so turn off listen only or loopback", i + 1);
            return false;
        }
        if (!p->enabled) continue;
        if (p->mode == MODE_CLIENT) {
            if (!p->remote_host[0] || !p->remote_port) {
                snprintf(err, errlen, "Port %d: TCP client needs a remote host and port", i + 1);
                return false;
            }
            continue;                                       /* local port unused */
        }
        if (!p->local_port) {
            snprintf(err, errlen, "Port %d: enter a local port", i + 1);
            return false;
        }
        if (p->mode == MODE_SERVER && p->local_port == 80) {
            snprintf(err, errlen, "Port %d: TCP 80 is used by this web page", i + 1);
            return false;
        }
        if (p->mode == MODE_UDP) {
            if (p->local_port == 68 || p->local_port == 5353) {
                snprintf(err, errlen, "Port %d: UDP %u is used by DHCP or mDNS", i + 1, p->local_port);
                return false;
            }
            if (p->remote_host[0] && !p->remote_port) {
                snprintf(err, errlen, "Port %d: enter the remote port", i + 1);
                return false;
            }
        }
        for (int j = 0; j < i; j++) {
            const port_cfg_t *q = &c->port[j];
            if (q->enabled && q->mode == p->mode && q->local_port == p->local_port) {
                snprintf(err, errlen, "Ports %d and %d both use local port %u", j + 1, i + 1, p->local_port);
                return false;
            }
        }
    }
    return true;
}

/* ---- JSON ---------------------------------------------------------------- */
size_t json_str(const char *s, char *out, size_t max)
{
    size_t n = 0;
#define PUT(ch) do { if (n + 1 >= max) return 0; out[n++] = (ch); } while (0)
    PUT('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') PUT('\\');
        PUT((*s >= 0x20 && *s < 0x7f) ? *s : '?');
    }
    PUT('"');
#undef PUT
    out[n] = 0;
    return n;
}

#define ADD(...) do { int w = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= max - n) { return 0; } n += (size_t)w; } while (0)
#define STR(s) do { size_t w = json_str(s, out + n, max - n); if (!w) { return 0; } n += w; } while (0)

static size_t fields_json(const void *base, const field_t *fs, size_t nf, char *out, size_t max)
{
    size_t n = 0;
    ADD("{");
    for (size_t k = 0; k < nf; k++) {
        const field_t *f = &fs[k];
        const uint8_t *p = (const uint8_t *)base + f->off;
        uint32_t u32;
        uint16_t u16;
        ADD("%s\"%s\":", k ? "," : "", f->key);
        switch (f->type) {
        case T_STR: case T_HOST: STR((const char *)p); break;
        case T_ENUM: ADD("\"%s\"", f->names[*p]); break;
        case T_BOOL: ADD("%s", *p ? "true" : "false"); break;
        case T_NBOOL: ADD("%s", *p ? "false" : "true"); break;
        case T_U32: memcpy(&u32, p, 4); ADD("%lu", (unsigned long)u32); break;
        case T_U16: memcpy(&u16, p, 2); ADD("%u", u16); break;
        default: ADD("%u", *p);
        }
    }
    ADD("}");
    return n;
}

size_t cfg_port_json(const port_cfg_t *p, char *out, size_t max)
{
    return fields_json(p, PORT_F, NPORT_F, out, max);
}

size_t cfg_json(const cfg_t *c, char *out, size_t max)
{
    size_t n = 0, w;
    char ip[4][16];
    if (!max) return 0;
    ip4_fmt(c->net.ip, ip[0]); ip4_fmt(c->net.mask, ip[1]);
    ip4_fmt(c->net.gw, ip[2]); ip4_fmt(c->net.dns, ip[3]);
    ADD("{\"device\":{\"name\":");
    STR(c->device_name);
    ADD("},\"net\":{\"mode\":\"%s\"", c->net.dhcp ? "dhcp" : "static");
    static const char *const NK[] = { "ip", "mask", "gateway", "dns" };
    const uint32_t nv[] = { c->net.ip, c->net.mask, c->net.gw, c->net.dns };
    for (int i = 0; i < 4; i++) ADD(",\"%s\":\"%s\"", NK[i], nv[i] ? ip[i] : "");
    ADD("},\"mqtt\":");
    if (!(w = fields_json(&c->mqtt, MQTT_F, NMQTT_F, out + n, max - n))) return 0;
    n += w - 1;                        /* reopen to add one more key */
    ADD(",\"password_set\":%s},\"password_set\":%s,\"ports\":[",
        c->mqtt.password[0] ? "true" : "false", c->web_set ? "true" : "false");
    for (int i = 0; i < PORTS; i++) {
        if (i) ADD(",");
        if (!(w = cfg_port_json(&c->port[i], out + n, max - n))) return 0;
        n += w - 1;
        char t[ACL_TEXT];
        acl_text(&c->access[i], t);
        ADD(",\"quiet_min\":%u,\"access\":\"%s\"}", cfg_quiet_min(c, i), t);
    }
    ADD("]}\n");
    return n;
}

static size_t fields_lines(const char *pre, const void *base, const field_t *fs, size_t nf, char *out, size_t max)
{
    size_t n = 0;
    for (size_t k = 0; k < nf; k++) {
        const field_t *f = &fs[k];
        const uint8_t *p = (const uint8_t *)base + f->off;
        uint32_t v = *p;
        uint16_t h;
        ADD("%s.%s=", pre, f->key);
        if (f->type == T_U32) memcpy(&v, p, 4);
        if (f->type == T_U16) { memcpy(&h, p, 2); v = h; }
        if (f->type == T_NBOOL) v = !v;
        if (f->type == T_STR || f->type == T_HOST) ADD("%s\n", (const char *)p);
        else if (f->type == T_ENUM) ADD("%s\n", f->names[*p]);
        else ADD("%lu\n", (unsigned long)v);
    }
    return n;
}

size_t cfg_lines(const cfg_t *c, char *out, size_t max)
{
    size_t n = 0, w;
    char ip[4][16], pre[8];
    if (!max) return 0;
    ip4_fmt(c->net.ip, ip[0]); ip4_fmt(c->net.mask, ip[1]);
    ip4_fmt(c->net.gw, ip[2]); ip4_fmt(c->net.dns, ip[3]);
    ADD("device.name=%s\nnet.mode=%s\nnet.ip=%s\nnet.mask=%s\nnet.gateway=%s\nnet.dns=%s\n", c->device_name,
        c->net.dhcp ? "dhcp" : "static", c->net.ip ? ip[0] : "", c->net.mask ? ip[1] : "",
        c->net.gw ? ip[2] : "", c->net.dns ? ip[3] : "");
    if (!(w = fields_lines("mqtt", &c->mqtt, MQTT_F, NMQTT_F, out + n, max - n))) return 0;
    n += w;
    for (int i = 0; i < PORTS; i++) {
        snprintf(pre, sizeof pre, "port%d", i + 1);
        if (!(w = fields_lines(pre, &c->port[i], PORT_F, NPORT_F, out + n, max - n))) return 0;
        n += w;
        char t[ACL_TEXT];
        acl_text(&c->access[i], t);
        ADD("%s.quiet_min=%u\n%s.access=%s\n", pre, cfg_quiet_min(c, i), pre, t);
    }
    return n;
}
