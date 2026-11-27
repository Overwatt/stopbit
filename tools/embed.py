#!/usr/bin/env python3
"""gzip the web page into a C header."""
import gzip, hashlib, sys
src, out = sys.argv[1:3]
z = gzip.compress(open(src, "rb").read(), 9, mtime=0)
tag = hashlib.sha256(z).hexdigest()[:16]          # the page's ETag: changes when the page does
with open(out, "w") as f:
    f.write("/* generated from web/index.html */\n#include <stdint.h>\n#define WEB_PAGE_TAG \"\\\"" + tag + "\\\"\"\n"
            "static const uint8_t WEB_PAGE[] = {" + ",".join(map(str, z)) + "};\n")
print(f"web page: {len(z)} bytes gzipped")
