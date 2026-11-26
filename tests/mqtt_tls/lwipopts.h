/* lwIP for tests/test_mqtt_tls.c: the MQTT client and altcp, on the host. No IP or TCP
 * of its own: the TCP connection under TLS is a socket (the test's altcp). */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_RAW                    0
#define LWIP_UDP                    0
#define LWIP_ARP                    0
#define LWIP_ICMP                   0
#define LWIP_IGMP                   0
#define LWIP_DNS                    0
#define LWIP_DHCP                   0
#define IP_REASSEMBLY               0
#define IP_FRAG                     0
#define LWIP_TCP                    1
#define TCP_QUEUE_OOSEQ             0
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (64 * 1024)
#define MEMP_NUM_ALTCP_PCB          8
#define MEMP_NUM_TCP_PCB            2
#define MEMP_NUM_TCP_SEG            8
#define PBUF_POOL_SIZE              2
/* as the gateway's (src/net/lwipopts.h) */
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (6 * TCP_MSS)
#define TCP_SND_QUEUELEN            24
#define LWIP_TCP_KEEPALIVE          1
#define MQTT_OUTPUT_RINGBUF_SIZE    4096
#define MQTT_VAR_HEADER_BUFFER_LEN  256
#define MQTT_REQ_MAX_IN_FLIGHT      8
#define MQTT_CONNECT_TIMOUT         15
#define LWIP_STATS                  0
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1   /* the test uses the system's, for sockets */
#endif
