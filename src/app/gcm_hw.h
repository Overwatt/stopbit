/* gcm_hw.h — AES-GCM for TLS records (gcm_hw.c). */
#ifndef GCM_HW_H
#define GCM_HW_H
#include <stdint.h>

uint32_t gcm_hw_cycles(void);   /* CPU cycles spent in AES-GCM so far (wraps) */

#endif
