#!/usr/bin/env python3
"""gzip the web page into a C header."""
import gzip, sys
src, out = sys.argv[1:3]
z = gzip.compress(open(src, "rb").read(), 9, mtime=0)
with open(out, "w") as f:
    f.write("/* generated from web/index.html */\n#include <stdint.h>\nstatic const uint8_t WEB_PAGE[] = {"
            + ",".join(map(str, z)) + "};\n")
print(f"web page: {len(z)} bytes gzipped")
