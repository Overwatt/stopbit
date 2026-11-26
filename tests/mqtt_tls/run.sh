#!/bin/sh
# The gateway's trusted roots (tests/test_roots.c), then MQTT over TLS against Mosquitto
# (tests/test_mqtt_tls.c). Makes a test CA and broker
# certificates (ECDSA and RSA, for mqtt.test), starts Mosquitto on 127.0.0.1, retains a
# 40000-byte message, and runs the test. Needs mosquitto, mosquitto_pub, openssl,
# python3, and the mbedTLS objects tests/sim/run.sh builds (run it once, same CC).
# mbedTLS's heap aligns to 4 bytes, enough for the gateway, so alignment is not checked.
cd "$(dirname "$0")/../.." || exit 1
CC=${CC:-cc}
W=${TMPDIR:-/tmp}/n540mqtt
SO="${TMPDIR:-/tmp}/n540sim4.o$(echo "$CC" | tr -c "a-z0-9\n" _ | sed "s/^cc//")"
[ -f "$SO/done" ] || { echo "run tests/sim/run.sh once first, with the same CC, for the mbedTLS objects"; exit 1; }
MOSQ=$(command -v mosquitto || echo /opt/homebrew/sbin/mosquitto); [ -x "$MOSQ" ] || MOSQ=/usr/sbin/mosquitto
rm -rf "${W:?}"; mkdir -p "$W"
L=build/_deps/lwip-src/src; M=build/_deps/mbedtls4-src; P=$M/tf-psa-crypto
MB="-I$M/include -I$P/include -I$P/drivers/builtin/include"
MBCFG='-DTF_PSA_CRYPTO_CONFIG_FILE="crypto_cfg.h"'

$CC -g -O1 -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all -w \
  '-DMBEDTLS_CONFIG_FILE="mbedtls_cfg.h"' "$MBCFG" -Isrc/app $MB \
  tests/test_roots.c src/app/roots.c "$SO"/*.o -o "$W/test_roots" || exit 1
ASAN_OPTIONS=detect_leaks=0 "$W/test_roots" tests/roots || exit 1

( cd "$W" || exit 1
  openssl ecparam -name prime256v1 -genkey -noout -out ca.key
  openssl req -x509 -new -key ca.key -sha256 -days 30 -subj /CN=n540-test-ca -out ca.crt
  printf 'subjectAltName=DNS:mqtt.test\nextendedKeyUsage=serverAuth\n' > ext
  openssl ecparam -name prime256v1 -genkey -noout -out ec.key
  openssl req -new -key ec.key -subj /CN=mqtt.test -out ec.csr
  openssl x509 -req -in ec.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 30 -sha256 -extfile ext -out ec.crt
  openssl genrsa -out rsa.key 2048
  openssl req -new -key rsa.key -subj /CN=mqtt.test -out rsa.csr
  openssl x509 -req -in rsa.csr -CA ca.crt -CAkey ca.key -CAserial ca.srl -days 30 -sha256 -extfile ext -out rsa.crt
  openssl ecparam -name prime256v1 -genkey -noout -out other.key
  openssl req -x509 -new -key other.key -sha256 -days 30 -subj /CN=someone-else -out other.crt
  openssl x509 -in ca.crt -outform der -out ca.der
  openssl x509 -in other.crt -outform der -out other.der
  python3 -c "import sys; sys.stdout.buffer.write(bytes((k * 7 + 3) & 255 for k in range(40000)))" > big.bin
  cat > mosquitto.conf <<EOF
per_listener_settings false
allow_anonymous true
persistence false
log_dest stderr
log_type error
listener 11883 127.0.0.1
listener 18883 127.0.0.1
certfile $W/ec.crt
keyfile $W/ec.key
listener 18884 127.0.0.1
certfile $W/rsa.crt
keyfile $W/rsa.key
EOF
) > "$W/certs.log" 2>&1 || { cat "$W/certs.log"; exit 1; }

"$MOSQ" -c "$W/mosquitto.conf" 2> "$W/mosquitto.log" &
broker=$!
for i in $(seq 50); do mosquitto_pub -p 11883 -t n540test/big -r -f "$W/big.bin" 2>/dev/null && break; sleep 0.2; done

$CC -g -O1 -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all -w -DAXI_BSS= \
  -DWORKER_TLS_STACK=8 -DWORKER_RENEW_STACK=8 -DWORKER_MQTT_STACK=131072 \
  '-DMBEDTLS_CONFIG_FILE="mbedtls_cfg.h"' '-DTF_PSA_CRYPTO_CONFIG_FILE="crypto_cfg.h"' \
  -Itests/mqtt_tls -I$L/include -Itests/stub -Isrc/app -I$M/include -I$P/include -I$P/drivers/builtin/include \
  tests/test_mqtt_tls.c src/app/mqtt_tls.c src/app/worker.c \
  $L/core/def.c $L/core/pbuf.c $L/core/mem.c $L/core/memp.c $L/core/altcp.c $L/core/altcp_alloc.c $L/core/ipv4/ip4_addr.c $L/apps/mqtt/mqtt.c \
  "$SO"/*.o -o "$W/test_mqtt_tls" || { kill $broker; exit 1; }
ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0 "$W/test_mqtt_tls" "$W" $broker
rc=$?
kill $broker 2>/dev/null
[ $rc = 0 ] || { echo "mosquitto said:"; tail -20 "$W/mosquitto.log"; }
exit $rc
