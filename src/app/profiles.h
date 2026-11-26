/* profiles.h — The built-in device profiles: the files in profiles/, embedded at build time. */
#ifndef PROFILES_H
#define PROFILES_H
#include <stddef.h>

const char *profiles_text(const char *id, size_t *len);   /* NULL: none by that id */
const char *profiles_id(int i);                           /* the i-th one's id; NULL past the last */

#endif
