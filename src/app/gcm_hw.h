/* gcm_hw.h — AES-GCM for TLS records on the chip's crypto engine (gcm_hw.c). */
#ifndef GCM_HW_H
#define GCM_HW_H
#include <stddef.h>
#include <stdint.h>

void     gcm_hw_init(void);                    /* clock it, check it; software if it disagrees */
uint32_t gcm_hw_cycles(void);                  /* CPU cycles spent in AES-GCM so far (wraps) */
void     gcm_hw_counts(uint32_t *hw, uint32_t *sw); /* records done by the engine and in software */
size_t   gcm_hw_json(char *out, size_t max);   /* {"engine":..,"why":..,"hw":n,"sw":n}; 0 if it does not fit */

#endif
