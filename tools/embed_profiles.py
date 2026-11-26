#!/usr/bin/env python3
"""The built-in device profiles (profiles/*.txt) into a C header: each one's id and text."""
import re, sys
out, files = sys.argv[1], sorted(sys.argv[2:])
rows = []
for f in files:
    data = open(f, "rb").read()
    m = re.search(rb"^[ \t]*profile[ \t]+([a-z0-9-]+)[ \t]", data, re.M)
    if not m:
        sys.exit(f"{f}: no 'profile id' line")
    rows.append((m.group(1).decode(), data))
with open(out, "w") as o:
    o.write("/* generated from the files in profiles/ by tools/embed_profiles.py */\n")
    for k, (pid, data) in enumerate(rows):
        o.write(f"static const char PROFILE_{k}[] = {{" + ",".join(str(b) for b in data) + ",0};\n")
    o.write("static const struct { const char *id, *text; unsigned len; } BUILTIN[] = {\n")
    for k, (pid, data) in enumerate(rows):
        o.write(f'    {{ "{pid}", PROFILE_{k}, {len(data)}u }},\n')
    o.write("};\n")
print(f"profiles: {len(rows)} built in")
