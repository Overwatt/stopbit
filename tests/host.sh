#!/bin/sh
# Every host test: built with the address and undefined-behaviour checks, run, stop at the first failure.
#   tests/host.sh            (CC=gcc tests/host.sh to use another compiler)
cd "$(dirname "$0")/.." || exit 1
CC=${CC:-cc}
O=${TMPDIR:-/tmp}/n540host; mkdir -p "$O"
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -w"
fail=0
t() {   # name, then flags and sources
  n=$1; shift
  if ! $CC $SAN "$@" -o "$O/$n" 2> "$O/$n.build"; then echo "$n: BUILD FAILED"; cat "$O/$n.build"; fail=1; return; fi
  if out=$(ASAN_OPTIONS=detect_leaks=0 "$O/$n" 2>&1); then echo "$n: $(echo "$out" | tail -1)"; else echo "$n: FAILED"; echo "$out" | tail -20; fail=1; fi
}
python3 tools/embed_profiles.py "$O/profiles_gen.h" profiles/*.txt > /dev/null || exit 1
t acl     -Isrc/app tests/test_acl.c src/app/acl.c src/app/cfg.c
t bridge  -DD2= -DD2_BSS= -Itests/stub_net -Itests/stub -Isrc/app tests/test_bridge.c src/app/bridge.c src/app/cfg.c src/app/acl.c src/app/knock.c src/app/traffic.c src/app/history.c src/app/modbus.c
t button  -Isrc/app tests/test_button.c src/app/button.c src/app/cfg.c src/app/acl.c
t cfg     -Isrc/app tests/test_cfg.c src/app/cfg.c src/app/acl.c
t eth     -DAPP_IMAGE '-DDMA_MEM=__attribute__((aligned(32)))' -Itests/stub_eth -Isrc/net tests/test_eth.c src/net/eth_mac.c
t cli     -DD2_BSS= -Isrc/app tests/test_cli.c src/app/cli.c src/app/cfg.c src/app/acl.c src/app/traffic.c
t fwlib   -Itests/stub -Isrc/app -Isrc/common -DWDT_H '-Dwdt_feed()=(void)0' -DAXI_BSS= tests/test_fwlib.c src/app/fwlib.c src/common/slots.c src/common/image.c src/common/sha256.c
t history -DD2_BSS= -Isrc/app tests/test_history.c src/app/history.c
t knock   -Isrc/app tests/test_knock.c src/app/knock.c
t modbus  -Isrc/app tests/test_modbus.c src/app/modbus.c
t profile -Isrc/app tests/test_profile.c src/app/profile.c
t pfdecode -Isrc/app tests/test_pfdecode.c src/app/pfdecode.c src/app/profile.c -lm
t pfwrite -Isrc/app tests/test_pfwrite.c src/app/pfwrite.c src/app/profile.c -lm
t builtin -I"$O" -Isrc/app tests/test_builtin.c src/app/profiles.c src/app/profile.c src/app/pfdecode.c -lm
t device -DD2_BSS= -I"$O" -Itests/stub -Isrc/app tests/test_device.c tests/stub/events_stub.c src/app/pfwrite.c src/app/device.c src/app/modbus.c src/app/profiles.c src/app/profile.c src/app/pfdecode.c src/app/cfg.c src/app/acl.c -lm
t devmqtt -DD2_BSS= -I"$O" -Itests/stub -Isrc/app tests/test_devmqtt.c src/app/devmqtt.c tests/stub/events_stub.c src/app/pfwrite.c src/app/device.c src/app/modbus.c src/app/profiles.c src/app/profile.c src/app/pfdecode.c src/app/cfg.c src/app/acl.c -lm
t devroom -DD2_BSS= -DPF_ROOM=4096 -Itests/stub -Isrc/app tests/test_devroom.c tests/stub/events_stub.c src/app/pfwrite.c src/app/device.c src/app/modbus.c src/app/profile.c src/app/pfdecode.c src/app/cfg.c src/app/acl.c -lm
t devmqtt_long -DD2_BSS= -I"$O" -Itests/stub -Isrc/app tests/test_devmqtt_long.c src/app/devmqtt.c tests/stub/events_stub.c src/app/pfwrite.c src/app/device.c src/app/modbus.c src/app/profile.c src/app/pfdecode.c src/app/cfg.c src/app/acl.c -lm
t devctl -DD2_BSS= -Itests/stub -Isrc/app tests/test_devctl.c src/app/devmqtt.c tests/stub/events_stub.c src/app/device.c src/app/pfwrite.c src/app/modbus.c src/app/profile.c src/app/pfdecode.c src/app/cfg.c src/app/acl.c -lm
t metrics -Isrc/app tests/test_metrics.c src/app/metrics.c
t quiet   -Isrc/app tests/test_quiet.c
t events  -Itests/stub -Isrc/app -Isrc/common -DWDT_H '-Dwdt_feed()=(void)0' tests/test_events.c src/app/events.c src/app/cfg.c src/app/acl.c src/common/slots.c src/common/image.c src/common/sha256.c
t store   -Itests/stub -Isrc/app -Isrc/common -DWDT_H '-Dwdt_feed()=(void)0' tests/test_store.c src/app/cfg_store.c src/app/cfg.c src/app/acl.c src/common/sha256.c
# The gateway's printf (newlib nano) has no 64-bit numbers: %ll prints "lu".
if grep -rn '%[-0-9.]*ll[dux]' src --include='*.c' --include='*.h'; then echo "printf: no %ll on the gateway"; fail=1; fi
exit $fail
