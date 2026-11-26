/* json_ok.h — Whether a reply is one well-formed JSON value (RFC 8259), with nothing
 * after it but white space. The web page parses every reply; one bad string spoils
 * the whole page. */
#ifndef JSON_OK_H
#define JSON_OK_H
#include <stdbool.h>
#include <string.h>

static const char *json_val(const char *p, int depth);

static const char *json_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

static const char *json_strv(const char *p)
{
    if (*p++ != '"') return NULL;
    for (;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') return p + 1;
        if (c < 0x20) return NULL;                          /* the end of the text, or a raw control */
        if (c == '\\') {
            c = (unsigned char)*++p;
            if (c == 'u') {
                for (int i = 1; i <= 4; i++) if (!p[i] || !strchr("0123456789abcdefABCDEF", p[i])) return NULL;
                p += 4;
            } else if (!c || !strchr("\"\\/bfnrt", c)) return NULL;
        }
    }
}

static const char *json_num(const char *p)
{
    if (*p == '-') p++;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (*p >= '0' && *p <= '9') p++;
    else return NULL;
    if (*p == '.') { p++; if (*p < '0' || *p > '9') return NULL; while (*p >= '0' && *p <= '9') p++; }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (*p < '0' || *p > '9') return NULL;
        while (*p >= '0' && *p <= '9') p++;
    }
    return p;
}

static const char *json_val(const char *p, int depth)
{
    if (depth > 32) return NULL;
    p = json_ws(p);
    if (*p == '"') return json_strv(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = json_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                if (!(p = json_strv(json_ws(p)))) return NULL;
                if (*(p = json_ws(p)) != ':') return NULL;
                p++;
            }
            if (!(p = json_val(p, depth + 1))) return NULL;
            p = json_ws(p);
            if (*p == close) return p + 1;
            if (*p++ != ',') return NULL;
        }
    }
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4)) return p + 4;
    return json_num(p);
}

static bool json_ok(const char *s)
{
    const char *e = json_val(s, 0);
    return e && !*json_ws(e);
}
#endif
