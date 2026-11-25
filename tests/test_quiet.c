/* cc -Isrc/app tests/test_quiet.c -o /tmp/q && /tmp/q */
#include "quiet.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    static const uint32_t T[] = { 0, 5000, 0xFFFFFFFFu - 30000u };      /* tick wrap too */
    for (int i = 0; i < 3; i++) {
        uint32_t t = T[i];
        assert(quiet_eval(t + 999999, 0, t, t) == QUIET_OK);            /* off */
        assert(quiet_eval(t + 59999, 1, t, t) == QUIET_OK);
        assert(quiet_eval(t + 60000, 1, t, t + 59999) == QUIET_SILENT);
        assert(quiet_eval(t + 60000, 1, t + 1, t) == QUIET_NO_CLIENT);
        assert(quiet_eval(t + 60000, 1, t, t) == QUIET_SILENT);         /* both: silence first */
        assert(quiet_eval(t + 86399999u, 1440, t, t) == QUIET_OK);
        assert(quiet_eval(t + 86400000u, 1440, t, t) == QUIET_SILENT);
        uint32_t at = t, now = t;                                       /* silent for 200 days */
        for (int h = 0; h < 4800; h++) { now += 3600000u; at = quiet_clamp(now, at); if (h) assert(quiet_eval(now, 60, at, now) == QUIET_SILENT); }
    }
    puts("ok");
    return 0;
}
