#!/usr/bin/env python3
"""Create the update key (once) and the header the firmware compiles in."""
import os, secrets, sys
keyfile, header = sys.argv[1:3]
if not os.path.exists(keyfile):
    os.makedirs(os.path.dirname(keyfile), exist_ok=True)
    fd = os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(secrets.token_hex(32) + "\n")
key = bytes.fromhex(open(keyfile).read().strip())
assert len(key) == 32
os.makedirs(os.path.dirname(header), exist_ok=True)
with open(header, "w") as f:
    f.write("/* generated; do not commit */\n#include <stdint.h>\nstatic const uint8_t SECRET_KEY[32] = {"
            + ",".join(map(str, key)) + "};\n")
