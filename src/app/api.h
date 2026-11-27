/* api.h — The app's JSON API and web page, called from http.c. */
#ifndef API_H
#define API_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool           machine;      /* signed with the update key, not the web password */
    const uint8_t *key;          /* whichever key signed it */
    const char    *nonce;
    char          *body;
    uint32_t       len;          /* of body, which may be binary */
} api_req_t;

void api_init(void);             /* loads config, starts network settings + bridge */
void api_poll(void);
/* Applies one "key=value" now and saves it shortly after (bursts write once). */
bool api_set_line(char *line, char *err, size_t errlen);
/* Saves anything pending, signs off from MQTT and resets. */
void api_restart(void) __attribute__((noreturn));
/* Length of JSON written, 0 if it did not fit, -1 if no such path. */
int  api_get(const char *path, char *out, size_t max);
/* GET /metrics, part by part: length, -1 past the last, -2 if a part does not fit. */
int  api_metrics(uint32_t part, char *out, size_t max);
bool api_is_post(const char *path);
/* Returns an HTTP status line tail; `out` gets the message or JSON. */
const char *api_post(const char *path, api_req_t *rq, char *out, size_t max);
const uint8_t *api_page(uint32_t *len);
const char *api_page_tag(void);          /* its ETag, quotes included */          /* gzipped HTML */
/* What firmware must contain to be started with these settings, or NULL. */
const char *api_fw_need(void);
/* Web-password key, or NULL if none is set. */
const uint8_t *api_web_key(void);
/* GET /api/devices and /api/settings, part by part (http.c's do_parts). */
int  api_devices(uint32_t part, char *out, size_t max);
int  api_settings(uint32_t part, char *out, size_t max);

#endif
