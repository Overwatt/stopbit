/* tcp.h — A fake lwIP for tests/test_bridge.c: calls are recorded, callbacks driven by hand. */
#ifndef STUB_NET_H
#define STUB_NET_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int8_t err_t; typedef uint16_t u16_t; typedef uint8_t u8_t; typedef uint32_t u32_t;
typedef struct { uint32_t addr; } ip_addr_t;
typedef ip_addr_t ip4_addr_t;
enum { ERR_OK = 0, ERR_MEM = -1, ERR_RTE = -4, ERR_INPROGRESS = -5, ERR_VAL = -6, ERR_ABRT = -13 };
#define TCP_MSS 1460
#define TCP_WND (4 * TCP_MSS)
#define TCP_WRITE_FLAG_COPY 1
#define TCP_WRITE_FLAG_MORE 2
#define tcp_sndqueuelen(p) ((p)->snd_queuelen)
#define SOF_REUSEADDR 4
#define SOF_KEEPALIVE 8
#define IP_ADDR_ANY NULL
#define IPADDR4_INIT(v) { v }
#define PBUF_TRANSPORT 0
#define PBUF_RAM 0
#define ip_2_ip4(p) (p)
#define ip4_addr_get_u32(p) ((p)->addr)
#define ip_addr_eq(a, b) ((a)->addr == (b)->addr)
#define ip_set_option(p, o) ((void)(p))
#define tcp_nagle_disable(p) ((void)(p))
struct pbuf { struct pbuf *next; void *payload; u16_t tot_len, len; };
struct tcp_pcb;
typedef err_t (*acc_fn)(void *, struct tcp_pcb *, err_t);
typedef err_t (*rcv_fn)(void *, struct tcp_pcb *, struct pbuf *, err_t);
typedef void (*err_fn)(void *, err_t);
struct tcp_pcb {
    ip_addr_t remote_ip; u16_t remote_port, port; uint32_t local; uint32_t keep_idle, keep_intvl, keep_cnt;
    bool used, listening, aborted, closed, connecting; void *arg; acc_fn acc; rcv_fn rx; err_fn err; err_t (*conn)(void *, struct tcp_pcb *, err_t);
    int owed; uint8_t out[65536]; int out_n;       /* owed: received and not yet acknowledged by the bridge */
    void *unacked; u16_t snd_queuelen; bool full; u8_t flags; int outputs;   /* lwIP's own; what the last write and outputs were */
};
struct udp_pcb;
typedef void (*udp_fn)(void *, struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t);
struct udp_pcb { bool used; udp_fn rx; void *arg; uint32_t local; };
struct tcp_pcb *tcp_new(void);
err_t tcp_bind(struct tcp_pcb *, const void *, u16_t);
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *, u8_t);
void tcp_accept(struct tcp_pcb *, acc_fn);
void tcp_arg(struct tcp_pcb *, void *);
void tcp_recv(struct tcp_pcb *, rcv_fn);
void tcp_err(struct tcp_pcb *, err_fn);
u16_t tcp_sndbuf(struct tcp_pcb *);
err_t tcp_write(struct tcp_pcb *, const void *, u16_t, u8_t);
err_t tcp_output(struct tcp_pcb *);
void tcp_recved(struct tcp_pcb *, u16_t);
err_t tcp_close(struct tcp_pcb *);
void tcp_abort(struct tcp_pcb *);
err_t tcp_connect(struct tcp_pcb *, const ip_addr_t *, u16_t, err_t (*)(void *, struct tcp_pcb *, err_t));
struct pbuf *pbuf_alloc(int, u16_t, int);
void pbuf_take(struct pbuf *, const void *, u16_t);
void pbuf_free(struct pbuf *);
struct udp_pcb *udp_new(void);
err_t udp_bind(struct udp_pcb *, const void *, u16_t);
void udp_remove(struct udp_pcb *);
void udp_recv(struct udp_pcb *, udp_fn, void *);
err_t udp_sendto(struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t);
struct netif;
struct netif *ip_route(const ip_addr_t *src, const ip_addr_t *dst);
err_t udp_sendto_if_src(struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t, struct netif *, const ip_addr_t *);
err_t dns_gethostbyname(const char *, ip_addr_t *, void (*)(const char *, const ip_addr_t *, void *), void *);
char *ipaddr_ntoa_r(const ip_addr_t *, char *, int);
#endif
