#include "acme.h"
#include "hclient.h"
#include "tls.h"
#include "clock.h"
#include "worker.h"
#include "extflash.h"
#include "sha256.h"
#include "stm32h7xx_hal.h"
#include "psa/crypto.h"
#include "mbedtls/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LE_HOST      "acme-v02.api.letsencrypt.org"
#define LE_STAGING   "acme-staging-v02.api.letsencrypt.org"
#define AWS_HOST     "route53.amazonaws.com"
#define BUF          8192u
#define DAY          86400u
#define CHECK_EVERY  (12u * 3600u)

/* ---- what is stored: two copies, the magic last, like tls.c ------------------ */
#define DEV   1
#define MAGIC 0x454D4341u
static const uint32_t ADDR[2] = { 0x0F8000u, 0x0F9000u };
typedef struct {
    uint8_t  automatic, has_account, has_secret, pad;
    char     zone[32], key_id[32], secret[64];
    uint8_t  account[32], account_pub[65], pad2[3];
    uint32_t last_ok;                    /* unix seconds of the last renewal */
} body_t;
typedef struct { uint32_t magic, seq; uint8_t sha[32]; body_t b; } rec_t;
static rec_t s_rec;
static int s_cur = -1;

void acme_mask(int dev, uint32_t addr, uint8_t *buf, uint32_t n)
{
    if (dev != DEV) return;
    for (uint32_t i = 0; i < n; i++)
        if (addr + i >= ADDR[0] && addr + i < ADDR[1] + 0x1000u) buf[i] = 0xFF;
}

static void digest(const rec_t *r, uint8_t out[32])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, &r->seq, 4);
    sha256_update(&s, &r->b, sizeof r->b);
    sha256_final(&s, out);
}

static bool load_one(int i, rec_t *r)
{
    uint8_t d[32];
    extflash_read(DEV, ADDR[i], r, sizeof *r);
    if (r->magic != MAGIC) return false;
    digest(r, d);
    return ct_equal(d, r->sha, 32);
}

static bool save(const body_t *b)
{
    static rec_t w, chk;
    int i = s_cur == 0 ? 1 : 0;
    w.magic = MAGIC;
    w.seq = s_rec.seq + 1;
    w.b = *b;
    digest(&w, w.sha);
    if (!extflash_erase(DEV, ADDR[i], 4096) || !extflash_write(DEV, ADDR[i] + 4, &w.seq, sizeof w - 4) ||
        !extflash_write(DEV, ADDR[i], &w.magic, 4) || !load_one(i, &chk) || memcmp(&chk, &w, sizeof w)) return false;
    s_rec = w;
    s_cur = i;
    return true;
}

/* ---- state ---------------------------------------------------------------------- */
static bool s_running, s_staging;
static const char *s_step = "";
static char s_result[120];
static uint32_t s_result_at, s_next_check, s_fails;

void acme_init(void)
{
    static rec_t r;
    for (int i = 0; i < 2; i++)
        if (load_one(i, &r) && (s_cur < 0 || r.seq > s_rec.seq)) { s_rec = r; s_cur = i; }
    if (s_cur < 0) memset(&s_rec, 0, sizeof s_rec);
}

/* ---- small things ------------------------------------------------------------------ */
static size_t b64url(const uint8_t *in, size_t n, char *out)
{
    static const char D[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = D[v >> 18];
        out[o++] = D[v >> 12 & 63];
        if (i + 1 < n) out[o++] = D[v >> 6 & 63];
        if (i + 2 < n) out[o++] = D[v & 63];
    }
    out[o] = 0;
    return o;
}

static void sha256(const void *d, size_t n, uint8_t out[32])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, d, n);
    sha256_final(&s, out);
}

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* HMAC-SHA256 with a key of any length. */
static void hmac(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64] = { 0 }, pad[64], inner[32];
    sha256_t s;
    if (klen > 64) sha256(key, klen, k);
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, msg, mlen);
    sha256_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}

/* The string value of "key" in JSON, searching from `from`; also *at, where it was. */
static bool jstr(const char *from, const char *key, char *out, size_t max, const char **at)
{
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(from, k);
    if (!p) return false;
    p += strlen(k);
    while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p == '[') { p++; while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; }    /* the first of a list */
    if (*p != '"') return false;
    const char *e = strchr(++p, '"');
    if (!e || (size_t)(e - p) >= max) return false;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    if (at) *at = p;
    return true;
}

/* ---- the job --------------------------------------------------------------------- */
typedef struct {
    char *buf;                       /* responses */
    char *body;                      /* requests */
    psa_key_id_t key;
    char host[48], nonce[96], kid[160], jwk[200], thumb[48];
    char order[160], authz[160], chal[160], finalize[160], cert[160], token[96], txt[48], name[64];
    hc_resp_t r;
} job_t;

static const char *step(const char *s) { s_step = s; return NULL; }

static void wait_s(uint32_t s)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < s * 1000u) worker_yield();
}

static const char *path_of(const char *url, const char *host)
{
    const char *p = strstr(url, host);
    return p ? p + strlen(host) : NULL;
}

static const char *get_nonce(job_t *j)
{
    const char *why = hc_request("HEAD", j->host, "/acme/new-nonce", NULL, "", 0, j->buf, BUF, &j->r);
    if (why) return why;
    if (!j->r.nonce[0]) return "Let's Encrypt returned no nonce";
    snprintf(j->nonce, sizeof j->nonce, "%s", j->r.nonce);
    return NULL;
}

/* A signed request (JWS), with `payload` as JSON, or NULL to POST-as-GET. */
static const char *post(job_t *j, const char *url, const char *payload, bool use_jwk)
{
    static char prot[600], p64[800], pl64[1700], sig64[90];
    uint8_t h[32], sig[64];
    size_t n;
    const char *path = path_of(url, j->host), *why;
    if (!path) return "Let's Encrypt redirected to an unexpected address";
    for (int tries = 0; tries < 3; tries++) {
        if (!j->nonce[0] && (why = get_nonce(j))) return why;
        snprintf(prot, sizeof prot, "{\"alg\":\"ES256\",%s%s%s,\"nonce\":\"%s\",\"url\":\"%s\"}",
                 use_jwk ? "\"jwk\":" : "\"kid\":\"", use_jwk ? j->jwk : j->kid, use_jwk ? "" : "\"", j->nonce, url);
        j->nonce[0] = 0;
        b64url((const uint8_t *)prot, strlen(prot), p64);
        if (payload && strlen(payload) * 4 / 3 + 4 >= sizeof pl64) return "request too long";
        if (payload) b64url((const uint8_t *)payload, strlen(payload), pl64);
        else pl64[0] = 0;
        snprintf(j->body, BUF, "%s.%s", p64, pl64);
        sha256(j->body, strlen(j->body), h);
        if (psa_sign_hash(j->key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), h, 32, sig, sizeof sig, &n) != PSA_SUCCESS || n != 64)
            return "could not sign";
        b64url(sig, 64, sig64);
        n = (size_t)snprintf(j->body, BUF, "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}", p64, pl64, sig64);
        why = hc_request("POST", j->host, path, "Content-Type: application/jose+json\r\n", j->body, n, j->buf, BUF, &j->r);
        if (why) return why;
        if (j->r.nonce[0]) snprintf(j->nonce, sizeof j->nonce, "%s", j->r.nonce);
        if (j->r.status == 400 && strstr(j->r.body, "badNonce")) continue;
        if (j->r.status >= 400) {
            static char msg[110];
            char d[90] = "";
            jstr(j->r.body, "detail", d, sizeof d, NULL);
            snprintf(msg, sizeof msg, "Let's Encrypt said %d: %s", j->r.status, d[0] ? d : "no reason given");
            return msg;
        }
        return NULL;
    }
    return "Let's Encrypt repeatedly rejected the nonce";
}

/* ---- Route 53, signed with AWS Signature Version 4 -------------------------------- */
static const char *r53(job_t *j, const char *method, const char *path, const char *body, char *change_id)
{
    char date[17], day[9], scope[64], payhash[65], creq_hash[65], sigx[65], hdr[400];
    static char creq[700], sts[300];
    uint8_t k[32], h[32];
    const body_t *b = &s_rec.b;
    if (!clock_iso(date, false) || !clock_iso(day, true)) return "the time is not known";
    sha256(body, strlen(body), h);
    hex(h, 32, payhash);
    snprintf(creq, sizeof creq, "%s\n%s\n\nhost:%s\nx-amz-date:%s\n\nhost;x-amz-date\n%s", method, path, AWS_HOST, date, payhash);
    sha256(creq, strlen(creq), h);
    hex(h, 32, creq_hash);
    snprintf(scope, sizeof scope, "%s/us-east-1/route53/aws4_request", day);
    snprintf(sts, sizeof sts, "AWS4-HMAC-SHA256\n%s\n%s\n%s", date, scope, creq_hash);
    char kk[80];
    int kn = snprintf(kk, sizeof kk, "AWS4%s", b->secret);
    hmac((const uint8_t *)kk, (size_t)kn, day, 8, k);
    memset(kk, 0, sizeof kk);
    hmac(k, 32, "us-east-1", 9, k);
    hmac(k, 32, "route53", 7, k);
    hmac(k, 32, "aws4_request", 12, k);
    hmac(k, 32, sts, strlen(sts), h);
    memset(k, 0, sizeof k);
    hex(h, 32, sigx);
    snprintf(hdr, sizeof hdr, "X-Amz-Date: %s\r\nAuthorization: AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=host;x-amz-date, Signature=%s\r\n%s",
             date, b->key_id, scope, sigx, body[0] ? "Content-Type: text/xml\r\n" : "");
    const char *why = hc_request(method, AWS_HOST, path, hdr, body, strlen(body), j->buf, BUF, &j->r);
    if (why) return why;
    if (j->r.status != 200) {
        static char msg[110];
        char *m = strstr(j->r.body, "<Message>"), *e = m ? strstr(m, "</Message>") : NULL;
        snprintf(msg, sizeof msg, "Route 53 said %d: %.*s", j->r.status, m && e ? (int)(e - m - 9) : 16, m && e ? m + 9 : "no reason given");
        return msg;
    }
    if (change_id) {
        char *p = strstr(j->r.body, "<Id>/change/"), *e = p ? strstr(p, "</Id>") : NULL;
        if (!p || !e || e - p - 12 >= 40) return "Route 53 returned no change ID";
        memcpy(change_id, p + 12, (size_t)(e - p - 12));
        change_id[e - p - 12] = 0;
    }
    return NULL;
}

static const char *txt_record(job_t *j, bool add)
{
    char id[40], path[80];
    snprintf(path, sizeof path, "/2013-04-01/hostedzone/%s/rrset", s_rec.b.zone);
    snprintf(j->body, BUF,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ChangeResourceRecordSetsRequest xmlns=\"https://route53.amazonaws.com/doc/2013-04-01/\">"
             "<ChangeBatch><Changes><Change><Action>%s</Action><ResourceRecordSet><Name>_acme-challenge.%s.</Name><Type>TXT</Type><TTL>60</TTL>"
             "<ResourceRecords><ResourceRecord><Value>\"%s\"</Value></ResourceRecord></ResourceRecords></ResourceRecordSet></Change></Changes>"
             "</ChangeBatch></ChangeResourceRecordSetsRequest>", add ? "UPSERT" : "DELETE", j->name, j->txt);
    const char *why = r53(j, "POST", path, j->body, id);
    if (why || !add) return why;
    snprintf(path, sizeof path, "/2013-04-01/change/%s", id);
    for (int i = 0; i < 40; i++) {                        /* until every Route 53 server has it */
        if ((why = r53(j, "GET", path, "", NULL))) return why;
        if (strstr(j->r.body, "INSYNC")) return NULL;
        wait_s(3);
    }
    return "Route 53 did not respond in time";
}

/* ---- the steps -------------------------------------------------------------------------- */
static const char *account(job_t *j)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    uint8_t pub[65], h[32];
    char x[48], y[48];
    size_t n;
    if (!s_rec.b.has_account) {                            /* one account key, kept */
        static body_t b;
        b = s_rec.b;
        psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&a, 256);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_EXPORT);
        psa_key_id_t t = 0;
        bool ok = psa_generate_key(&a, &t) == PSA_SUCCESS && psa_export_key(t, b.account, 32, &n) == PSA_SUCCESS &&
                  psa_export_public_key(t, b.account_pub, 65, &n) == PSA_SUCCESS;
        psa_destroy_key(t);
        b.has_account = 1;
        if (!ok || !save(&b)) return "could not make or keep an account key";
    }
    psa_reset_key_attributes(&a);
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    if (psa_import_key(&a, s_rec.b.account, 32, &j->key) != PSA_SUCCESS) return "the account key is unreadable";
    memcpy(pub, s_rec.b.account_pub, 65);
    b64url(pub + 1, 32, x);
    b64url(pub + 33, 32, y);
    snprintf(j->jwk, sizeof j->jwk, "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}", x, y);
    sha256(j->jwk, strlen(j->jwk), h);
    b64url(h, 32, j->thumb);
    char url[80];
    snprintf(url, sizeof url, "https://%s/acme/new-acct", j->host);
    const char *why = post(j, url, "{\"termsOfServiceAgreed\":true}", true);
    if (why) return why;
    if (!j->r.location[0]) return "Let's Encrypt returned no account";
    snprintf(j->kid, sizeof j->kid, "%s", j->r.location);
    return NULL;
}

static const char *order(job_t *j)
{
    char url[80], pl[160], status[24] = "";
    const char *why, *at;
    snprintf(url, sizeof url, "https://%s/acme/new-order", j->host);
    snprintf(pl, sizeof pl, "{\"identifiers\":[{\"type\":\"dns\",\"value\":\"%s\"}]}", j->name);
    if ((why = post(j, url, pl, false))) return why;
    snprintf(j->order, sizeof j->order, "%s", j->r.location);
    if (!j->order[0] || !jstr(j->r.body, "authorizations", j->authz, sizeof j->authz, NULL) ||
        !jstr(j->r.body, "finalize", j->finalize, sizeof j->finalize, NULL)) return "Let's Encrypt's order could not be read";
    if ((why = post(j, j->authz, NULL, false))) return why;
    jstr(j->r.body, "status", status, sizeof status, NULL);
    if (!strcmp(status, "valid")) { j->chal[0] = 0; return NULL; }           /* proved recently */
    const char *d = strstr(j->r.body, "\"dns-01\"");
    if (!d) return "Let's Encrypt offered no DNS challenge";
    const char *o = d;
    while (o > j->r.body && *o != '{') o--;                                   /* the challenge's own object */
    if (!jstr(o, "url", j->chal, sizeof j->chal, &at) || !jstr(o, "token", j->token, sizeof j->token, NULL))
        return "Let's Encrypt's challenge could not be read";
    char ka[160];
    uint8_t h[32];
    snprintf(ka, sizeof ka, "%s.%s", j->token, j->thumb);
    sha256(ka, strlen(ka), h);
    b64url(h, 32, j->txt);
    return NULL;
}

static const char *poll_status(job_t *j, const char *url, const char *want, int tries)
{
    char status[24];
    const char *why;
    for (int i = 0; i < tries; i++) {
        if ((why = post(j, url, NULL, false))) return why;
        status[0] = 0;
        jstr(j->r.body, "status", status, sizeof status, NULL);
        if (!strcmp(status, want)) return NULL;
        if (!strcmp(status, "invalid")) {
            static char msg[110];
            char d[90] = "";
            jstr(j->r.body, "detail", d, sizeof d, NULL);
            snprintf(msg, sizeof msg, "Let's Encrypt could not check: %s", d[0] ? d : "no reason given");
            return msg;
        }
        wait_s(3);
    }
    return "Let's Encrypt did not respond in time";
}

/* PEM chain in j->buf to concatenated DER in place. */
static size_t pem_to_der(char *pem, size_t len)
{
    static const char B[] = "-----BEGIN CERTIFICATE-----", E[] = "-----END CERTIFICATE-----";
    uint8_t *out = (uint8_t *)pem;
    size_t o = 0;
    char *p = pem;
    while ((p = strstr(p, B))) {
        char *e = strstr(p, E);
        if (!e) return 0;
        uint32_t v = 0;
        int bits = 0;
        for (char *c = p + sizeof B - 1; c < e; c++) {
            int d = *c >= 'A' && *c <= 'Z' ? *c - 'A' : *c >= 'a' && *c <= 'z' ? *c - 'a' + 26 :
                    *c >= '0' && *c <= '9' ? *c - '0' + 52 : *c == '+' ? 62 : *c == '/' ? 63 : -1;
            if (d < 0) continue;
            v = v << 6 | (uint32_t)d;
            if ((bits += 6) >= 8) { bits -= 8; out[o++] = (uint8_t)(v >> bits); }
        }
        p = e + sizeof E - 1;
    }
    (void)len;
    return o;
}

static const char *tls_do(const char *cmd, size_t len)
{
    const char *why;
    for (int i = 0; i < 600 && tls_busy(); i++) worker_yield();
    if ((why = tls_command((const uint8_t *)cmd, len))) return why;
    for (int i = 0; i < 6000 && tls_busy(); i++) worker_yield();         /* a new key takes a moment */
    return NULL;
}

static const char *renew(job_t *j)
{
    const char *why;
    uint32_t a, b;
    const char *name = tls_issued(&a, &b);
    if (!name) name = tls_requested();
    if (!name) return "there is no host name: make a signing request first";
    snprintf(j->name, sizeof j->name, "%s", name);
    if (step("Account") || (why = account(j))) return why;
    if (step("Order") || (why = order(j))) return why;
    if (j->chal[0]) {
        if (step("Adding the DNS record") || (why = txt_record(j, true))) {
            txt_record(j, false);                                             /* it may have got there */
            return why;
        }
        step("Waiting for Let's Encrypt to verify the record");
        why = post(j, j->chal, "{}", false);
        if (!why) why = poll_status(j, j->authz, "valid", 40);
        const char *gone = txt_record(j, false);                              /* tidy up, either way */
        if (why) return why;
        if (gone) return gone;
    }
    char req[80];
    int n = snprintf(req, sizeof req, "request %s", j->name);
    if (step("Generating a new key") || (why = tls_do(req, (size_t)n))) return why;
    uint8_t *der = (uint8_t *)j->body;
    size_t dl = tls_csr_der(der, 1200);
    if (!dl) return "no signing request";
    char *csr64 = j->body + 1200;
    b64url(der, dl, csr64);
    char *pl = j->body + 3200;
    snprintf(pl, 1800, "{\"csr\":\"%s\"}", csr64);
    static char plc[1800];
    snprintf(plc, sizeof plc, "%s", pl);
    if (step("Completing the order") || (why = post(j, j->finalize, plc, false))) return why;
    if ((why = poll_status(j, j->order, "valid", 40))) return why;
    if (!jstr(j->r.body, "certificate", j->cert, sizeof j->cert, NULL)) return "Let's Encrypt returned no certificate";
    if (step("Downloading the certificate") || (why = post(j, j->cert, NULL, false))) return why;
    size_t len = pem_to_der(j->r.body, j->r.len);
    if (!len || len > 4096) return "the certificate is not understood";
    memmove(j->buf + 5, j->r.body, len);
    memcpy(j->buf, "cert\n", 5);
    if (s_staging) return NULL;                                                /* a test: keep what we have */
    step("Installing");
    return tls_do(j->buf, len + 5);
}

static void run(void *arg)
{
    static job_t j;
    const char *why;
    (void)arg;
    memset(&j, 0, sizeof j);
    snprintf(j.host, sizeof j.host, "%s", s_staging ? LE_STAGING : LE_HOST);
    j.buf = mbedtls_calloc(1, BUF);
    j.body = mbedtls_calloc(1, BUF);
    why = !j.buf || !j.body ? "out of memory" : renew(&j);
    hc_close();
    if (j.key) psa_destroy_key(j.key);
    mbedtls_free(j.buf);
    mbedtls_free(j.body);
    s_result_at = clock_unix();
    if (why) {
        snprintf(s_result, sizeof s_result, "%s%s: %s", s_staging ? "Test " : "", s_step, why);
        s_fails++;
    } else {
        snprintf(s_result, sizeof s_result, s_staging ? "Test passed: a staging certificate was issued and not installed" : "Renewed");
        s_fails = 0;
        if (!s_staging) {
            static body_t b;
            b = s_rec.b;
            b.last_ok = clock_unix();
            save(&b);
        }
    }
    s_step = "";
    s_running = false;
}

static bool start(bool staging)
{
    if (s_running || worker_busy(WORKER_RENEW)) return false;
    s_staging = staging;
    s_running = true;
    s_result[0] = 0;
    return worker_start(WORKER_RENEW, run, NULL);
}

/* Due once a third of the certificate's life is left; Let's Encrypt suggests that. */
static bool due(uint32_t now)
{
    uint32_t from, to;
    if (!tls_issued(&from, &to)) return false;
    return to < now || to - now < (to - from) / 3;
}

void acme_poll(void)
{
    worker_poll(WORKER_RENEW);
    uint32_t now = clock_unix();
    const body_t *b = &s_rec.b;
    if (!b->automatic || !b->has_secret || !clock_synced() || s_running || now < s_next_check) return;
    if (due(now)) {
        start(false);
        uint32_t back = s_fails > 4 ? DAY : 3600u << (s_fails < 4 ? s_fails : 4);   /* 1 h, then longer */
        s_next_check = now + (back < DAY ? back : DAY);
    } else {
        s_next_check = now + CHECK_EVERY;
    }
}

/* ---- settings ------------------------------------------------------------------------ */
static bool id_ok(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (!n || n >= max) return false;
    for (; *s; s++) if (!((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9'))) return false;
    return true;
}

const char *acme_command(char *body, const uint8_t *secret, size_t slen)
{
    static body_t b;
    if (!strcmp(body, "renew") || !strcmp(body, "test")) {
        if (!s_rec.b.has_secret || !s_rec.b.zone[0]) return "Enter the Route 53 details first";
        if (!clock_synced()) return "The clock is not set yet; try again in a minute";
        if (!start(!strcmp(body, "test"))) return "Already renewing";
        return NULL;
    }
    if (s_running) return "Renewing; try again when it has finished";
    b = s_rec.b;
    for (char *l = body, *next; l && *l; l = next) {
        if ((next = strchr(l, '\n'))) *next++ = 0;
        char *v = strchr(l, '=');
        if (!v) return "Not understood";
        *v++ = 0;
        if (!strcmp(l, "zone")) {
            if (*v && !id_ok(v, sizeof b.zone)) return "The hosted zone ID is letters and digits, like Z0123456789ABC";
            snprintf(b.zone, sizeof b.zone, "%s", v);
        } else if (!strcmp(l, "key_id")) {
            if (*v && !id_ok(v, sizeof b.key_id)) return "The access key ID is letters and digits, like AKIA…";
            snprintf(b.key_id, sizeof b.key_id, "%s", v);
        } else if (!strcmp(l, "auto")) {
            if (strcmp(v, "0") && strcmp(v, "1")) return "Not understood";
            b.automatic = *v == '1';
        } else {
            return "Not understood";
        }
    }
    if (secret) {
        size_t n = 0;
        while (n < slen && secret[n]) n++;
        for (size_t i = n; i < slen; i++) if (secret[i]) return "That secret cannot be used";
        if (n >= sizeof b.secret) return "That secret is too long";
        for (size_t i = 0; i < n; i++) if (secret[i] <= 0x20 || secret[i] >= 0x7f) return "That secret cannot be used";
        memset(b.secret, 0, sizeof b.secret);
        memcpy(b.secret, secret, n);
        b.has_secret = n > 0;
    }
    if (b.automatic && (!b.has_secret || !b.zone[0] || !b.key_id[0])) return "Automatic renewal needs the zone, the key ID and the secret";
    if (!memcmp(&b, &s_rec.b, sizeof b)) return NULL;
    if (!save(&b)) return "Could not write to flash";
    s_next_check = 0;
    return NULL;
}

size_t acme_json(char *out, size_t max)
{
    const body_t *b = &s_rec.b;
    uint32_t now = clock_unix(), from = 0, to = 0;
    const char *name = tls_issued(&from, &to);
    char iso[17] = "";
    clock_iso(iso, false);
    worker_stats_t w;
    worker_stats(WORKER_RENEW, &w);
    int n = snprintf(out, max,
        "{\"auto\":%s,\"zone\":\"%s\",\"key_id\":\"%s\",\"secret_set\":%s,\"running\":%s,\"step\":\"%s\",\"staging\":%s,"
        "\"result\":\"%s\",\"result_s\":%ld,\"last_ok\":%lu,\"clock\":\"%s\",\"clock_s\":%lu,\"name\":\"%s\",\"due_in_s\":%ld,\"tls_out\":%lu,"
        "\"slice_us\":%lu,\"stack\":[%lu,%lu]}\n",
        b->automatic ? "true" : "false", b->zone, b->key_id, b->has_secret ? "true" : "false", s_running ? "true" : "false",
        s_step, s_staging ? "true" : "false", s_result, s_result_at && now ? (long)(now - s_result_at) : -1L,
        (unsigned long)b->last_ok, iso, (unsigned long)clock_synced_ago(), name ? name : "",
        name && now ? (long)((int64_t)to - (int64_t)(to - from) / 3 - (int64_t)now) : 0L, (unsigned long)hc_handshakes(),
        (unsigned long)w.longest_us, (unsigned long)w.stack_used, (unsigned long)w.stack_size);
    return n > 0 && (size_t)n < max ? (size_t)n : 0;
}
