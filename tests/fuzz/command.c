/* command.c — commands for every control of every built-in profile (src/app/pfwrite.c): a
 * field picked by the first byte, the rest as the payload. What must hold: refused with a
 * reason, or a write of 1 or 2 registers at the field's address whose value lies in its mask,
 * and a merge that changes only masked bits. */
#include "pfwrite.h"
#include "profiles.h"
#include <assert.h>
#include <string.h>

static pf_build_t B[8];
static profile_t P[8];
static int np = -1;

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (np < 0) {
        char e[96];
        size_t len;
        for (np = 0; np < 8 && profiles_id(np); np++) {
            const char *t = profiles_text(profiles_id(np), &len);
            assert(profile_parse(&B[np], t, len, e, sizeof e));
            profile_view(&B[np], &P[np]);
        }
    }
    if (n < 2 || n > 64 || !np) return 0;
    const profile_t *p = &P[d[0] % np];
    int i = d[1] % p->nfield;
    char s[64];
    memcpy(s, d + 2, n - 2);
    s[n - 2] = 0;
    pf_write_t w;
    char why[32] = "";
    if (!pf_command(p, i, s, &w, why, sizeof why)) { assert(why[0]); return 0; }
    assert(pf_control(&p->field[i]) && w.addr == p->field[i].addr && (w.regs == 1 || w.regs == 2));
    uint16_t have[2] = { 0xA5A5, 0x5A5A }, out[2];
    pf_merge(&w, have, out);
    for (int r = 0; r < w.regs; r++) assert(!(w.value[r] & ~w.mask[r]) && !((out[r] ^ have[r]) & ~w.mask[r]));
    return 0;
}
