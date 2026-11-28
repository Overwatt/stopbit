/* lwIP for tests/ipalias: IPv4, ARP, ACD, TCP and UDP on the host, frames in and out by the
 * test. Checksums are not checked on input, so the test's frames need none. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_RAW                    0
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_ACD                    1
#define LWIP_DNS                    0
#define LWIP_IGMP                   0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (32 * 1024)
#define CHECKSUM_CHECK_IP           0
#define CHECKSUM_CHECK_TCP          0
#define CHECKSUM_CHECK_UDP          0
#define LWIP_STATS                  0
#endif
