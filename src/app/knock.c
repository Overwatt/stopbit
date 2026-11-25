#include "knock.h"
#include "quiet.h"
#include <stddef.h>
#include <string.h>

void knock_add(knocks_t *s, uint32_t ip, uint32_t now)
{
    knock_t *k = &s->k[0];
    for (int i = 0; i < KNOCK_N; i++) {
        knock_t *c = &s->k[i];
        if (c->count && c->ip == ip) { k = c; break; }
        if (!k->count) continue;                        /* a free place is as good as it gets */
        if (!c->count || c->seq < k->seq) k = c;
    }
    if (!k->count || k->ip != ip) {
        memset(k, 0, sizeof *k);
        k->ip = ip;
    } else if (now - k->at >= KNOCK_RECENT) {
        k->tries = 0;
    }
    if (k->count != UINT32_MAX) k->count++;
    if (k->tries != UINT32_MAX) k->tries++;
    k->at = now;
    k->seq = ++s->seq;
}

const knock_t *knock_get(const knocks_t *s, int n)
{
    for (int i = 0; i < KNOCK_N; i++) {
        int newer = 0;
        if (!s->k[i].count) continue;
        for (int j = 0; j < KNOCK_N; j++) newer += s->k[j].count && s->k[j].seq > s->k[i].seq;
        if (newer == n) return &s->k[i];
    }
    return NULL;
}

bool knock_persistent(const knocks_t *s, uint32_t now)
{
    for (int i = 0; i < KNOCK_N; i++)
        if (s->k[i].count && s->k[i].tries >= KNOCK_TRIES && now - s->k[i].at < KNOCK_RECENT) return true;
    return false;
}

void knock_forget(knocks_t *s, int i) { memset(&s->k[i], 0, sizeof s->k[i]); }

void knock_clamp(knocks_t *s, uint32_t now)
{
    for (int i = 0; i < KNOCK_N; i++) s->k[i].at = quiet_clamp(now, s->k[i].at);
}
