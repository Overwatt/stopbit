#include "cfg_store.h"
#include "extflash.h"
#include "sha256.h"
#include <string.h>

#define DEV    1
#define MAGIC  0x47464335u
#define SCHEMA 2u                       /* bump when cfg_t's layout changes */
#define SIZE1  472u                     /* schema 1: cfg_t without mqtt */
_Static_assert(offsetof(cfg_t, mqtt) == SIZE1, "schema 1 layout must not move");
_Static_assert(sizeof(port_cfg_t) == 92 && offsetof(port_cfg_t, listen_only) == 91, "listen_only fills the padding");
static const uint32_t ADDR[2] = { 0x0F0000u, 0x0F1000u };

typedef struct {
    uint32_t magic, schema, seq, size;
    uint8_t  sha[32];                   /* over schema, seq, size and cfg */
    cfg_t    cfg;
} rec_t;
_Static_assert(sizeof(rec_t) <= 4096, "config record exceeds a sector");

static rec_t s_r;                       /* too big for the stack */
static uint32_t s_seq;
static int s_cur = -1;

static void digest(const rec_t *r, uint8_t out[32])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, &r->schema, 12);
    sha256_update(&s, &r->cfg, r->size);
    sha256_final(&s, out);
}

static bool read_valid(int i)
{
    uint8_t d[32];
    extflash_read(DEV, ADDR[i], &s_r, sizeof s_r);
    if (s_r.magic != MAGIC) return false;
    if (!(s_r.schema == SCHEMA && s_r.size == sizeof(cfg_t)) &&
        !(s_r.schema == 1u && s_r.size == SIZE1)) return false;
    digest(&s_r, d);
    return ct_equal(d, s_r.sha, 32);
}

bool cfg_load(cfg_t *c)
{
    char err[96];
    for (int i = 0; i < 2; i++) {
        if (!read_valid(i) || (s_cur >= 0 && s_r.seq <= s_seq)) continue;
        s_cur = i;
        s_seq = s_r.seq;
        cfg_defaults(c);                 /* newer fields, if this is an old record */
        memcpy(c, &s_r.cfg, s_r.size);
    }
    if (s_cur >= 0 && cfg_validate(c, err, sizeof err)) return true;
    cfg_defaults(c);
    return false;
}

bool cfg_save(const cfg_t *c)
{
    int i = s_cur == 0 ? 1 : 0;         /* never the copy we would fall back to */
    static rec_t w;
    memset(&w, 0, sizeof w);
    w.magic = MAGIC;
    w.schema = SCHEMA;
    w.seq = s_seq + 1;
    w.size = sizeof(cfg_t);
    w.cfg = *c;
    digest(&w, w.sha);
    if (!extflash_erase(DEV, ADDR[i], 4096)) return false;
    if (!extflash_write(DEV, ADDR[i], &w, sizeof w)) return false;
    if (!read_valid(i) || memcmp(&s_r, &w, sizeof w)) return false;
    s_cur = i;
    s_seq = w.seq;
    return true;
}
