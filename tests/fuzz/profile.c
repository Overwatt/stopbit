/* profile.c — Any text as a profile (src/app/profile.c). What parses must hold together:
 * every index inside its table, every string inside the pool; and decoding any reply of
 * the right size for each block (pfdecode.c, from Task 3) must not fail. */
#include "profile.h"
#include "pfdecode.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static pf_build_t B;
static profile_t P;
static pf_state_t S;

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static char err[160];
    static uint8_t data[250], txt[200];
    if (!profile_parse(&B, (const char *)d, n, err, sizeof err)) { assert(err[0]); return 0; }
    /* Packed, as device.c keeps it, into just the bytes it asks for: a table read past
     * its end is then read past the allocation. */
    size_t need = profile_size(&B);
    void *mem = malloc(need);
    const profile_t *q = profile_pack(&B, mem, need);
    assert(q && !profile_pack(&B, mem, need - 8));
    P = *q;
    memset(&B, 0xA5, sizeof B);
    assert(P.nblock >= 1 && P.nblock <= PF_BLOCKS && P.nfield <= PF_FIELDS && P.npool <= PF_POOL && !P.pool[0]);
    for (int i = 0; i < P.nfield; i++) {
        const pf_field_t *f = &P.field[i];
        assert(f->id < P.npool && f->name < P.npool && f->unit < P.npool && f->dc < P.npool && f->sc < P.npool);
        assert(f->kind >= PF_CALC ? f->block == 0xFF : f->block < P.nblock);
        assert((uint32_t)f->vname + f->nvname <= P.nvname && (uint32_t)f->arg + f->narg <= P.narg);
        for (int k = 0; k < f->narg; k++) assert(P.arg[f->arg + k] < i);
    }
    pf_state_init(&S);
    for (int b = 0; b < P.nblock; b++) {
        size_t len = P.block[b].fn <= 2 ? (P.block[b].count + 7u) / 8u : P.block[b].count * 2u;
        for (size_t k = 0; k < len; k++) data[k] = (uint8_t)(n + k * 37 + b);
        (void)pf_decode(&P, b, data, len, &S);
        (void)pf_decode(&P, b, data, len ? len - 1 : 1, &S);   /* the wrong size: refused */
        pf_drop(&P, b, &S);
        (void)pf_decode(&P, b, data, len, &S);
    }
    for (int i = 0; i < P.nfield; i++) { (void)pf_text(&P, i, &S, (char *)txt, sizeof txt); (void)pf_num((char *)txt, sizeof txt, S.val[i].v, pf_prec(&P.field[i])); }
    free(mem);
    return 0;
}
