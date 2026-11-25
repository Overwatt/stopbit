/*
 * acl.h — Who may use a port's network side: addresses or subnets, each allowed to
 * read, or to read and write. An empty list allows anyone. Pure C; tests/test_acl.c.
 */
#ifndef ACL_H
#define ACL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ACL_MAX  8
#define ACL_TEXT (ACL_MAX * 28)         /* longest text form, with its NUL */

typedef struct {
    uint32_t ip;                        /* network byte order, host bits clear */
    uint8_t  bits, write, pad[2];
} acl_ent_t;
typedef struct {
    uint32_t  n;
    acl_ent_t e[ACL_MAX];               /* in the order entered; unused ones zero */
} acl_t;

typedef enum { ACL_DENY, ACL_READ, ACL_WRITE } acl_role_t;

/* The most specific entry decides. */
acl_role_t acl_role(const acl_t *a, uint32_t ip);
/* "10.0.0.5 read, 10.0.0.0/24 write"; empty clears. On failure *a is unchanged. */
bool acl_parse(acl_t *a, const char *text);
void acl_text(const acl_t *a, char out[ACL_TEXT]);
bool acl_valid(const acl_t *a);         /* as read back from flash */
/* Lets nobody in: what a damaged list becomes. */
void acl_deny_all(acl_t *a);
#endif
