/* tcp.h — Enough of lwIP's raw TCP API, over ordinary sockets, to run http.c. */
#ifndef SIM_TCP_H
#define SIM_TCP_H
#include <stdint.h>
#include <stddef.h>
typedef int8_t err_t;
typedef uint16_t u16_t;
typedef uint8_t u8_t;
enum { ERR_OK = 0, ERR_MEM = -1, ERR_VAL = -6, ERR_ABRT = -13, ERR_RST = -14 };
#define TCP_MSS 1460
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (6 * TCP_MSS)
#define TCP_SND_QUEUELEN 24
#define TCP_WRITE_FLAG_COPY 1
#define IP_ADDR_ANY NULL
#define SOF_REUSEADDR 4
struct tcp_pcb;
void ip_set_option(struct tcp_pcb *, int);
struct pbuf { struct pbuf *next; void *payload; u16_t tot_len, len; int ref; };
struct tcp_pcb;
typedef err_t (*tcp_accept_fn)(void *, struct tcp_pcb *, err_t);
typedef err_t (*tcp_recv_fn)(void *, struct tcp_pcb *, struct pbuf *, err_t);
typedef err_t (*tcp_sent_fn)(void *, struct tcp_pcb *, u16_t);
typedef err_t (*tcp_poll_fn)(void *, struct tcp_pcb *);
typedef void  (*tcp_err_fn)(void *, err_t);
typedef struct { uint32_t addr; } ip_addr_t;
enum { ERR_INPROGRESS = -5 };
err_t tcp_connect(struct tcp_pcb *, const ip_addr_t *, u16_t, err_t (*)(void *, struct tcp_pcb *, err_t));
err_t dns_gethostbyname(const char *, ip_addr_t *, void (*)(const char *, const ip_addr_t *, void *), void *);
void pbuf_free(struct pbuf *p);
void pbuf_ref(struct pbuf *p);
void pbuf_cat(struct pbuf *h, struct pbuf *t);
struct tcp_pcb *tcp_new(void);
err_t tcp_bind(struct tcp_pcb *, const void *, u16_t);
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *, u8_t);
void tcp_accept(struct tcp_pcb *, tcp_accept_fn);
void tcp_arg(struct tcp_pcb *, void *);
void tcp_recv(struct tcp_pcb *, tcp_recv_fn);
void tcp_sent(struct tcp_pcb *, tcp_sent_fn);
void tcp_err(struct tcp_pcb *, tcp_err_fn);
void tcp_poll(struct tcp_pcb *, tcp_poll_fn, u8_t);
void tcp_nagle_disable(struct tcp_pcb *);
u16_t tcp_sndbuf(struct tcp_pcb *);
u16_t tcp_sndqueuelen(struct tcp_pcb *);
err_t tcp_write(struct tcp_pcb *, const void *, u16_t, u8_t);
err_t tcp_output(struct tcp_pcb *);
void tcp_recved(struct tcp_pcb *, u16_t);
err_t tcp_close(struct tcp_pcb *);
void tcp_abort(struct tcp_pcb *);
#endif
