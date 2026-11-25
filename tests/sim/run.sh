#!/bin/sh
# Builds and runs the simulator on 127.0.0.1:8541. Needs build/app.bin.
cd "$(dirname "$0")/../.." || exit 1
cc -g -O1 -fsanitize=address -w -DAPP_IMAGE -DFLASH_REAL -DWDT_H '-Dwdt_feed()=(void)0' -DAXI_BSS= -DCONN_MEM= \
  -Itests/sim -Itests/stub -Isrc/app -Isrc/common -Isrc/http -Isrc/net -Ibuild/gen \
  tests/sim/sim.c tests/sim/id.c src/http/http.c src/app/fwlib.c src/common/slots.c src/common/image.c src/common/sha256.c \
  -o "${TMPDIR:-/tmp}/n540sim" && exec "${TMPDIR:-/tmp}/n540sim" build/app.bin
