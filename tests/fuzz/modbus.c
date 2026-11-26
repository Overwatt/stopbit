/* modbus.c — Modbus TCP to RTU (src/app/modbus.c) driven by a script: client bytes in
 * any pieces from any of the four, bytes heard on the line, polls, time passing,
 * clients coming and going, a line with no room. What must hold throughout: a frame put
 * on the line is whole RTU with a good CRC; a reply goes only to an open client and is
 * whole Modbus TCP (protocol 0, the length it says); at most MB_QUEUE wait. */
#include "modbus.h"
#include <assert.h>
#include <string.h>

static modbus_t M;
static bool full, open_[MB_CLIENTS];

static size_t line(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    assert(n >= 4 && n <= 1 + MB_PDU + 2);
    assert(modbus_crc(d, n - 2) == (uint16_t)(d[n - 2] | d[n - 1] << 8));
    return full ? 0 : n;
}

static void reply(void *ctx, int c, const uint8_t *d, size_t n)
{
    (void)ctx;
    assert(c >= 0 && c < MB_CLIENTS && open_[c]);
    assert(n >= 9 && n <= 7 + MB_PDU);
    assert(d[2] == 0 && d[3] == 0 && (size_t)(d[4] << 8 | d[5]) == n - 6);
}

static const mb_io_t IO = { line, reply, NULL };

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 2) return 0;
    modbus_init(&M, &IO, 1u + (d[0] >> 1) * 16u, 2000);
    M.broadcast = d[0] & 1;
    memset(open_, 0, sizeof open_);
    full = false;
    uint32_t now = 0;
    for (size_t i = 1; i < n;) {
        uint8_t op = d[i++];
        int c = op >> 3 & 3;
        size_t k = 0;
        if ((op & 7) <= 1 && i < n) {                   /* a length, then that many bytes */
            k = d[i++] % 64u;
            if (k > n - i) k = n - i;
        }
        switch (op & 7) {
        case 0:                                         /* a client sends */
            if (!open_[c]) { modbus_open(&M, c, op & 0x20); open_[c] = true; }
            if (!modbus_from_client(&M, c, d + i, k, now)) { modbus_close(&M, c); open_[c] = false; }
            i += k;
            break;
        case 1:                                         /* the line hears */
            modbus_from_line(&M, d + i, k);
            i += k;
            break;
        case 2: modbus_poll(&M, now, (uint32_t)(op >> 3) * 250u, op & 0x80); break;
        case 3: now += (uint32_t)(i < n ? d[i++] : 0) * 10u; break;
        case 4: if (open_[c]) { modbus_close(&M, c); open_[c] = false; } break;
        case 5: if (!open_[c]) { modbus_open(&M, c, op & 0x20); open_[c] = true; } break;
        case 6: if (open_[c]) modbus_may_write(&M, c, op & 0x20); break;
        default: full = !full;
        }
        assert(M.qn <= MB_QUEUE && M.state <= MB_TURNAROUND);
    }
    return 0;
}
