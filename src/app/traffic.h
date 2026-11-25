/* traffic.h — The last few packets seen on each port, for the console's `log`. */
#ifndef TRAFFIC_H
#define TRAFFIC_H
#include "cfg.h"
#define TRAFFIC_N    32
#define TRAFFIC_DATA 48
typedef struct {
    uint32_t ms;
    uint16_t len;                   /* real length; data[] may hold less */
    uint8_t  tx;                    /* 1: sent on the serial line, 0: received */
    uint8_t  data[TRAFFIC_DATA];
} traffic_t;
void traffic_init(void);
void traffic_add(int port, bool tx, const uint8_t *d, size_t n, uint32_t ms);
/* k = 0 is the newest. False when there are no more. */
bool traffic_get(int port, int k, traffic_t *out);
void traffic_clear(int port);
#endif
