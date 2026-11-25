#!/bin/sh
# Run openocd commands against the board without resetting it.
# usage: tools/swd.sh 'cmd' 'cmd' ...   (wrap reads as: echo [mdw ADDR N])
A=""; for c in "$@"; do A="$A -c '$c'"; done
ssh "${N540_SWD_HOST:-swd-host}" "sudo timeout 60 openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
 -c 'adapter speed 1800' -c 'gdb_port disabled' -c 'tcl_port disabled' -c 'telnet_port disabled' \
 -c init $A -c shutdown" 2>&1 | sed -e '1,/gdb port disabled/d'
