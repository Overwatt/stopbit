/* profiles.c — The built-in device profiles (profiles.h). */
#include "profiles.h"
#include "profiles_gen.h"
#include <string.h>

#define N (int)(sizeof BUILTIN / sizeof BUILTIN[0])

const char *profiles_text(const char *id, size_t *len)
{
    for (int i = 0; i < N; i++)
        if (!strcmp(BUILTIN[i].id, id)) { *len = BUILTIN[i].len; return BUILTIN[i].text; }
    return NULL;
}

const char *profiles_id(int i) { return i >= 0 && i < N ? BUILTIN[i].id : NULL; }
