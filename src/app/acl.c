#include "acl.h"
#include "cfg.h"
#include <stdio.h>
#include <string.h>

/* Network-order mask, on any host. */
static uint32_t mask(int bits)
{
    uint8_t b[4];
    uint32_t m = bits ? 0xFFFFFFFFu << (32 - bits) : 0, n;
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(m >> (24 - 8 * i));
    memcpy(&n, b, 4);
    return n;
}

acl_role_t acl_role(const acl_t *a, uint32_t ip)
{
    int best = -1;
    acl_role_t r = ACL_DENY;
    if (!a->n) return ACL_WRITE;
    for (uint32_t i = 0; i < a->n && i < ACL_MAX; i++) {
        const acl_ent_t *e = &a->e[i];
        if ((ip & mask(e->bits)) != e->ip || (int)e->bits <= best) continue;
        best = e->bits;
        r = e->write ? ACL_WRITE : ACL_READ;
    }
    return r;
}

static bool word(const char **s, char *out, size_t max)
{
    size_t n = 0;
    while (**s == ' ') (*s)++;
    while (**s && **s != ' ' && **s != ',') {
        if (n + 1 >= max) return false;
        out[n++] = *(*s)++;
    }
    while (**s == ' ') (*s)++;
    out[n] = 0;
    return n > 0;
}

bool acl_parse(acl_t *out, const char *s)
{
    acl_t a;
    memset(&a, 0, sizeof a);
    while (*s == ' ') s++;
    while (*s) {
        char addr[24], role[8], *slash;
        acl_ent_t e;
        memset(&e, 0, sizeof e);
        if (a.n == ACL_MAX || !word(&s, addr, sizeof addr) || !word(&s, role, sizeof role)) return false;
        if (!strcmp(role, "write")) e.write = 1;
        else if (strcmp(role, "read")) return false;
        e.bits = 32;
        if ((slash = strchr(addr, '/'))) {
            *slash++ = 0;
            if (*slash < '0' || *slash > '9' || (slash[1] && (slash[1] < '0' || slash[1] > '9' || slash[2] || *slash == '0'))) return false;
            int b = *slash - '0';
            if (slash[1]) b = b * 10 + slash[1] - '0';
            if (b > 32) return false;
            e.bits = (uint8_t)b;
        }
        if (!ip4_parse(addr, &e.ip)) return false;
        e.ip &= mask(e.bits);
        for (uint32_t i = 0; i < a.n; i++)              /* twice, perhaps with both roles */
            if (a.e[i].ip == e.ip && a.e[i].bits == e.bits) return false;
        a.e[a.n++] = e;
        if (*s == ',') {
            s++;
            while (*s == ' ') s++;
            if (!*s) return false;
        } else if (*s) {
            return false;
        }
    }
    *out = a;
    return true;
}

void acl_text(const acl_t *a, char out[ACL_TEXT])
{
    size_t n = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < a->n && i < ACL_MAX; i++) {
        char ip[16], net[4] = "";
        ip4_fmt(a->e[i].ip, ip);
        if (a->e[i].bits < 32) snprintf(net, sizeof net, "/%u", a->e[i].bits);
        n += (size_t)snprintf(out + n, ACL_TEXT - n, "%s%s%s %s", i ? ", " : "", ip, net, a->e[i].write ? "write" : "read");
    }
}

bool acl_valid(const acl_t *a)
{
    if (a->n > ACL_MAX) return false;
    for (uint32_t i = 0; i < a->n; i++)
        if (a->e[i].bits > 32 || a->e[i].write > 1 || (a->e[i].ip & ~mask(a->e[i].bits))) return false;
    return true;
}

void acl_deny_all(acl_t *a)
{
    memset(a, 0, sizeof *a);
    a->n = 1;
    a->e[0].ip = 0xFFFFFFFFu;           /* not an address anyone can have */
    a->e[0].bits = 32;
}
