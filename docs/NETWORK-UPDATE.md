# Network update

| Where | What |
|---|---|
| internal flash, 128 KB | loader: network, HTTP, updates |
| ext flash 1 `0x100000` / `0x180000` | app slots A / B (4 KB header + up to 384 KB) |
| ext flash 0 `0x000000` to `0x1F4000` | library: 5 spaces of 400 KB |
| ext flash 1 `0x000000` to `0x0C8000` | library: 2 spaces of 400 KB |
| AXI SRAM `0x24000000` | the app runs here; the loader stages uploads here |

**The library overwrites the vendor's firmware and settings**, one space at a time as
images are kept. The slots use space that was blank.

```sh
H=n540-a1b2c3.local
tools/n540.py $H info
tools/n540.py $H update build/app.bin            # install, boot, confirm
tools/n540.py $H firmware                        # what is kept
tools/n540.py $H rollback VERSION                # start a kept one, confirm
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

**Library (app).** An upload goes to a library space as it arrives, never through RAM. It
is read back and hashed there, then copied to the spare slot and hashed again, then started
on trial. A bad or cut upload leaves both slots alone. A kept image about to lose its slot
is saved to the library first. When the library is full, images that failed their trial go
first, then the oldest; never the running one. `POST /rollback` with `sha=<hash>` starts a
kept one the same way. If any port is listen-only, firmware that does not know that setting
is refused. The loader knows nothing of the library: `update --loader` still goes straight
to a slot, through RAM.

`tests/test_fwlib.c` cuts the power at every flash operation of an install, in simulation.
`tests/sim/run.sh` runs the web server, library and slots on a computer, for
`tests/test_install.py 127.0.0.1:8541`.
**The library has not been run on a gateway yet.**

**Tested on hardware (2026-11-24):** loader self-update from SRAM and from flash,
interrupted upload, tampered body, replayed nonce, wrong path, junk and wrongly
linked images, app that hangs, app that never networks, recovery after each,
power-cycle boot (two, ST-Link detached).
**Not tested on hardware:** power loss during an install, the 3-minute
trial timeout, the crash-count limit.

**Known gap:** losing power during `selfupdate` (about 2 s) leaves no loader;
recover with `tools/ramrun.sh`.
