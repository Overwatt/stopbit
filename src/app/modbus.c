/*
 * modbus.c — Modbus TCP to Modbus RTU for one port (see modbus.h).
 *
 * A request: MBAP header (transaction, protocol 0, length, unit) and PDU (function,
 * data). On the line: unit, PDU, CRC-16 low byte first. A reply is a frame that ends
 * in a silence of gap_us, has a good CRC and the request's unit and function (or the
 * function | 0x80: an exception). Anything else heard while waiting is counted and
 * ignored; the wait goes on until the timeout.
 */
#include "modbus.h"
#include <string.h>

enum { EX_ILLEGAL_FUNCTION = 0x01, EX_BUSY = 0x06, EX_NO_PATH = 0x0A, EX_NO_RESPONSE = 0x0B };

uint16_t modbus_crc(const uint8_t *d, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= *d++;
        for (int b = 0; b < 8; b++) c = c & 1 ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
    }
    return c;
}

void modbus_init(modbus_t *m, const mb_io_t *io, uint32_t timeout_ms, uint32_t gap_us)
{
    memset(m, 0, sizeof *m);
    m->io = *io;
    m->timeout_ms = timeout_ms;
    m->gap_us = gap_us;
}

void modbus_open(modbus_t *m, int c, bool may_write)
{
    m->cl[c].n = 0;
    m->cl[c].open = true;
    m->cl[c].may_write = may_write;
}

void modbus_may_write(modbus_t *m, int c, bool may_write) { m->cl[c].may_write = may_write; }

void modbus_close(modbus_t *m, int c)
{
    m->cl[c].open = false;
    m->cl[c].n = 0;
    uint8_t keep = 0;
    for (uint8_t i = 0; i < m->qn; i++) {
        if (i == 0 && m->state != MB_IDLE) {            /* on the line: let it finish */
            if (m->q[0].client == c) m->q[0].client = -1;
            m->q[keep++] = m->q[0];
        } else if (m->q[i].client != c) {
            if (keep != i) m->q[keep] = m->q[i];
            keep++;
        }
    }
    m->qn = keep;
}

/* An answer of our own: the request's header, the function with 0x80, the code. */
static void exception(modbus_t *m, int c, uint16_t tid, uint8_t unit, uint8_t fn, uint8_t code)
{
    uint8_t r[9] = { (uint8_t)(tid >> 8), (uint8_t)tid, 0, 0, 0, 3, unit, (uint8_t)(fn | 0x80), code };
    if (c >= 0 && m->cl[c].open) m->io.reply(m->io.ctx, c, r, sizeof r);
}

static bool may_read_only(const uint8_t *pdu, uint16_t len)
{
    uint8_t fn = pdu[0];
    if (fn >= 1 && fn <= 4) return true;
    return fn == 43 && len >= 2 && pdu[1] == 0x0E;     /* read device identification */
}

static void take(modbus_t *m, int c, const uint8_t *a, uint16_t len)
{
    uint16_t tid = (uint16_t)(a[0] << 8 | a[1]);
    uint8_t unit = a[6];
    const uint8_t *pdu = a + 7;
    uint16_t plen = (uint16_t)(len - 1);
    m->s.requests++;
    if (unit == 0 ? !m->broadcast : unit > 247) {     /* a client left at its default must not reach them all */
        m->s.refused++;
        exception(m, c, tid, unit, pdu[0], EX_NO_PATH);
        return;
    }
    if (!m->cl[c].may_write && !may_read_only(pdu, plen)) {
        m->s.refused++;
        exception(m, c, tid, unit, pdu[0], EX_ILLEGAL_FUNCTION);
        return;
    }
    if (m->qn == MB_QUEUE) {
        m->s.busy++;
        exception(m, c, tid, unit, pdu[0], EX_BUSY);
        return;
    }
    mb_req_t *r = &m->q[m->qn++];
    r->client = (int8_t)c;
    r->unit = unit;
    r->tid = tid;
    r->len = plen;
    memcpy(r->pdu, pdu, plen);
}

bool modbus_from_client(modbus_t *m, int c, const uint8_t *d, size_t n, uint32_t now_ms)
{
    (void)now_ms;
    if (!m->cl[c].open) return false;
    while (n) {
        uint8_t *b = m->cl[c].buf;
        uint16_t have = m->cl[c].n;
        size_t want = 7;                                /* the header first */
        if (have >= 6) want = 6u + (size_t)(b[4] << 8 | b[5]);
        size_t k = want - have < n ? want - have : n;
        memcpy(b + have, d, k);
        m->cl[c].n = have = (uint16_t)(have + k);
        d += k;
        n -= k;
        if (have == 6 || have == 7) {                   /* the header is whole: check it */
            uint16_t pid = (uint16_t)(b[2] << 8 | b[3]), len = (uint16_t)(b[4] << 8 | b[5]);
            if (pid != 0 || len < 2 || len > 1 + MB_PDU) return false;
        }
        if (have >= 7 && have == 6u + (uint16_t)(b[4] << 8 | b[5])) {
            take(m, c, b, (uint16_t)(have - 6));
            m->cl[c].n = 0;
        }
    }
    return true;
}

void modbus_from_line(modbus_t *m, const uint8_t *d, size_t n)
{
    if (m->state != MB_WAITING) return;                 /* our own echo, or nobody asked */
    for (size_t i = 0; i < n; i++) {
        if (m->rxn < sizeof m->rx) m->rx[m->rxn++] = d[i];
        else m->rx_over = true;
    }
}

/* The frame heard is the reply to q[0]? */
static bool is_reply(const modbus_t *m)
{
    const mb_req_t *r = &m->q[0];
    if (m->rx_over || m->rxn < 4) return false;
    uint16_t crc = (uint16_t)(m->rx[m->rxn - 2] | m->rx[m->rxn - 1] << 8);
    if (modbus_crc(m->rx, m->rxn - 2u) != crc) return false;
    return m->rx[0] == r->unit && (m->rx[1] & 0x7F) == r->pdu[0] && (m->rx[1] == r->pdu[0] || m->rxn == 5);
}

static void done(modbus_t *m)
{
    memmove(m->q, m->q + 1, (size_t)(m->qn - 1) * sizeof m->q[0]);
    m->qn--;
    m->state = MB_IDLE;
    m->rxn = 0;
    m->rx_over = false;
}

void modbus_poll(modbus_t *m, uint32_t now, uint32_t quiet_us, bool tx_idle)
{
    mb_req_t *r = &m->q[0];
    switch (m->state) {
    case MB_IDLE: {
        if (!m->qn || !tx_idle || quiet_us < m->gap_us) return;
        uint8_t f[1 + MB_PDU + 2];
        f[0] = r->unit;
        memcpy(f + 1, r->pdu, r->len);
        uint16_t crc = modbus_crc(f, 1u + r->len);
        f[1 + r->len] = (uint8_t)crc;
        f[2 + r->len] = (uint8_t)(crc >> 8);
        if (m->io.line(m->io.ctx, f, 3u + r->len) != 3u + r->len) return;   /* no room yet */
        m->state = MB_SENDING;
        m->rxn = 0;
        m->rx_over = false;
        return;
    }
    case MB_SENDING:
        if (!tx_idle) return;
        m->at = now;
        if (r->unit == 0) { m->s.broadcasts++; m->state = MB_TURNAROUND; }
        else m->state = MB_WAITING;
        return;
    case MB_TURNAROUND:
        if (now - m->at >= MB_TURN_MS) done(m);
        return;
    case MB_WAITING:
        if (m->rxn && quiet_us >= m->gap_us) {          /* a whole frame */
            if (is_reply(m)) {
                uint32_t ms = now - m->at;
                uint16_t plen = (uint16_t)(m->rxn - 3);
                uint8_t a[7 + MB_PDU];
                a[0] = (uint8_t)(r->tid >> 8); a[1] = (uint8_t)r->tid;
                a[2] = 0; a[3] = 0;
                a[4] = (uint8_t)((plen + 1) >> 8); a[5] = (uint8_t)(plen + 1);
                a[6] = r->unit;
                memcpy(a + 7, m->rx + 1, plen);
                m->s.replies++;
                if (m->rx[1] & 0x80) m->s.exceptions++;
                m->s.last_ms = ms;
                if (ms > m->s.max_ms) m->s.max_ms = ms;
                m->s.total_ms += ms;
                if (r->client >= 0 && m->cl[r->client].open) m->io.reply(m->io.ctx, r->client, a, 7u + plen);
                done(m);
                return;
            }
            m->s.bad++;
            m->rxn = 0;
            m->rx_over = false;
        }
        if (now - m->at >= m->timeout_ms) {
            m->s.timeouts++;
            exception(m, r->client, r->tid, r->unit, r->pdu[0], EX_NO_RESPONSE);
            done(m);
        }
        return;
    }
}
