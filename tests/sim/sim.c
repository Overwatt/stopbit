/* sim.c — The gateway's web server, firmware library and boot slots on a computer.
 * Real http.c, fwlib.c and slots.c; simulated flash, sockets, loader and settings.
 * Build and run: tests/sim/run.sh. Listens on 127.0.0.1:8541. */
#include "http.h"
#include "api.h"
#include "cli.h"
#include "fwlib.h"
#include "fw_id.h"
#include "net.h"
#include "eth_mac.h"
#include "tls.h"
#include "worker.h"
#include "sha256.h"
#include "system.h"
#include "stm32h7xx_hal.h"
#include "lwip/tcp.h"
#include "../flash_sim.h"
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- TCP ----------------------------------------------------------------- */
struct tcp_pcb {
    int fd; bool used, listening, closing;
    void *arg; tcp_accept_fn acc; tcp_recv_fn rx; tcp_sent_fn sent; tcp_err_fn err; tcp_poll_fn poll;
    int wnd; uint8_t out[TCP_SND_BUF]; int out_n; bool eof; uint32_t polled;
};
static struct tcp_pcb P[40];
t_dwt_t t_dwt;
uint32_t t_dwt_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec) * 2 / 5); }   /* 400 MHz */
static int s_max_gap;                        /* longest the main loop was away, ms */
static bool s_reboot;

static void drop(struct tcp_pcb *p) { if (p->fd >= 0) close(p->fd); memset(p, 0, sizeof *p); p->fd = -1; }
static int s_pbufs;
void pbuf_free(struct pbuf *p) { while (p && --p->ref == 0) { struct pbuf *n = p->next; free(p->payload); free(p); s_pbufs--; p = n; } }
void pbuf_ref(struct pbuf *p) { p->ref++; }
void pbuf_cat(struct pbuf *h, struct pbuf *t)
{
    for (; h->next; h = h->next) h->tot_len += t->tot_len;
    h->tot_len += t->tot_len;
    h->next = t;
}
struct tcp_pcb *tcp_new(void)
{
    for (int i = 0; i < 40; i++) if (!P[i].used) { memset(&P[i], 0, sizeof P[i]); P[i].used = true; P[i].fd = -1; P[i].wnd = TCP_WND; return &P[i]; }
    return NULL;
}
err_t tcp_bind(struct tcp_pcb *p, const void *a, u16_t port)
{
    (void)a;
    struct sockaddr_in s = { .sin_family = AF_INET, .sin_port = htons(port == 443 ? 8543 : 8541), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int one = 1;
    p->fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(p->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    return bind(p->fd, (void *)&s, sizeof s) ? ERR_VAL : ERR_OK;
}
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *p, u8_t n)
{ listen(p->fd, n); fcntl(p->fd, F_SETFL, O_NONBLOCK); p->listening = true; return p; }
void tcp_accept(struct tcp_pcb *p, tcp_accept_fn f) { p->acc = f; }
void tcp_arg(struct tcp_pcb *p, void *a) { p->arg = a; }
void tcp_recv(struct tcp_pcb *p, tcp_recv_fn f) { p->rx = f; }
void tcp_sent(struct tcp_pcb *p, tcp_sent_fn f) { p->sent = f; }
void tcp_err(struct tcp_pcb *p, tcp_err_fn f) { p->err = f; }
void tcp_poll(struct tcp_pcb *p, tcp_poll_fn f, u8_t n) { (void)n; p->poll = f; }
void tcp_nagle_disable(struct tcp_pcb *p) { (void)p; }
u16_t tcp_sndbuf(struct tcp_pcb *p) { return (u16_t)(TCP_SND_BUF - p->out_n); }
u16_t tcp_sndqueuelen(struct tcp_pcb *p) { return (u16_t)(p->out_n / TCP_MSS); }
err_t tcp_write(struct tcp_pcb *p, const void *d, u16_t n, u8_t f)
{
    (void)f;
    assert(!p->closing && p->used);
    if (n > TCP_SND_BUF - p->out_n) return ERR_MEM;
    memcpy(p->out + p->out_n, d, n);
    p->out_n += n;
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *p) { (void)p; return ERR_OK; }
void tcp_recved(struct tcp_pcb *p, u16_t n)
{
    assert(p->used);
    p->wnd += n;
    if (p->wnd > TCP_WND) { printf("ACKNOWLEDGED MORE THAN ARRIVED: %d\n", p->wnd); abort(); }
}
void tcp_abort(struct tcp_pcb *p)
{
    struct linger l = { 1, 0 };
    tcp_err_fn e = p->err; void *a = p->arg;
    setsockopt(p->fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
    drop(p);
    if (e) e(a, ERR_ABRT);
}
err_t tcp_close(struct tcp_pcb *p)
{
    if (p->listening) { drop(p); return ERR_OK; }
    if (p->wnd != TCP_WND) {                 /* lwIP: unacknowledged data means a reset */
        struct linger l = { 1, 0 };
        printf("close with %d bytes unacknowledged: reset\n", TCP_WND - p->wnd);
        setsockopt(p->fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
        drop(p);
        return ERR_OK;
    }
    p->closing = true;
    return ERR_OK;
}
static void serve(struct tcp_pcb *p)
{
    if (p->listening) {
        int fd = accept(p->fd, NULL, NULL);
        if (fd < 0) return;
        struct tcp_pcb *n = tcp_new();
        if (!n) { close(fd); return; }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        n->fd = fd;
        n->polled = t_now;
        p->acc(p->arg, n, ERR_OK);
        return;
    }
    if (p->out_n) {
        ssize_t k = send(p->fd, p->out, (size_t)p->out_n, 0);
        if (k > 0) {
            memmove(p->out, p->out + k, (size_t)(p->out_n - k));
            p->out_n -= (int)k;
            if (p->sent && !p->closing) { if (p->sent(p->arg, p, (u16_t)k) == ERR_ABRT) return; }
        } else if (errno != EAGAIN) { tcp_err_fn e = p->err; void *a = p->arg; drop(p); if (e) e(a, ERR_RST); return; }
    }
    if (!p->used) return;
    if (p->closing) {
        if (!p->out_n) { shutdown(p->fd, SHUT_WR); drop(p); }
        return;
    }
    if (p->wnd > 0 && !p->eof) {
        uint8_t b[TCP_MSS];
        size_t want = p->wnd < TCP_MSS ? (size_t)p->wnd : TCP_MSS;
        ssize_t k = recv(p->fd, b, want, 0);
        if (k > 0) {
            p->wnd -= (int)k;
            size_t cut = k > 1 && rand() % 3 == 0 ? (size_t)rand() % (size_t)k : 0;   /* sometimes a chain */
            struct pbuf *h = NULL, **tail = &h;
            for (size_t at = 0; at < (size_t)k;) {
                size_t n = at == 0 && cut ? cut : (size_t)k - at;
                struct pbuf *q = calloc(1, sizeof *q);
                q->payload = malloc(n);
                memcpy(q->payload, b + at, n);
                q->len = (u16_t)n;
                q->ref = 1;
                s_pbufs++;
                q->tot_len = (u16_t)((size_t)k - at);
                *tail = q; tail = &q->next;
                at += n;
            }
            if (p->rx(p->arg, p, h, ERR_OK) == ERR_ABRT) return;
        } else if (k == 0) {
            p->eof = true;
            if (p->rx && p->rx(p->arg, p, NULL, ERR_OK) == ERR_ABRT) return;
        } else if (errno != EAGAIN) { tcp_err_fn e = p->err; void *a = p->arg; drop(p); if (e) e(a, ERR_RST); return; }
    }
    if (p->used && !p->closing && p->poll && t_now - p->polled >= 1000) { p->polled = t_now; p->poll(p->arg, p); }
}

/* ---- the rest of the gateway ---------------------------------------------- */
void sim_version(const char *);          /* id.c */
uint8_t g_mac[6] = { 2, 0x54, 0, 0, 0, 1 };
char g_hostname[16] = "n540-sim";
eth_info_t g_eth;
uint32_t g_reset_cause;
bool g_clock_hse = true;
static bool s_need;
void net_ip_str(char o[16]) { strcpy(o, "127.0.0.1"); }
bool rng_fill(void *d, uint32_t n) { arc4random_buf(d, n); return true; }
#include "web_page.h"
const uint8_t *api_page(uint32_t *n) { *n = sizeof WEB_PAGE; return WEB_PAGE; }
const uint8_t *api_web_key(void) { static uint8_t k[32]; memset(k, 0x77, 32); return k; }
const char *api_fw_need(void) { return s_need ? "listen_only" : NULL; }
bool api_is_post(const char *p) { return !strcmp(p, "/api/need") || !strcmp(p, "/api/check") || !strcmp(p, "/api/gap") || !strcmp(p, "/api/tls"); }
const char *api_post(const char *p, api_req_t *r, char *o, size_t n)
{
    if (!strcmp(p, "/api/need")) s_need = r->body[0] == '1';
    if (!strcmp(p, "/api/gap")) { snprintf(o, n, "{\"gap\":%d}", s_max_gap); s_max_gap = 0; return "200 OK"; }
    if (!strcmp(p, "/api/tls")) {
        const char *why = tls_command((const uint8_t *)r->body, r->len);
        snprintf(o, n, "%s", why ? why : "{}");
        return why ? "400 Bad Request" : "200 OK";
    }
    snprintf(o, n, "{}");
    return "200 OK";
}
int api_get(const char *p, char *o, size_t n)
{ if (!strcmp(p, "/api/tls")) return (int)tls_json(o, n);
  if (!strcmp(p, "/api/tls/csr")) return (int)tls_csr(o, n);
  if (!strcmp(p, "/api/pbufs")) return snprintf(o, n, "%d", s_pbufs);
  return strcmp(p, "/api/library") ? -1 : (int)fwlib_json((int)bkp_get(BKP_SLOT), o, n); }
void api_restart(void) { s_reboot = true; http_init(); for (;;) { extern jmp_buf s_loop; longjmp(s_loop, 1); } }
void NVIC_SystemReset(void) { api_restart(); }
jmp_buf s_loop;

/* What the loader does, less the jump. */
static void boot(void)
{
    slot_info_t in[2];
    for (int s = 0; s < 2; s++) slots_get(s, &in[s]);
    int first = (in[1].valid && (!in[0].valid || in[1].hdr.seq > in[0].hdr.seq)) ? 1 : 0, run = -1;
    for (int k = 0; k < 2 && run < 0; k++) {
        int s = first ^ k;
        if (!in[s].valid || (!in[s].confirmed && !in[s].tries_left)) continue;
        if (!in[s].confirmed) { const uint32_t z = 0; extflash_write(SLOT_DEV, slots_addr(s) + 0x110u + 4u * (uint32_t)(SLOT_TRIES - in[s].tries_left), &z, 4); }
        sha256_t c; uint8_t d[32];
        sha256_init(&c); sha256_update(&c, F[1] + slots_addr(s) + SLOT_HDR, in[s].hdr.size); sha256_final(&c, d);
        if (memcmp(d, in[s].hdr.sha, 32)) { printf("slot %d is damaged\n", s); continue; }
        run = s;
    }
    if (run < 0) { printf("NOTHING TO BOOT\n"); abort(); }
    bkp_set(BKP_SLOT, (uint32_t)run);
    bkp_set(BKP_BOOTS, bkp_get(BKP_BOOTS) + 1);
    f_nno = 0;
    f_no[f_nno++] = (range_t){ 1, 0x0C8000, 0x0F4000 };
    f_no[f_nno++] = (range_t){ 1, 0x0F8000, 0x100000 };
    f_no[f_nno++] = (range_t){ 0, 0x1F4000, 0x200000 };
    f_no[f_nno++] = (range_t){ 1, slots_addr(run) + SLOT_HDR, slots_addr(run) + 0x80000 };
    slots_get(run, &in[run]);
    sim_version(in[run].hdr.version);
    printf("boot %u: slot %d %s%s\n", bkp_get(BKP_BOOTS), run, in[run].hdr.version, in[run].confirmed ? "" : " (trial)");
    fflush(stdout);
}
static uint32_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < EXTFLASH_SIZE; i++) { F[0][i] = (uint8_t)rand(); F[1][i] = (uint8_t)(i < 0x9a000 ? rand() : 0xFF); }
    for (int s = 0; s < 2; s++) {            /* two kept images, as on the device */
        FILE *f = fopen(argv[1], "rb");
        static uint8_t b[STAGE_SIZE];
        uint32_t n = (uint32_t)fread(b, 1, sizeof b, f);
        int slot;
        fclose(f);
        snprintf((char *)b + FW_ID_OFFSET + 8, 24, "first-%d", s);
        const char *why = slots_install(b, n, s ? 0 : 1, &slot);
        if (why) { puts(why); return 1; }
        slots_confirm(slot);
    }
    t_now = ms();
    setjmp(s_loop);
    for (int i = 0; i < 40; i++) if (P[i].used) drop(&P[i]);
    fwlib_cancel();
    boot();
    http_init();
    tls_init();
    for (uint32_t last = ms();;) {
        t_now = ms();
        if ((int)(t_now - last) > s_max_gap) s_max_gap = (int)(t_now - last);
        last = t_now;
        http_poll();
        tls_poll();
        for (int i = 0; i < 40; i++) if (P[i].used) serve(&P[i]);
        usleep(50);
    }
    (void)argc;
}
