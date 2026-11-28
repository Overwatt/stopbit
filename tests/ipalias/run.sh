#!/bin/sh
# src/ipalias, and what uses it, against real lwIP on the host (tests/ipalias/lwipopts.h).
# Needs lwIP's source, which configuring the firmware build fetches (build/_deps/lwip-src).
#   tests/ipalias/run.sh            (CC=gcc tests/ipalias/run.sh to use another compiler)
cd "$(dirname "$0")/../.." || exit 1
CC=${CC:-cc}
L=build/_deps/lwip-src/src
[ -d "$L" ] || { echo "configure the firmware build once first (cmake -B build ...), for lwIP's source"; exit 1; }
O=${TMPDIR:-/tmp}/n540ipalias; mkdir -p "$O"
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -w"
LW="$L/core/init.c $L/core/def.c $L/core/inet_chksum.c $L/core/ip.c $L/core/mem.c $L/core/memp.c
    $L/core/netif.c $L/core/pbuf.c $L/core/tcp.c $L/core/tcp_in.c $L/core/tcp_out.c $L/core/timeouts.c
    $L/core/udp.c $L/core/ipv4/acd.c $L/core/ipv4/etharp.c $L/core/ipv4/icmp.c $L/core/ipv4/ip4.c
    $L/core/ipv4/ip4_addr.c $L/core/ipv4/ip4_frag.c $L/netif/ethernet.c"
fail=0
t() {   # name, then flags and sources
  n=$1; shift
  if ! $CC $SAN -Itests/ipalias -Isrc/ipalias -I$L/include "$@" $LW -o "$O/$n" 2> "$O/$n.build"; then
    echo "$n: BUILD FAILED"; cat "$O/$n.build"; fail=1; return; fi
  if out=$(ASAN_OPTIONS=detect_leaks=0 "$O/$n" 2>&1); then echo "$n: $(echo "$out" | tail -1)"
  else echo "$n: FAILED"; echo "$out" | tail -20; fail=1; fi
}
t ipalias tests/ipalias/test_ipalias.c src/ipalias/ipalias.c
t addrs -Itests/stub -Isrc/app -Isrc/net tests/ipalias/test_addrs.c src/app/addrs.c src/ipalias/ipalias.c tests/stub/events_stub.c src/app/cfg.c src/app/acl.c
exit $fail
