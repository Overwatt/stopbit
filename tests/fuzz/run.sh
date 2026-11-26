#!/bin/sh
# Builds the fuzz targets and runs each for SECONDS (default 600), in parallel.
#   tests/fuzz/run.sh [SECONDS] [TARGET...]
# Findings: $OUT/TARGET/crash-* and hang-*, each with a .log; corpora grow in $OUT/TARGET/corpus.
cd "$(dirname "$0")/../.." || exit 1
SECS=${1:-600}; shift 2>/dev/null
OUT=${FUZZ_OUT:-${TMPDIR:-/tmp}/n540fuzz}
mkdir -p "$OUT"
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
COV="-fsanitize-coverage=trace-pc-guard"
cc -g -O1 -fsanitize=address,undefined -c tests/fuzz/driver.c -o "$OUT/driver.o" || exit 1
build() {   # name, then sources and flags
  n=$1; shift
  cc $SAN $COV -w -Itests/fuzz/key "$@" "$OUT/driver.o" -o "$OUT/fuzz_$n" || { echo "build $n failed"; return 1; }
}
for t in ${*:-cfg http hclient}; do
  case $t in
  cfg) build cfg -Isrc/app tests/fuzz/cfg.c src/app/cfg.c src/app/acl.c ;;
  http) build http -DAPP_IMAGE -DFLASH_REAL -DWDT_H '-Dwdt_feed()=(void)0' -DAXI_BSS= -DCONN_MEM= \
          -Itests/fuzz -Itests/sim -Itests/stub -Isrc/app -Isrc/common -Isrc/http -Isrc/net \
          tests/fuzz/http.c src/http/http.c src/app/fwlib.c src/app/cfg.c src/app/acl.c src/app/events.c \
          src/common/slots.c src/common/image.c src/common/sha256.c tests/sim/id.c ;;
  hclient) build hclient -Itests/fuzz -Itests/sim -Itests/stub -Isrc/app -Isrc/common \
          -Ibuild/_deps/mbedtls4-src/include -Ibuild/_deps/mbedtls4-src/tf-psa-crypto/include \
          -Ibuild/_deps/mbedtls4-src/tf-psa-crypto/drivers/builtin/include \
          '-DMBEDTLS_CONFIG_FILE="mbedtls_cfg.h"' '-DTF_PSA_CRYPTO_CONFIG_FILE="crypto_cfg.h"' \
          tests/fuzz/hclient.c src/app/hclient.c ;;
  esac
  mkdir -p "$OUT/$t/corpus"
  cp tests/fuzz/seeds/$t/* "$OUT/$t/corpus/" 2>/dev/null
  d=tests/fuzz/dict/$t.txt; [ -f "$d" ] || d=""
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:symbolize=1 "$OUT/fuzz_$t" "$OUT/$t/corpus" "$SECS" "$d" 8192 2> "$OUT/$t/progress.log" &
done
wait
for t in ${*:-cfg http hclient}; do printf '%s: ' "$t"; tail -1 "$OUT/$t/progress.log"; ls "$OUT/$t" | grep -E '^(crash|hang)-[0-9a-f]+$' | head -5; done
