/*
 * slots.h — Two application slots in external flash 1, with trial boots.
 *
 * Both sit in space that was blank on the stock unit, so no stock content is
 * overwritten. Boot flags are NOR bits that only ever go 1 -> 0 after install:
 *   attempts[i]  cleared one per boot while the image is unconfirmed
 *   confirmed    cleared once the image has proved itself
 * An unconfirmed image gets SLOT_TRIES boots; after that the loader falls back
 * to the other slot, or stays in network mode.
 */
#ifndef SLOTS_H
#define SLOTS_H
#include <stdint.h>
#include <stdbool.h>

#define SLOT_COUNT 2
#define SLOT_TRIES 3
#define SLOT_NONE  (-1)

typedef struct {
    uint32_t magic, seq, size;
    uint8_t  sha[32];
    char     version[24];
} slot_hdr_t;

typedef struct {
    slot_hdr_t hdr;
    bool valid, confirmed;
    int  tries_left;
} slot_info_t;

void slots_get(int slot, slot_info_t *out);
/* Copies the best bootable slot to STAGE_ADDR. Returns its index, or SLOT_NONE. */
int  slots_load_best(void);
/* Installs the image at STAGE_ADDR. Returns an error string, or NULL. */
const char *slots_install(uint32_t size, int *slot);
bool slots_confirm(int slot);

/* Backup registers: survive resets (not power loss). */
void     bkp_init(void);
uint32_t bkp_get(int i);
void     bkp_set(int i, uint32_t v);
enum { BKP_STAY = 0, BKP_SLOT = 1, BKP_CRASHES = 2 };
#define BKP_STAY_MAGIC 0x53544159u   /* stay in the loader on next boot */
#define CRASH_LIMIT    5u            /* boots without 30 s of app uptime */
#endif
