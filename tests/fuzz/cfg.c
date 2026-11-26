/* cfg.c — Settings lines, as POST /api/config, the console and MQTT commands send them.
 * Whatever parses and validates must come back the same through cfg_lines(), and its
 * JSON must fit the reply buffer and be well formed. */
#include "cfg.h"
#include "acl.h"
#include "json_ok.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static cfg_t c, t;
    static char body[8192], err[128], text[8192], json[4344];
    if (n >= sizeof body) return 0;
    memcpy(body, d, n);
    body[n] = 0;
    cfg_defaults(&c);
    if (!cfg_parse(&c, body, err, sizeof err)) { assert(err[0]); return 0; }
    if (!cfg_validate(&c, err, sizeof err)) { assert(err[0]); return 0; }
    size_t k = cfg_lines(&c, text, sizeof text);
    assert(k && k < sizeof text);
    cfg_defaults(&t);
    assert(cfg_parse(&t, text, err, sizeof err));
    assert(cfg_validate(&t, err, sizeof err));
    memcpy(t.mqtt.password, c.mqtt.password, sizeof t.mqtt.password);   /* never written out */
    assert(!memcmp(&c, &t, sizeof c));
    assert(cfg_json(&c, json, sizeof json) && json_ok(json));
    for (int p = 0; p < PORTS; p++) (void)acl_role(&c.access[p], (uint32_t)rand());
    return 0;
}
