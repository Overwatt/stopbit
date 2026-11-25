#!/bin/sh
# Load loader_ram.bin into AXI SRAM over SWD and start it. Writes no flash.
# usage: tools/ramrun.sh [host-with-stlink]
set -e
H=${1:-swd-host}
D=$(cd "$(dirname "$0")/.." && pwd)
B=$D/build/loader_ram.bin
SP=0x$(xxd -e -l 4 "$B" | awk '{print $2}')
PC=0x$(xxd -e -s 4 -l 4 "$B" | awk '{print $2}')
scp -q "$B" "$H":n540_dumps/loader_ram.bin
ssh "$H" "cd ~/n540_dumps && sudo timeout 60 openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
 -c 'adapter speed 1800' -c 'gdb_port disabled' -c 'tcl_port disabled' -c 'telnet_port disabled' \
 -c 'init' -c 'reset halt' -c 'mmw 0x580244E0 4 0' -c 'mmw 0x58020800 0x100000 0x200000' -c 'mww 0x58020818 0x400' -c 'mww 0x58020818 0x4000000' -c 'load_image loader_ram.bin 0x24060000 bin' \
 -c 'verify_image loader_ram.bin 0x24060000 bin' \
 -c 'reg msp $SP' -c 'reg pc $PC' -c 'reg xPSR 0x01000000' -c 'resume' -c 'shutdown'" 2>&1 | tail -12
