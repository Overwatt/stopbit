#!/bin/sh
# Builds the fuzz targets and runs each for SECONDS (default 600), in parallel.
#   tests/fuzz/run.sh [SECONDS] [TARGET...]
# Findings: $OUT/TARGET/crash-* and hang-*, each with a .log; corpora grow in $OUT/TARGET/corpus.
# Exits 1 if a target failed to build or found anything. Needs clang: CC=clang where cc is gcc.
{   # read whole before it starts: the script may be edited during a run of hours
cd "$(dirname "$0")/../.." || exit 1
SECS=${1:-600}; shift 2>/dev/null
OUT=${FUZZ_OUT:-${TMPDIR:-/tmp}/n540fuzz}
CC=${CC:-cc}
mkdir -p "$OUT"
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -D_GNU_SOURCE"
COV="-fsanitize-coverage=trace-pc-guard"
$CC -g -O1 -fsanitize=address,undefined -c tests/fuzz/driver.c -o "$OUT/driver.o" || exit 1
build() {   # name, then sources and flags
  n=$1; shift
  $CC $SAN $COV -w -Itests/fuzz/key "$@" "$OUT/driver.o" -o "$OUT/fuzz_$n" || { echo "build $n failed"; return 1; }
}
bad=0
for t in ${*:-cfg http hclient modbus}; do
  case $t in
  cfg) build cfg -Isrc/app tests/fuzz/cfg.c src/app/cfg.c src/app/acl.c ;;
  modbus) build modbus -Isrc/app tests/fuzz/modbus.c src/app/modbus.c ;;
  http) build http -DAPP_IMAGE -DFLASH_REAL -DWDT_H '-Dwdt_feed()=(void)0' -DAXI_BSS= -DCONN_MEM= \
          -Itests/fuzz -Itests/sim -Itests/stub -Isrc/app -Isrc/common -Isrc/http -Isrc/net \
          tests/fuzz/http.c src/http/http.c src/app/fwlib.c src/app/cfg.c src/app/acl.c src/app/events.c \
          src/common/slots.c src/common/image.c src/common/sha256.c tests/sim/id.c ;;
  hclient) build hclient -Itests/fuzz -Itests/sim -Itests/stub -Isrc/app -Isrc/common \
          -Ibuild/_deps/mbedtls4-src/include -Ibuild/_deps/mbedtls4-src/tf-psa-crypto/include \
          -Ibuild/_deps/mbedtls4-src/tf-psa-crypto/drivers/builtin/include \
          '-DMBEDTLS_CONFIG_FILE="mbedtls_cfg.h"' '-DTF_PSA_CRYPTO_CONFIG_FILE="crypto_cfg.h"' \
          tests/fuzz/hclient.c src/app/hclient.c ;;
  acme) M=build/_deps/mbedtls4-src; P=$M/tf-psa-crypto
        SO="${TMPDIR:-/tmp}/n540sim4.o$(echo "$CC" | tr -c "a-z0-9\n" _ | sed "s/^cc//")"   # as tests/sim/run.sh names it
        if [ -f "$SO/done" ]; then
          build acme -DAPP_IMAGE -DCLOCK_HOST -DWDT_H '-Dwdt_feed()=(void)0' \
            '-DMBEDTLS_CONFIG_FILE="mbedtls_cfg.h"' '-DTF_PSA_CRYPTO_CONFIG_FILE="crypto_cfg.h"' \
            -Itests/fuzz -Itests/sim -Itests/stub -Isrc/app -Isrc/common -I$M/include -I$P/include -I$P/drivers/builtin/include \
            tests/fuzz/acme.c src/app/clock.c src/app/cfg.c src/app/acl.c src/common/slots.c src/common/image.c src/common/sha256.c "$SO"/*.o
        else
          echo "acme: run tests/sim/run.sh once first, with the same CC, for the crypto objects"; false
        fi ;;
  *) echo "no target $t"; false ;;
  esac || { bad=1; continue; }
  mkdir -p "$OUT/$t/corpus"
  cp tests/fuzz/seeds/$t/* "$OUT/$t/corpus/" 2>/dev/null
  d=tests/fuzz/dict/$t.txt; [ -f "$d" ] || d=""
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:symbolize=1 "$OUT/fuzz_$t" "$OUT/$t/corpus" "$SECS" "$d" 8192 2> "$OUT/$t/progress.log" &
done
wait
for t in ${*:-cfg http hclient modbus}; do
  [ -f "$OUT/$t/progress.log" ] || continue
  printf '%s: ' "$t"; tail -1 "$OUT/$t/progress.log"
  found=$(ls "$OUT/$t" | grep -E '^(crash|hang)-[0-9a-f]+$')
  [ -z "$found" ] || { echo "$found" | head -5; bad=1; }
done
exit $bad
}
