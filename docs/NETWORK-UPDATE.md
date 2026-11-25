# Network update

| Where | What |
|---|---|
| internal flash, 128 KB | loader: network, HTTP, updates |
| ext flash 1 `0x100000` / `0x180000` | app slots A / B (4 KB header + up to 384 KB) |
| AXI SRAM `0x24000000` | staged upload; the app runs here |

The stock firmware's contents in both external flashes are untouched; the slots
use space that was blank.

```sh
H=n540-a1b2c3.local
tools/n540.py $H info
tools/n540.py $H update build/app.bin            # install, boot, confirm
tools/n540.py $H selfupdate build/loader_flash.bin
tools/n540.py $H run build/app.bin               # SRAM only, nothing written
tools/n540.py $H dump 1 out.bin                  # read external flash
```

**Key.** The first configure creates `secrets/n540.key` (gitignored) and compiles
it into images. Back it up: a loader built with another key needs SWD to replace.
Requests carry an HMAC over a single-use nonce, the path and the body hash, so the
key never crosses the wire. `/status` is open and nothing is encrypted.

**Boot.** Newest bootable slot wins. A new image gets 3 boots until `update`
confirms it; otherwise the loader falls back to the other slot. An unconfirmed
app also reboots itself after 3 minutes. Five boots in a row without 30 s of
uptime keeps the board in the loader; a power-on reset clears that count.
`info` shows the last six reset causes (`fe0000` power-on, `460000` watchdog or
NRST pin, `1460000` software). These survive short power cycles.

**Tested on hardware (2026-11-24):** loader self-update from SRAM and from flash,
interrupted upload, tampered body, replayed nonce, wrong path, junk and wrongly
linked images, app that hangs, app that never networks, recovery after each,
power-cycle boot (two, ST-Link detached).
**Not tested:** power loss during an install, the 3-minute
trial timeout, the crash-count limit.

**Known gap:** losing power during `selfupdate` (about 2 s) leaves no loader;
recover with `tools/ramrun.sh`.
