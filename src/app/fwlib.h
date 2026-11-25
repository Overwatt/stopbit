/*
 * fwlib.h — Firmware images kept in external flash, beside the two boot slots.
 *
 * An upload is written to a library space as it arrives and checked there. Only
 * then is it copied to the spare boot slot, which the loader starts on trial.
 * Rolling back copies a kept image the same way. Boot slots belong to slots.c;
 * the loader knows nothing about this library.
 *
 * Nothing here blocks for longer than one flash page or a 1 KB read: fwlib_poll()
 * does one small step at a time.
 */
#ifndef FWLIB_H
#define FWLIB_H
#include "slots.h"
#include <stddef.h>

#define FWLIB_COUNT 7
#define FWLIB_RING  16384u

typedef struct {
    slot_hdr_t hdr;
    bool valid;
    bool tried;              /* has been copied to a boot slot */
    bool kept;               /* has passed a trial */
} fwlib_entry_t;

void fwlib_get(int i, fwlib_entry_t *out);

/* Start taking an upload of `size` bytes. `running` is the boot slot in use (or
 * SLOT_NONE). `need`, if not NULL, is text an image must contain to be started.
 * NULL, or why not. */
const char *fwlib_receive(uint32_t size, const uint8_t sha[32], int running, const char *need);
/* How much fwlib_feed() would take. */
size_t fwlib_room(void);
/* Takes all `n` bytes. The caller keeps no more than FWLIB_RING undrained. */
void fwlib_feed(const uint8_t *d, size_t n);
/* Start installing an image from the library. */
const char *fwlib_rollback(const uint8_t sha[32], int running, const char *need);

typedef enum { FW_IDLE, FW_BUSY, FW_DONE, FW_FAILED } fw_result_t;
/* FW_DONE and FW_FAILED are reported once. *drained: upload bytes consumed. */
fw_result_t fwlib_poll(uint32_t *drained);
const char *fwlib_error(void);
int  fwlib_slot(void);       /* where FW_DONE put it */
bool fwlib_active(void);
/* Gives up. Whatever was being written stays invalid. */
void fwlib_cancel(void);

/* The image in this boot slot passed its trial. */
void fwlib_mark_kept(int slot);
/* Returns length, or 0 if it did not fit. */
size_t fwlib_json(int running, char *out, size_t max);
#endif
