/*
 * driver.c — A small coverage-guided fuzzer. Apple's clang ships without libFuzzer, so
 * this stands in: a target defines LLVMFuzzerTestOneInput, is built with
 * -fsanitize-coverage=trace-pc-guard and the address and undefined-behaviour checks, and
 * is linked with this file (built without coverage). See run.sh.
 *
 *   fuzz_x CORPUS SECONDS [DICT] [MAXLEN]
 *   fuzz_x -r INPUT          runs one input in this process, to see a failure
 *
 * Each input runs in a fresh child process, so targets with state start clean and a crash
 * or a hang does not end the run. Inputs that reach new code are kept in CORPUS; one that
 * crashes or hangs is kept beside it as crash-* or hang-*, with what it printed.
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

/* ---- coverage: one byte per edge, shared with the child ---- */
#define MAXGUARDS (1u << 20)
static uint32_t s_ng;
static uint8_t *s_hit;                  /* this run, written by the child */
static uint8_t s_seen[MAXGUARDS];       /* all runs */

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop)
{
    if (start == stop || *start) return;
    if (!s_hit) s_hit = mmap(NULL, MAXGUARDS, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    for (uint32_t *g = start; g < stop && s_ng + 1 < MAXGUARDS; g++) *g = ++s_ng;
}
void __sanitizer_cov_trace_pc_guard(uint32_t *g) { s_hit[*g] = 1; }

/* ---- corpus ---- */
typedef struct { uint8_t *d; size_t n; } in_t;
static in_t *s_c;
static size_t s_nc, s_capc, s_max = 4096;
static char *s_dict[512];
static size_t s_dlen[512], s_nd;
static const char *s_dir;
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(uint32_t n)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return n ? (uint32_t)(s_rng % n) : 0;
}

static uint64_t fnv(const uint8_t *d, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 1099511628211ull;
    return h;
}

static void add(const uint8_t *d, size_t n, bool save)
{
    if (s_nc == s_capc) s_c = realloc(s_c, (s_capc = s_capc ? 2 * s_capc : 256) * sizeof *s_c);
    s_c[s_nc].d = malloc(n ? n : 1);
    memcpy(s_c[s_nc].d, d, n);
    s_c[s_nc++].n = n;
    if (!save) return;
    char p[1024];
    snprintf(p, sizeof p, "%s/%016llx", s_dir, (unsigned long long)fnv(d, n));
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}

static void load(const char *dir)
{
    DIR *D = opendir(dir);
    struct dirent *e;
    static uint8_t b[1 << 20];
    while (D && (e = readdir(D))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        size_t n = fread(b, 1, sizeof b, f);
        fclose(f);
        add(b, n > s_max ? s_max : n, false);
    }
    if (D) closedir(D);
    if (!s_nc) add((const uint8_t *)"", 0, false);
}

static void dict(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    while (f && s_nd < 512 && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = 0;
        if (!n || line[0] == '#') continue;
        /* \r \n \t \0 \\ and \xNN, so tokens can hold line breaks */
        char *o = line, *i = line;
        while (*i) {
            if (*i != '\\' || !i[1]) { *o++ = *i++; continue; }
            i++;
            if (*i == 'r') *o++ = '\r'; else if (*i == 'n') *o++ = '\n'; else if (*i == 't') *o++ = '\t';
            else if (*i == '0') *o++ = 0;
            else if (*i == 'x' && i[1] && i[2]) { char h[3] = { i[1], i[2], 0 }; *o++ = (char)strtol(h, NULL, 16); i += 2; }
            else *o++ = *i;
            i++;
        }
        s_dlen[s_nd] = (size_t)(o - line);
        s_dict[s_nd] = malloc(s_dlen[s_nd]);
        memcpy(s_dict[s_nd], line, s_dlen[s_nd]);
        s_nd++;
    }
    if (f) fclose(f);
}

/* ---- mutation ---- */
static const char *const NUMS[] = { "0", "1", "-1", "255", "256", "65535", "65536", "2147483647",
    "2147483648", "4294967295", "4294967296", "18446744073709551615", "99999999999999999999",
    "0x7fffffff", "-2147483648", "1e9", "08", "+1", " 1", "00000000000000000001" };

static void ins(uint8_t *b, size_t *n, size_t at, const void *d, size_t k)
{
    if (*n + k > s_max) k = s_max - *n;
    if (!k) return;
    memmove(b + at + k, b + at, *n - at);
    memcpy(b + at, d, k);
    *n += k;
}

static size_t mutate(uint8_t *b, size_t n)
{
    for (int k = 1 + (int)rnd(5); k--;) {
        size_t at = n ? rnd((uint32_t)n) : 0, len;
        switch (rnd(13)) {
        case 0: if (n) b[at] ^= (uint8_t)(1u << rnd(8)); break;
        case 1: if (n) b[at] = (uint8_t)rnd(256); break;
        case 2: { static const uint8_t I[] = { 0, 1, 0x7f, 0x80, 0xff, '\r', '\n', ' ', ':', '/', '%', '"', '\\', '=', ',', '.' };
                  if (n) b[at] = I[rnd(sizeof I)]; break; }
        case 3: { uint8_t r[8]; len = 1 + rnd(8); for (size_t i = 0; i < len; i++) r[i] = (uint8_t)rnd(256); ins(b, &n, at, r, len); break; }
        case 4: if (n) { len = 1 + rnd((uint32_t)(n - at)); memmove(b + at, b + at + len, n - at - len); n -= len; } break;
        case 5: if (n) { size_t from = rnd((uint32_t)n); len = 1 + rnd((uint32_t)(n - from > 64 ? 64 : n - from)); uint8_t t[64]; memcpy(t, b + from, len); ins(b, &n, rnd((uint32_t)n + 1), t, len); } break;
        case 6: if (s_nd) { size_t i = rnd((uint32_t)s_nd); len = s_dlen[i]; if (at + len <= n) memcpy(b + at, s_dict[i], len); } break;
        case 7: case 8: if (s_nd) { size_t i = rnd((uint32_t)s_nd); ins(b, &n, n ? rnd((uint32_t)n + 1) : 0, s_dict[i], s_dlen[i]); } break;
        case 9: { const in_t *o = &s_c[rnd((uint32_t)s_nc)]; if (o->n) { size_t from = rnd((uint32_t)o->n); n = at; ins(b, &n, n, o->d + from, o->n - from); } break; }
        case 10: { size_t i = at; while (i < n && (b[i] < '0' || b[i] > '9')) i++;          /* a number, replaced */
                   if (i == n) break; size_t j = i; while (j < n && b[j] >= '0' && b[j] <= '9') j++;
                   memmove(b + i, b + j, n - j); n -= j - i; const char *s = NUMS[rnd(sizeof NUMS / sizeof NUMS[0])]; ins(b, &n, i, s, strlen(s)); break; }
        case 11: if (n) { uint8_t t[16]; len = 1 + rnd(16); if (at + len > n) len = n - at; memcpy(t, b + at, len);   /* a piece, many times */
                   for (uint32_t r = 1 + rnd(300); r-- && n + len <= s_max;) ins(b, &n, at, t, len); } break;
        case 12: if (n) n = rnd((uint32_t)n); break;
        }
    }
    return n;
}

/* ---- running one input in a child ---- */
static uint8_t *s_cur;
static size_t s_curn;

static int run(const uint8_t *d, size_t n, int *sig)
{
    memset(s_hit, 0, s_ng + 1);
    char log[1024];
    snprintf(log, sizeof log, "%s/../last.log", s_dir);
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 2); dup2(fd, 1); close(fd); }
        alarm(3);                                   /* a hang ends here, as SIGALRM */
        LLVMFuzzerTestOneInput(d, n);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    *sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void keep_failure(const char *kind, const uint8_t *d, size_t n)
{
    char p[1024], q[1024], log[1024];
    uint64_t h = fnv(d, n);
    snprintf(p, sizeof p, "%s/../%s-%016llx", s_dir, kind, (unsigned long long)h);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
    snprintf(log, sizeof log, "%s/../last.log", s_dir);
    snprintf(q, sizeof q, "%s.log", p);
    rename(log, q);
    fprintf(stderr, "\n%s: %s\n", kind, p);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "-r")) {         /* replay one input, here, to see what it does */
        static uint8_t b[1 << 20];
        FILE *f = fopen(argv[2], "rb");
        if (!f) return 2;
        size_t n = fread(b, 1, sizeof b, f);
        fclose(f);
        if (!s_hit) s_hit = calloc(1, MAXGUARDS);
        LLVMFuzzerTestOneInput(b, n);
        fprintf(stderr, "replayed %zu bytes: no failure\n", n);
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "fuzz CORPUS SECONDS [DICT] [MAXLEN]\n       fuzz -r INPUT\n"); return 2; }
    s_dir = argv[1];
    long secs = atol(argv[2]);
    if (argc > 3 && argv[3][0]) dict(argv[3]);
    if (argc > 4) s_max = (size_t)atol(argv[4]);
    s_rng ^= (uint64_t)time(NULL) * 2654435761u ^ (uint64_t)getpid();
    mkdir(s_dir, 0755);
    load(s_dir);
    s_cur = malloc(s_max + 64);

    /* what the corpus already reaches */
    for (size_t i = 0; i < s_nc; i++) {
        int sig, rc = run(s_c[i].d, s_c[i].n, &sig);
        if (sig || rc) keep_failure(sig == SIGALRM ? "hang" : "crash", s_c[i].d, s_c[i].n);
        for (uint32_t g = 1; g <= s_ng; g++) s_seen[g] |= s_hit[g];
    }
    time_t start = time(NULL), shown = 0;
    unsigned long long execs = 0, fails = 0;
    uint32_t cov = 0;
    for (uint32_t g = 1; g <= s_ng; g++) cov += s_seen[g];
    while (time(NULL) - start < secs) {
        const in_t *base = &s_c[rnd((uint32_t)s_nc)];
        memcpy(s_cur, base->d, base->n);
        s_curn = mutate(s_cur, base->n);
        int sig, rc = run(s_cur, s_curn, &sig);
        execs++;
        if (sig || rc) {
            fails++;
            keep_failure(sig == SIGALRM ? "hang" : "crash", s_cur, s_curn);
            continue;
        }
        uint32_t fresh = 0;
        for (uint32_t g = 1; g <= s_ng; g++)
            if (s_hit[g] && !s_seen[g]) { s_seen[g] = 1; fresh++; }
        if (fresh) { cov += fresh; add(s_cur, s_curn, true); }
        if (time(NULL) - shown >= 30) {
            shown = time(NULL);
            fprintf(stderr, "%lds: %llu runs (%llu/s), %u of %u edges, %zu inputs, %llu failures\n",
                    (long)(shown - start), execs, execs / (unsigned long long)(shown - start + 1), cov, s_ng, s_nc, fails);
        }
    }
    fprintf(stderr, "done: %llu runs, %u of %u edges, %zu inputs, %llu failures\n", execs, cov, s_ng, s_nc, fails);
    return fails ? 1 : 0;
}
