/* lwIP 2.2 configuration: bare-metal, raw API, single-threaded. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_RAW                    0

#define MEM_ALIGNMENT               4
#ifdef APP_IMAGE
#define MEM_SIZE                    (32 * 1024)
#else
#define MEM_SIZE                    (24 * 1024)
#endif
#ifdef APP_IMAGE                    /* 73 KB that need not be in the fastest memory */
#define LWIP_DECLARE_MEMORY_ALIGNED(variable_name, size) \
    __attribute__((section(".d2_bss"), aligned(4))) u8_t variable_name[LWIP_MEM_ALIGN_BUFFER(size)]
#endif
#define MEMP_NUM_PBUF               16
#ifdef APP_IMAGE                    /* 4 ports x 4 clients + web */
#define MEMP_NUM_TCP_PCB            32
#define MEMP_NUM_TCP_PCB_LISTEN     8
#define MEMP_NUM_TCP_SEG            96
#define MEMP_NUM_UDP_PCB            10
#else
#define MEMP_NUM_TCP_PCB            12
#define MEMP_NUM_TCP_PCB_LISTEN     6
#define MEMP_NUM_TCP_SEG            32
#endif
#define MEMP_NUM_SYS_TIMEOUT        22
#define SO_REUSE                    1
#define PBUF_POOL_SIZE              20
#define PBUF_POOL_BUFSIZE           1536

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 1
#define LWIP_DHCP_AUTOIP_COOP       1
#define LWIP_DHCP_AUTOIP_COOP_TRIES 4
#define LWIP_ACD                    1
/* Skip the ~6 s conflict probe: this box should be back quickly after an update. */
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_IGMP                   1
#define LWIP_UDP                    1
#ifdef APP_IMAGE
#define LWIP_DNS                    1
#else
#define LWIP_DNS                    0
#endif
#define LWIP_NETIF_HOSTNAME         1
#ifdef APP_IMAGE                    /* the time, for checking certificates and signing to AWS */
#define LWIP_DHCP_GET_NTP_SRV       1
#define LWIP_DHCP_MAX_NTP_SERVERS   2
#define SNTP_SERVER_DNS             1
#define SNTP_SET_SYSTEM_TIME(sec)   clock_set((uint32_t)(sec))
#include <stdint.h>
void clock_set(uint32_t unix_s);
#endif
#define LWIP_DNS_SUPPORT_MDNS_QUERIES 1      /* so homeassistant.local works */

#define MQTT_OUTPUT_RINGBUF_SIZE    4096
#define MQTT_VAR_HEADER_BUFFER_LEN  256
#define MQTT_REQ_MAX_IN_FLIGHT      8
#define MQTT_CONNECT_TIMOUT         15
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1

/* n540-xxxxxx.local */
#define LWIP_MDNS_RESPONDER         1
#define MDNS_MAX_SERVICES           2
#define LWIP_NUM_NETIF_CLIENT_DATA  1

#define LWIP_TCP                    1
#define TCP_MSS                     1460
#ifdef APP_IMAGE                    /* bridge keeps one ring this big per connection */
#define TCP_WND                     (4 * TCP_MSS)
#else
#define TCP_WND                     (8 * TCP_MSS)
#endif
#define TCP_SND_BUF                 (6 * TCP_MSS)
#define TCP_SND_QUEUELEN            24
#define TCP_LISTEN_BACKLOG          1
#define LWIP_TCP_KEEPALIVE          1

#define LWIP_STATS                  0
#define LWIP_DEBUG                  0
#endif
