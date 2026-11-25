#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
#include <stdint.h>
#include <stdlib.h>
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_UNISTD_H 1
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
void lwip_assert_fail(const char *msg);
#define LWIP_PLATFORM_ASSERT(x) lwip_assert_fail(x)
uint32_t net_rand(void);
#define LWIP_RAND() net_rand()
#endif
