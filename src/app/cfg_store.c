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
/* Later settings (more_cfg_t): their own record too, for the same reason. Unlike the
 * lists, one that cannot be read just means defaults. */
#define MMAGIC  0x45524F4Du
#define MSCHEMA 1u
#define MSIZE   sizeof(more_cfg_t)
_Static_assert(sizeof(more_cfg_t) == 256, "later settings fill their spare bytes, never grow");
static const uint32_t MADDR[2] = { 0x0FA000u, 0x0FB000u };
/* Devices (devN.*): their own record too. One that cannot be read: no devices. */
#define DMAGIC  0x56454444u
#define DSCHEMA 1u
#define DSIZE   (DEVS * sizeof(dev_cfg_t))
_Static_assert(sizeof(dev_cfg_t) == 112, "device settings layout must not move");
static const uint32_t DADDR[2] = { 0x0EE000u, 0x0EF000u };

typedef struct {
    uint32_t magic, schema, seq, size;
    uint8_t  sha[32];                   /* over schema, seq, size and body */
    uint8_t  body[DSIZE > SIZE2 ? DSIZE : SIZE2];
} rec_t;
_Static_assert(DSIZE <= sizeof(((rec_t *)0)->body), "record too small");
_Static_assert(ASIZE <= SIZE2 && MSIZE <= SIZE2, "record too small");
_Static_assert(sizeof(rec_t) <= 4096, "config record exceeds a sector");

static rec_t s_r, s_w;                  /* too big for the stack */
static uint32_t s_seq;
static int s_cur = -1;
static acl_t s_saved[PORTS];            /* as last read or written */
static more_cfg_t s_msaved;
static dev_cfg_t s_dsaved[DEVS];

/* A record kept apart from the main one, in two alternating sectors. */
typedef struct { uint32_t magic, schema, size; const uint32_t *addr; int cur; uint32_t seq; } side_t;
static side_t s_acl  = { AMAGIC, ASCHEMA, ASIZE, AADDR, -1, 0 };
static side_t s_more = { MMAGIC, MSCHEMA, MSIZE, MADDR, -1, 0 };
static side_t s_dev  = { DMAGIC, DSCHEMA, DSIZE, DADDR, -1, 0 };

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

static bool side_read(const side_t *k, int i, bool *ours)
{
    uint8_t d[32];
    extflash_read(DEV, k->addr[i], &s_r, HEAD + k->size);
    if (s_r.magic != k->magic) return false;
    *ours = true;
    if (s_r.schema != k->schema || s_r.size != k->size) return false;
    digest(&s_r, d);
    return ct_equal(d, s_r.sha, 32);
}

/* The newer valid copy into `body`. 1: read; 0: something is there but cannot be
 * read; -1: nothing stored. */
static int side_load(side_t *k, void *body)
{
    bool ours = false;
    k->cur = -1;
    k->seq = 0;
    for (int i = 0; i < 2; i++) {
        if (!side_read(k, i, &ours) || (k->cur >= 0 && s_r.seq <= k->seq)) continue;
        k->cur = i;
        k->seq = s_r.seq;
        memcpy(body, s_r.body, k->size);
    }
    return k->cur >= 0 ? 1 : ours ? 0 : -1;
}

/* The magic goes last, so a power cut leaves this copy looking blank, not damaged. */
static bool side_save(side_t *k, const void *body)
{
    int i = k->cur == 0 ? 1 : 0;
    bool ours;
    memset(&s_w, 0, sizeof s_w);
    s_w.magic = k->magic;
    s_w.schema = k->schema;
    s_w.seq = k->seq + 1;
    s_w.size = k->size;
    memcpy(s_w.body, body, k->size);
    digest(&s_w, s_w.sha);
    if (!extflash_erase(DEV, k->addr[i], 4096)) return false;
    if (!extflash_write(DEV, k->addr[i] + 4, &s_w.schema, HEAD + k->size - 4)) return false;
    if (!extflash_write(DEV, k->addr[i], &s_w.magic, 4)) return false;
    if (!side_read(k, i, &ours) || memcmp(&s_r, &s_w, HEAD + k->size)) return false;
    k->cur = i;
    k->seq = s_w.seq;
    return true;
}

/* Nothing stored: anyone. Something stored that cannot be read: nobody, rather than
 * silently opening ports that were restricted. */
static void access_load(cfg_t *c)
{
    bool ok = true;
    memset(c->access, 0, sizeof c->access);
    int r = side_load(&s_acl, c->access);
    for (int p = 0; p < PORTS; p++) ok = ok && acl_valid(&c->access[p]);
    if (r == 0 || !ok)
        for (int p = 0; p < PORTS; p++) acl_deny_all(&c->access[p]);
    memcpy(s_saved, c->access, sizeof s_saved);
}

static bool access_save(const cfg_t *c)
{
    if (!side_save(&s_acl, c->access)) return false;
    memcpy(s_saved, c->access, sizeof s_saved);
    return true;
}

/* Anything not stored, or not readable, is its default. */
static void more_load(cfg_t *c)
{
    memset(&c->more, 0, sizeof c->more);
    if (side_load(&s_more, &c->more) != 1) memset(&c->more, 0, sizeof c->more);
    c->more.ntp[sizeof c->more.ntp - 1] = c->more.topic[sizeof c->more.topic - 1] = 0;
    s_msaved = c->more;
}

static void dev_load(cfg_t *c)
{
    memset(c->dev, 0, sizeof c->dev);
    if (side_load(&s_dev, c->dev) != 1) memset(c->dev, 0, sizeof c->dev);
    for (int i = 0; i < DEVS; i++) {                    /* terminated, whatever was stored */
        dev_cfg_t *d = &c->dev[i];
        d->profile[sizeof d->profile - 1] = d->name[sizeof d->name - 1] = 0;
        d->label[sizeof d->label - 1] = d->prefix[sizeof d->prefix - 1] = 0;
    }
    memcpy(s_dsaved, c->dev, sizeof s_dsaved);
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
    more_load(c);
    dev_load(c);
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
    if (memcmp(&s_msaved, &c->more, sizeof s_msaved)) {
        if (!side_save(&s_more, &c->more)) return false;
        s_msaved = c->more;
    }
    if (memcmp(s_dsaved, c->dev, sizeof s_dsaved)) {
        if (!side_save(&s_dev, c->dev)) return false;
        memcpy(s_dsaved, c->dev, sizeof s_dsaved);
    }
    return main_save(c);
}
