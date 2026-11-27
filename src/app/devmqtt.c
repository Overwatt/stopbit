/*
 * devmqtt.c — Each device's values over MQTT: its state as one JSON object, its status,
 * and an HA discovery config per field; a removed device's emptied. One publish a call,
 * so the main loop keeps moving (mqtt_ha.c calls this each pass once its own are done).
 */
#include "devmqtt.h"
#include "device.h"
#include <stdio.h>
#include <string.h>

#define BUF 7680   /* a device's state or one config: within lwIP MQTT's 8192-byte output ring, with topic and header */
static char s_buf[BUF];
static struct { uint8_t gen; int16_t disc; bool status, online; uint32_t seq, at; } s_d[DEVS];
static struct { bool used; char name[25]; const profile_t *p; int16_t at; } s_gone[DEVS];

static const char *comp(const pf_field_t *f)
{
    switch (f->kind) {
    case PF_BINARY: case PF_CALCBIN: return "binary_sensor";
    case PF_NUMBER: return "number";
    case PF_SELECT: return "select";
    case PF_SWITCH: return "switch";
    case PF_TIME:   return "text";
    default:        return "sensor";
    }
}

/* "a_b" as an HA object id: lower case, anything but a-z 0-9 as _. */
static void object(char *out, size_t max, const char *a, const char *b)
{
    size_t n = 0;
    for (const char *s = a; *s && n + 1 < max; s++) out[n++] = (char)((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ? *s : *s >= 'A' && *s <= 'Z' ? *s + 32 : '_');
    if (n + 1 < max) out[n++] = '_';
    for (const char *s = b; *s && n + 1 < max; s++) out[n++] = (char)((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ? *s : *s >= 'A' && *s <= 'Z' ? *s + 32 : '_');
    out[n] = 0;
}

static void gone(int i, const device_t *d)
{
    (void)i;
    if (!d->p) return;
    for (int k = 0; k < DEVS; k++)
        if (!s_gone[k].used) {
            s_gone[k].used = true;
            snprintf(s_gone[k].name, sizeof s_gone[k].name, "%s", d->cfg.name);
            s_gone[k].p = d->p;
            s_gone[k].at = 0;
            device_hold(d->p, 1);
            return;
        }
}

void devmqtt_init(void) { memset(s_d, 0, sizeof s_d); memset(s_gone, 0, sizeof s_gone); device_on_gone(gone); }

void devmqtt_restart(void)
{
    for (int i = 0; i < DEVS; i++) { s_d[i].disc = 0; s_d[i].status = false; s_d[i].seq = 0; s_d[i].at = 0; }
}

#define ADD(...) do { int w_ = snprintf(b + n, max - n, __VA_ARGS__); if (w_ < 0 || (size_t)w_ >= max - n) return 0; n += (size_t)w_; } while (0)

static size_t config(char *b, size_t max, const device_t *d, int fi, const char *base, const char *host)
{
    const profile_t *p = d->p;
    const pf_field_t *f = &p->field[fi];
    const char *id = pf_str(p, f->id);
    char nm[96], dn[80], mk[64], md[96], un[40], ent[80], own[40], pre[sizeof d->cfg.prefix + 1] = "";
    size_t n = 0;
    bool bin = f->kind == PF_BINARY || f->kind == PF_CALCBIN || f->kind == PF_SWITCH;
    if (!json_str(pf_str(p, f->name), nm, sizeof nm) || !json_str(d->cfg.label[0] ? d->cfg.label : d->cfg.name, dn, sizeof dn) ||
        !json_str(pf_str(p, p->maker), mk, sizeof mk) || !json_str(pf_str(p, p->name), md, sizeof md) ||
        !json_str(pf_str(p, f->unit), un, sizeof un)) return 0;
    if (d->cfg.ids) snprintf(pre, sizeof pre, "%s_", d->cfg.prefix);   /* ESPHome ids: a new unique_id, so HA gives the ESP32's entity id */
    if (d->cfg.ids) object(ent, sizeof ent, d->cfg.prefix, pf_str(p, f->name));
    else { snprintf(own, sizeof own, "n540_%s", d->cfg.name); object(ent, sizeof ent, own, id); }
    ADD("{\"~\":\"%s/dev/%s\",\"name\":%s,\"unique_id\":\"%s_%s_%s%s\",\"default_entity_id\":\"%s.%s\","
        "\"availability\":[{\"topic\":\"%s/status\"},{\"topic\":\"~/status\"},"
        "{\"topic\":\"~/state\",\"value_template\":\"{{ 'online' if value_json['%s'] is not none else 'offline' }}\"}],"
        "\"availability_mode\":\"all\",\"state_topic\":\"~/state\"",
        base, d->cfg.name, nm, host, d->cfg.name, pre, id, comp(f), ent, base, id);
    if (bin) ADD(",\"value_template\":\"{{ 'ON' if value_json['%s'] else 'OFF' }}\"", id);
    else ADD(",\"value_template\":\"{{ value_json['%s'] }}\"", id);
    if (f->unit) ADD(",\"unit_of_measurement\":%s", un);
    if (f->dc) ADD(",\"device_class\":\"%s\"", pf_str(p, f->dc));
    if (f->sc && !pf_control(f)) ADD(",\"state_class\":\"%s\"", pf_str(p, f->sc));
    if (f->kind == PF_SENSOR || f->kind == PF_CALC) ADD(",\"suggested_display_precision\":%d", pf_prec(f));
    if ((f->flags & PFF_DIAG) && !pf_control(f)) ADD(",\"entity_category\":\"diagnostic\"");
    if (pf_control(f)) {
        ADD(",\"command_topic\":\"~/set/%s\",\"entity_category\":\"config\"", id);
        if (f->flags & PFF_HIDDEN) ADD(",\"enabled_by_default\":false");
        if (f->kind == PF_NUMBER) {
            char lo[24], hi[24], st[24];
            int pr = pf_prec(f);
            if (!pf_num(lo, sizeof lo, f->lo, pr) || !pf_num(hi, sizeof hi, f->hi, pr) || !pf_num(st, sizeof st, f->k, pr)) return 0;
            ADD(",\"min\":%s,\"max\":%s,\"step\":%s,\"mode\":\"box\"", lo, hi, st);
        } else if (f->kind == PF_SELECT) {
            ADD(",\"options\":[");
            for (int k = 0; k < f->nvname; k++) {
                char o[96];
                if (!json_str(pf_str(p, p->vname[f->vname + k].name), o, sizeof o)) return 0;
                ADD("%s%s", k ? "," : "", o);
            }
            ADD("]");
        } else if (f->kind == PF_TIME) {
            ADD(",\"pattern\":\"^([01][0-9]|2[0-3]):[0-5][0-9]$\",\"min\":5,\"max\":5");
        }
    }
    ADD(",\"device\":{\"identifiers\":[\"%s_%s\"],\"name\":%s,\"manufacturer\":%s,\"model\":%s,\"via_device\":\"%s\"}}",
        host, d->cfg.name, dn, mk, md, host);
    return n;
}

static size_t state(char *b, size_t max, const device_t *d)
{
    size_t n = 0;
    ADD("{");
    for (int f = 0; f < d->p->nfield; f++) {
        char v[520];
        if (!device_value(d, f, v, sizeof v)) return 0;
        ADD("%s\"%s\":%s", f ? "," : "", pf_str(d->p, d->p->field[f].id), v);
    }
    ADD("}");
    return n;
}
#undef ADD

bool devmqtt_poll(devmqtt_pub_t pub, const char *base, const char *host, bool announce, uint32_t now)
{
    char topic[160];
    for (int k = 0; k < DEVS; k++) {                     /* removed devices: emptied first */
        if (!s_gone[k].used) continue;
        const profile_t *p = s_gone[k].p;
        int at = s_gone[k].at;
        if (at < p->nfield) {
            const pf_field_t *f = &p->field[at];
            snprintf(topic, sizeof topic, "homeassistant/%s/%s_%s/%s/config", comp(f), host, s_gone[k].name, pf_str(p, f->id));
        } else {
            snprintf(topic, sizeof topic, "%s/dev/%s/%s", base, s_gone[k].name, at == p->nfield ? "state" : "status");
        }
        if (!pub(topic, "", true)) return false;
        if (++s_gone[k].at > p->nfield + 1) { device_hold(p, -1); s_gone[k].used = false; }
        return true;
    }
    for (int i = 0; i < DEVS; i++) {
        const device_t *d = device_get(i);
        if (!d || !d->p) continue;
        if (s_d[i].gen != d->gen) { memset(&s_d[i], 0, sizeof s_d[i]); s_d[i].gen = d->gen; }
        if (s_d[i].disc >= 0 && s_d[i].disc < d->p->nfield) {
            const pf_field_t *f = &d->p->field[s_d[i].disc];
            snprintf(topic, sizeof topic, "homeassistant/%s/%s_%s/%s/config", comp(f), host, d->cfg.name, pf_str(d->p, f->id));
            bool show = announce && (!pf_control(f) || d->cfg.controls);      /* controls off: emptied */
            size_t n = show ? config(s_buf, sizeof s_buf, d, s_d[i].disc, base, host) : 0;
            if (show && !n) { s_d[i].disc++; return true; }                  /* will not fit: left out */
            if (!pub(topic, show ? s_buf : "", true)) return false;
            if (++s_d[i].disc == d->p->nfield) s_d[i].disc = -1;
            return true;
        }
        if (!s_d[i].status || s_d[i].online != d->online) {
            snprintf(topic, sizeof topic, "%s/dev/%s/status", base, d->cfg.name);
            if (!pub(topic, d->online ? "online" : "offline", true)) return false;
            s_d[i].status = true;
            s_d[i].online = d->online;
            return true;
        }
        if ((d->seq != s_d[i].seq && now - s_d[i].at >= 1000u) || now - s_d[i].at >= 60000u) {
            snprintf(topic, sizeof topic, "%s/dev/%s/state", base, d->cfg.name);
            if (!state(s_buf, sizeof s_buf, d)) { s_d[i].seq = d->seq; s_d[i].at = now; return true; }   /* too big: skipped */
            if (!pub(topic, s_buf, true)) return false;
            s_d[i].seq = d->seq;
            s_d[i].at = now;
            return true;
        }
    }
    return true;
}
