/* roots.h — Root certificates for outgoing connections (DER). */
#ifndef ROOTS_H
#define ROOTS_H
#include <stddef.h>
typedef struct { const unsigned char *der; size_t len; } root_t;
extern const root_t g_roots[];
extern const unsigned g_nroots;
#endif
