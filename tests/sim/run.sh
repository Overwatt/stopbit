#!/bin/sh
# Builds and runs the simulator: HTTP on 127.0.0.1:8541, HTTPS (once on) on 8543.
# Needs build/app.bin and the mbedTLS that the firmware build fetched.
cd "$(dirname "$0")/../.." || exit 1
O="${TMPDIR:-/tmp}/n540sim4.o"
M=build/_deps/mbedtls4-src
P=$M/tf-psa-crypto
CFG='-DMBEDTLS_CONFIG_FILE="mbedtls_cfg.h" -DTF_PSA_CRYPTO_CONFIG_FILE="crypto_cfg.h"'
PUB="-Isrc/app -I$M/include -I$P/include -I$P/drivers/builtin/include"
PRIV="-I$M/library -I$P/core -I$P/dispatch -I$P/drivers/builtin/src -I$P/extras -I$P/platform -I$P/utilities"
FLAGS="-g -O1 -fsanitize=address -w $PUB"
mkdir -p "$O"
if [ ! -f "$O/done" ] || [ src/app/mbedtls_cfg.h -nt "$O/done" ] || [ src/app/crypto_cfg.h -nt "$O/done" ]; then
  for f in "$M"/library/*.c "$P"/core/*.c "$P"/drivers/builtin/src/*.c "$P"/extras/*.c "$P"/platform/*.c "$P"/utilities/*.c; do
    n=$(basename "$f" .c); d=
    [ "$n" = ecp ] && d=-Dmbedtls_mpi_mul_mpi=tls_mpi_mul_mpi
    [ "$n" = bignum_core ] && d=-Dmemcpy=tls_core_memcpy
    # the worker's own stack confuses the address checker, so not in what runs on it
    cc -g -O1 -w -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 $PUB $PRIV "$(echo $CFG | cut -d' ' -f1)" "$(echo $CFG | cut -d' ' -f2)" $d -c "$f" -o "$O/$n.o" || exit 1
  done
  touch "$O/done"
fi
cc $FLAGS "$(echo $CFG | cut -d' ' -f1)" "$(echo $CFG | cut -d' ' -f2)" -DAPP_IMAGE -DFLASH_REAL -DSIM_TICK -DWDT_H '-Dwdt_feed()=(void)0' -DAXI_BSS= -DAXI_HI= -DCONN_MEM= -DWORKER_STACK=131072 ${SIM_FLAGS} \
  -Itests/sim -Itests/stub -Isrc/common -Isrc/http -Isrc/net -Ibuild/gen \
  tests/sim/sim.c tests/sim/id.c src/http/http.c src/app/fwlib.c src/app/tls.c src/app/worker.c \
  src/common/slots.c src/common/image.c src/common/sha256.c "$O"/*.o -o "${TMPDIR:-/tmp}/n540sim" && exec "${TMPDIR:-/tmp}/n540sim" build/app.bin
