# Roadmap

Milestone ladder from "boots" to "better than stock." Each rung is a working,
testable state. Scope is kept deliberately narrow — we build the open/hackable
gateway we want, not a clone of USR's whole feature matrix.

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
