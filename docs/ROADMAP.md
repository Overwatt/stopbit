# Roadmap

Milestone ladder from "boots" to "better than stock." Each rung is a working,
testable state. Scope is kept deliberately narrow — we build the open/hackable
gateway we want, not a clone of USR's whole feature matrix.

## Direction

We run our own firmware and update it over the network. SWD is only for first
flash and emergencies. See [NETWORK-UPDATE.md](NETWORK-UPDATE.md).

## Flashing over SWD (read before flashing)

- **An external watchdog on PC10 pulls NRST about every 1.6 s unless PC10
  toggles, even while the core is halted.** Any SWD flash operation longer than
  that gets reset part-way. This very likely caused, or contributed to, the
  "brick" of 2026-11-24. Connecting external power coincided with the recovery,
  so keep it connected, but it was never isolated as the cause.
- Preferred route: `tools/ramrun.sh` loads the loader into SRAM over SWD (no
  flash writes), then `tools/n540.py HOST selfupdate build/loader_flash.bin`
  writes internal flash, feeding the watchdog as it goes.
- ST-Link on a direct USB port, VTref wired. SWD header:
  `1=3V3, 2=GND, 3/4=SWDIO/NRST, 5=SWCLK`. This clone cannot
  connect-under-reset.

## M0 — Back up the stock firmware (bench, prerequisite)
- [x] SWD: connect ST-Link, read Option Bytes → **RDP level** (go/no-go for the
      whole project — RDP2 means SWD is dead and we stop).
- [x] If RDP0: dump internal 128 KB (`0x08000000`) → `dumps/internal_bootloader.bin`.
- [x] Dump both external flashes (over the network, via our loader).
- [ ] Analyse dumps → confirm boot flow, begin extracting the **pin map**.

## M1 — Boot + heartbeat + RS485 echo  *(scaffolded)*
- [x] Project structure, CMake build, linker script, HAL vendored.
- [x] Clock to 400 MHz from 25 MHz HSE.
- [x] Heartbeat LED; 4× RS485 init + loopback echo (polling).
- [ ] **Fill in real pins in `board.h`** (from M0), flash, confirm LED blinks
      and each RS485 port echoes on a scope / USB-RS485 adapter.

## M2 — Serial plumbing for real
- [x] Interrupt/DMA RX with per-port ring buffers (replace polling).
- [x] Runtime serial config per port (baud/parity/stop).
- [ ] `T R G` debug console: printf logging + a tiny CLI.

## M3 — Ethernet up
- [x] PHY: IP101G (ID 0x02430C54) at MDIO address 1. No reset line needed so far.
- [x] Own polled MAC driver + **LwIP** 2.2.1 (raw API).
- [x] MPU region for non-cacheable DMA memory (D-cache itself still off).
- [x] DHCP, mDNS, **ping** (~0.4 ms).
- [x] Static IP configuration.
- [x] Network update: loader self-update, A/B app slots, rollback.

## M4 — The core product: transparent RS485 ↔ TCP bridge
- [x] Each RS485 port ↔ a TCP server socket (configurable port).
- [x] Bidirectional byte shuttling with backpressure.
- [x] TCP client + UDP modes.
- [ ] **Prove it through the real transceivers** (jumper or USB-RS485 adapter).

## M5 — "Better than stock" (pick what's fun)
- [x] Modern web UI + **REST/JSON API** (vs. their 2010 iframe UI).
- [ ] **MQTT** with Home Assistant auto-discovery.
- [ ] Prometheus metrics / live per-port traffic + latency stats.
- [ ] TLS, real auth (no telnet data-port backdoor).
- [ ] Modbus TCP ↔ RTU gateway with multi-master queuing.
- [ ] Embedded scripting (Lua / MicroPython) for edge logic.

## Known risks / open questions
- Internal flash is a single 128 KB erase sector, so it holds only the loader;
  apps run from AXI SRAM (384 KB).
- RS485 direction: no GPIO in a 16-pin sweep affected any RX line, with TX high
  or low. Hypothesis: hardware auto-direction from TX. Prove with a loopback.
