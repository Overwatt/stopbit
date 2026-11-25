#include "cfg_store.h"
#include "extflash.h"
#include "sha256.h"
#include <string.h>

#define DEV    1
#define MAGIC  0x47464335u
#define SCHEMA 2u                       /* bump when the layout before `access` changes */
#define SIZE1  472u                     /* schema 1: cfg_t without mqtt */
#define SIZE2  656u                     /* schema 2: cfg_t without access lists */
_Static_assert(offsetof(cfg_t, mqtt) == SIZE1, "schema 1 layout must not move");
_Static_assert(offsetof(cfg_t, access) == SIZE2, "schema 2 layout must not move");
_Static_assert(sizeof(port_cfg_t) == 92 && offsetof(port_cfg_t, listen_only) == 91, "listen_only fills the padding");
_Static_assert(sizeof(mqtt_cfg_t) == 184 && offsetof(mqtt_cfg_t, no_announce) == 182, "no_announce fills the padding");
_Static_assert(offsetof(cfg_t, quiet) == 470, "quiet fills the padding after web_key");

/* Two kinds of record, each in two alternating sectors. Access lists have their own
 * so that older firmware, after a rollback, still reads everything else: it accepts
 * only a record of exactly SIZE2. */
#define AMAGIC  0x4C434135u
#define ASCHEMA 1u
#define ASIZE   (PORTS * sizeof(acl_t))
_Static_assert(sizeof(acl_t) == 68, "access list layout must not move");
static const uint32_t ADDR[2]  = { 0x0F0000u, 0x0F1000u };
static const uint32_t AADDR[2] = { 0x0F2000u, 0x0F3000u };

typedef struct {
    uint32_t magic, schema, seq, size;
    uint8_t  sha[32];                   /* over schema, seq, size and body */
    uint8_t  body[SIZE2];
} rec_t;
_Static_assert(ASIZE <= SIZE2, "record too small");
_Static_assert(sizeof(rec_t) <= 4096, "config record exceeds a sector");

static rec_t s_r, s_w;                  /* too big for the stack */
static uint32_t s_seq, s_aseq;
static int s_cur = -1, s_acur = -1;
static acl_t s_saved[PORTS];            /* as last read or written */

#define HEAD offsetof(rec_t, body)

static void digest(const rec_t *r, uint8_t out[32])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, &r->schema, 12);
    sha256_update(&s, r->body, r->size);
    sha256_final(&s, out);
}

static bool read_valid(int i)
{
    uint8_t d[32];
    extflash_read(DEV, ADDR[i], &s_r, sizeof s_r);
    if (s_r.magic != MAGIC) return false;
    if (!(s_r.schema == SCHEMA && s_r.size == SIZE2) &&
        !(s_r.schema == 1u && s_r.size == SIZE1)) return false;
    digest(&s_r, d);
    return ct_equal(d, s_r.sha, 32);
}

static bool aread_valid(int i, bool *ours)
{
    uint8_t d[32];
    extflash_read(DEV, AADDR[i], &s_r, HEAD + ASIZE);
    if (s_r.magic != AMAGIC) return false;
    *ours = true;
    if (s_r.schema != ASCHEMA || s_r.size != ASIZE) return false;
    digest(&s_r, d);
    return ct_equal(d, s_r.sha, 32);
}

/* Nothing stored: anyone. Something stored that cannot be read: nobody, rather than
 * silently opening ports that were restricted. */
static void access_load(cfg_t *c)
{
    bool ours = false, ok = true;
    s_acur = -1;
    s_aseq = 0;
    memset(c->access, 0, sizeof c->access);
    for (int i = 0; i < 2; i++) {
        if (!aread_valid(i, &ours) || (s_acur >= 0 && s_r.seq <= s_aseq)) continue;
        s_acur = i;
        s_aseq = s_r.seq;
        memcpy(c->access, s_r.body, ASIZE);
    }
    for (int p = 0; p < PORTS; p++) ok = ok && acl_valid(&c->access[p]);
    if ((s_acur < 0 && ours) || !ok)
        for (int p = 0; p < PORTS; p++) acl_deny_all(&c->access[p]);
    memcpy(s_saved, c->access, sizeof s_saved);
}

/* The magic goes last, so a power cut leaves this copy looking blank, not damaged. */
static bool access_save(const cfg_t *c)
{
    int i = s_acur == 0 ? 1 : 0;
    bool ours;
    memset(&s_w, 0, sizeof s_w);
    s_w.magic = AMAGIC;
    s_w.schema = ASCHEMA;
    s_w.seq = s_aseq + 1;
    s_w.size = ASIZE;
    memcpy(s_w.body, c->access, ASIZE);
    digest(&s_w, s_w.sha);
    if (!extflash_erase(DEV, AADDR[i], 4096)) return false;
    if (!extflash_write(DEV, AADDR[i] + 4, &s_w.schema, HEAD + ASIZE - 4)) return false;
    if (!extflash_write(DEV, AADDR[i], &s_w.magic, 4)) return false;
    if (!aread_valid(i, &ours) || memcmp(&s_r, &s_w, HEAD + ASIZE)) return false;
    s_acur = i;
    s_aseq = s_w.seq;
    memcpy(s_saved, c->access, sizeof s_saved);
    return true;
}

bool cfg_load(cfg_t *c)
{
    char err[96];
    s_cur = -1;
    s_seq = 0;
    for (int i = 0; i < 2; i++) {
        if (!read_valid(i) || (s_cur >= 0 && s_r.seq <= s_seq)) continue;
        s_cur = i;
        s_seq = s_r.seq;
        cfg_defaults(c);                 /* newer fields, if this is an old record */
        memcpy(c, s_r.body, s_r.size);
    }
    bool ok = s_cur >= 0 && cfg_validate(c, err, sizeof err);
    if (!ok) cfg_defaults(c);
    access_load(c);
    return ok;
}

static bool main_save(const cfg_t *c)
{
    int i = s_cur == 0 ? 1 : 0;         /* never the copy we would fall back to */
    memset(&s_w, 0, sizeof s_w);
    s_w.magic = MAGIC;
    s_w.schema = SCHEMA;
    s_w.seq = s_seq + 1;
    s_w.size = SIZE2;
    memcpy(s_w.body, c, SIZE2);
    digest(&s_w, s_w.sha);
    if (!extflash_erase(DEV, ADDR[i], 4096)) return false;
    if (!extflash_write(DEV, ADDR[i], &s_w, sizeof s_w)) return false;
    if (!read_valid(i) || memcmp(&s_r, &s_w, sizeof s_w)) return false;
    s_cur = i;
    s_seq = s_w.seq;
    return true;
}

/* Lists first: a setting that needs its list is never stored ahead of it. If only
 * the second write fails, the new lists still apply from the next start. */
bool cfg_save(const cfg_t *c)
{
    if (memcmp(s_saved, c->access, sizeof s_saved) && !access_save(c)) return false;
    return main_save(c);
}
