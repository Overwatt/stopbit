# Roadmap

Milestone ladder from "boots" to "better than stock." Each rung is a working,
testable state. Scope is kept deliberately narrow — we build the open/hackable
gateway we want, not a clone of USR's whole feature matrix.

## Direction (current): our firmware + network update, not stock restore

We run our own firmware on the board and prioritize a **network firmware-update
path** so SWD flashing stops mattering. Reach: Ethernet up → a TCP/HTTP endpoint
that writes a received image to flash → split into a tiny **bootloader**
(SWD-flashed once) + **app** (updated over the network) with a golden-image
fallback. This mirrors USR's own "web_boot" design. After that, SWD is only for
first-flash/emergencies; everyday updates are `curl` over the LAN.

## ⚠️ Flashing over SWD — hard-won lessons (read before flashing)

- **The board MUST have its own external power connected.** On the ST-Link's
  3.3 V alone, reads/erases work but **flash *programming* silently fails** (the
  program charge pumps need real current) — you read back blank and think the
  write failed. This caused a multi-hour brick scare. Always power the board.
- ST-Link into a **direct USB port** (not a hub); **3.3V/VTref wire connected**
  (header pin 1). SWD header: `1=3V3, 2=GND, 3/4=SWDIO/NRST, 5=SWCLK`.
- This ST-Link **clone can't connect-under-reset**, so a corrupt flash → CPU
  lockup → debug-domain gating → intermittent examine failures. Workaround =
  "escape" trick (run a loop in SRAM to get a clean CPU state), then flash. A
  **genuine ST-Link/J-Link removes all of this.** Recovery scripts live on
  `swd-host:~/n540_dumps/*.cfg`.

## M0 — Back up the stock firmware (bench, prerequisite)
- [ ] SWD: connect ST-Link, read Option Bytes → **RDP level** (go/no-go for the
      whole project — RDP2 means SWD is dead and we stop).
- [ ] If RDP0: dump internal 128 KB (`0x08000000`) → `dumps/internal_bootloader.bin`.
- [ ] CH341A + SOIC-8 clip: dump both external GD SPI flashes (hold MCU in reset).
- [ ] Analyse dumps → confirm boot flow, begin extracting the **pin map**.

## M1 — Boot + heartbeat + RS485 echo  *(scaffolded)*
- [x] Project structure, CMake build, linker script, HAL vendored.
- [x] Clock to 400 MHz from 25 MHz HSE.
- [x] Heartbeat LED; 4× RS485 init + loopback echo (polling).
- [ ] **Fill in real pins in `board.h`** (from M0), flash, confirm LED blinks
      and each RS485 port echoes on a scope / USB-RS485 adapter.

## M2 — Serial plumbing for real
- [ ] Interrupt/DMA RX with per-port ring buffers (replace polling).
- [ ] Runtime serial config per port (baud/parity/stop).
- [ ] `T R G` debug console: printf logging + a tiny CLI.

## M3 — Ethernet up
- [ ] Identify PHY (LAN8720 / RTL8201 / DP83848?), set address + reset line.
- [ ] HAL ETH + **LwIP** (add dep: lwip `STABLE-2_2_1_RELEASE`).
- [ ] MPU region for non-cacheable DMA descriptors (M7 D-cache coherency).
- [ ] DHCP + static IP; **ping** the board. 🎯 first big milestone.

## M4 — The core product: transparent RS485 ↔ TCP bridge
- [ ] Each RS485 port ↔ a TCP server socket (configurable port).
- [ ] Bidirectional byte shuttling with backpressure.
- [ ] TCP client + UDP modes.

## M5 — "Better than stock" (pick what's fun)
- [ ] Modern web UI + **REST/JSON API** (vs. their 2010 iframe UI).
- [ ] **MQTT** with Home Assistant auto-discovery.
- [ ] Prometheus metrics / live per-port traffic + latency stats.
- [ ] TLS, real auth (no telnet data-port backdoor).
- [ ] Modbus TCP ↔ RTU gateway with multi-master queuing.
- [ ] Embedded scripting (Lua / MicroPython) for edge logic.

## Known risks / open questions
- Power supply topology (LDO vs SMPS) affects clock bring-up — see `system_clock.c`.
- 128 KB internal flash is tight once LwIP + web land → plan to move code to
  external QSPI (XIP @ `0x90000000`) with a small internal bootloader.
- RS485 DE line: using manual GPIO toggle; confirm whether DE sits on a UART
  native DE/RTS pin (would allow hardware auto-direction).
