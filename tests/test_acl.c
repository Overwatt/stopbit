/* cc -Isrc/app tests/test_acl.c src/app/{acl,cfg}.c -o /tmp/a && /tmp/a */
#include "acl.h"
#include "cfg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ip(const char *s) { uint32_t v; assert(ip4_parse(s, &v)); return v; }
static acl_t A;
static void ok(const char *s, const char *want)
{
    char t[ACL_TEXT];
    if (!acl_parse(&A, s)) { printf("should accept: %s\n", s); exit(1); }
    acl_text(&A, t);
    if (strcmp(t, want)) { printf("%s -> %s\n", s, t); exit(1); }
    assert(acl_valid(&A));
    acl_t b;
    assert(acl_parse(&b, t) && !memcmp(&A, &b, sizeof b));      /* text round-trips */
}
static void bad(const char *s)
{
    acl_t was = A;
    if (acl_parse(&A, s)) { printf("should reject: %s\n", s); exit(1); }
    assert(!memcmp(&was, &A, sizeof A));
}

int main(void)
{
    /* empty: anyone */
    ok("", "");
    ok("   ", "");
    assert(acl_role(&A, ip("1.2.3.4")) == ACL_WRITE && acl_role(&A, 0) == ACL_WRITE);

    ok("10.20.1.54 read", "10.20.1.54 read");
    assert(acl_role(&A, ip("10.20.1.54")) == ACL_READ);
    assert(acl_role(&A, ip("10.20.1.55")) == ACL_DENY && acl_role(&A, ip("54.1.75.10")) == ACL_DENY && acl_role(&A, 0) == ACL_DENY);

    /* the most specific decides, in any order */
    ok("10.20.0.0/23 read,10.20.1.9 write", "10.20.0.0/23 read, 10.20.1.9 write");
    assert(acl_role(&A, ip("10.20.1.9")) == ACL_WRITE && acl_role(&A, ip("10.20.1.8")) == ACL_READ);
    assert(acl_role(&A, ip("10.20.0.1")) == ACL_READ && acl_role(&A, ip("10.20.2.0")) == ACL_DENY && acl_role(&A, ip("10.74.255.255")) == ACL_DENY);
    ok("10.20.1.9 read, 0.0.0.0/0 write", "10.20.1.9 read, 0.0.0.0/0 write");
    assert(acl_role(&A, ip("10.20.1.9")) == ACL_READ && acl_role(&A, ip("8.8.8.8")) == ACL_WRITE);

    /* host bits are dropped, so what is shown is what is matched */
    ok("10.20.1.54/24 write", "10.20.1.0/24 write");
    ok("10.20.1.54/32 write", "10.20.1.54 write");
    ok("200.7.7.7/1 read", "128.0.0.0/1 read");
    assert(acl_role(&A, ip("255.0.0.1")) == ACL_READ && acl_role(&A, ip("127.0.0.1")) == ACL_DENY);
    ok("  1.2.3.4   read  ,  1.2.3.0/30  write ", "1.2.3.4 read, 1.2.3.0/30 write");

    static const char *B[] = { "10.0.0.1", "read", "10.0.0.1 admin", "10.0.0.1 READ", "10.0.0.1 read,", ", 10.0.0.1 read",
        "10.0.0.1 read 10.0.0.2 read", "10.0.0.1 read,, 10.0.0.2 read", "10.0.0.256 read", "10.0.0 read", "10.0.0.1/33 read",
        "10.0.0.1/ read", "10.0.0.1/-1 read", "10.0.0.1/024 read", "10.0.0.1/08 read", "10.0.0.1/24/8 read", "10.0.0.1/2x read", "host.local read",
        "10.0.0.1 read, 10.0.0.1 write", "10.0.0.5/24 read, 10.0.0.9/24 write", "::1 read", "10.0.0.1read", "10.0.0.1 reads",
        "10.0.0.1\tread", "10.0.0.1 read;10.0.0.2 read", "1111111111111111111111111111111111 read" };
    for (size_t i = 0; i < sizeof B / sizeof *B; i++) bad(B[i]);

    char t[1024] = "";
    for (int i = 0; i < ACL_MAX; i++) snprintf(t + strlen(t), 64, "%s255.255.255.%d write", i ? ", " : "", 240 + i);
    ok(t, t);
    strcat(t, ", 1.1.1.1 read");
    bad(t);
    /* the longest text there can be fits */
    t[0] = 0;
    for (int i = 0; i < ACL_MAX; i++) snprintf(t + strlen(t), 64, "%s255.255.255.%d/31 write", i ? ", " : "", 240 + 2 * i % 16);
    ok(t, t);
    assert(strlen(t) + 1 <= ACL_TEXT);

    acl_deny_all(&A);
    assert(acl_valid(&A) && acl_role(&A, ip("10.20.1.54")) == ACL_DENY && acl_role(&A, 0) == ACL_DENY);

    /* every role against a plain model, and junk never crashes or half-applies */
    srand(3);
    for (int k = 0; k < 20000; k++) {
        acl_t a; memset(&a, 0, sizeof a);
        char s[512] = "";
        int n = rand() % (ACL_MAX + 1);
        struct { uint32_t a; int b, w; } M[ACL_MAX];
        int m = 0;
        for (int i = 0; i < n; i++) {
            uint32_t v = 0x0A140000u | (uint32_t)(rand() & 0x3FF);
            int b = 22 + rand() % 11, w = rand() & 1, dup = 0;
            v &= 0xFFFFFFFFu << (32 - b);
            for (int j = 0; j < m; j++) dup |= M[j].a == v && M[j].b == b;
            if (dup) continue;
            M[m].a = v; M[m].b = b; M[m].w = w; m++;
            snprintf(s + strlen(s), 64, "%s%u.%u.%u.%u/%d %s", m > 1 ? "," : "", v >> 24, v >> 16 & 255, v >> 8 & 255, v & 255, b, w ? "write" : "read");
        }
        assert(acl_parse(&a, s));
        for (int q = 0; q < 8; q++) {
            uint32_t v = 0x0A140000u | (uint32_t)(rand() & 0x7FF);
            int best = -1, r = m ? 0 : 2;
            for (int j = 0; j < m; j++) if ((v & (0xFFFFFFFFu << (32 - M[j].b))) == M[j].a && M[j].b > best) { best = M[j].b; r = M[j].w ? 2 : 1; }
            char d[16]; snprintf(d, sizeof d, "%u.%u.%u.%u", v >> 24, v >> 16 & 255, v >> 8 & 255, v & 255);
            assert((int)acl_role(&a, ip(d)) == r);
        }
    }
    for (int k = 0; k < 200000; k++) {
        static const char C[] = "0123456789./, readwite";
        char s[64]; int l = rand() % 63;
        for (int i = 0; i < l; i++) s[i] = C[(size_t)rand() % (sizeof C - 1)];
        s[l] = 0;
        acl_t a = A, was = A;
        if (acl_parse(&a, s)) assert(acl_valid(&a)); else assert(!memcmp(&a, &was, sizeof a));
    }
    puts("ok");
    return 0;
}
