/* cfg_store.h — Two alternating copies of cfg_t in blank space of ext flash 1. */
#ifndef CFG_STORE_H
#define CFG_STORE_H
#include "cfg.h"
bool cfg_load(cfg_t *c);              /* false: nothing valid, defaults loaded */
bool cfg_save(const cfg_t *c);        /* written, read back and compared */
#endif
