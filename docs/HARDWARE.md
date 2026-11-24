# Hardware notes — USR-N540

What we know about the board, how to access the stock unit, and the pin map we
still have to recover. The authoritative, machine-readable pin map is
[`src/board/board.h`](../src/board/board.h); this doc is the human context.

## Identity

- **Product:** USR-N540 (USR-N540-H7-4 variant: 4× RS485), by Jinan USR IOT / PUSR.
- **Product page:** https://www.pusr.com/products/4-port-RS485-to-ethernet-converters-usr-n540.html
- **Manual (N580/N540/N520/N510):** https://www.pusr.com/uploads/20220830/64f5a4f2044d1fedbe711044f87bf6f1.pdf

## Silicon (from chip markings)

| Ref | Part | Role |
|---|---|---|
| MCU | **STM32H750VBT6**, LQFP100, Cortex-M7 | main processor, run @ 400 MHz |
| Flash ×2 | **2× GigaDevice GD25Q16** (25Q16ESIG), SOIC-8 | 2 MB each = **4 MB** external SPI NOR (matches datasheet "128K+4M"). Likely code (QSPI XIP) + data/web, or QUADSPI dual-flash. |
| PHY | **IC Plus IP101GRI**, QFN | 10/100 **RMII Ethernet PHY** |
| Magnetics | **OUYCORE FC1680C** | 10/100 LAN transformer (PHY ↔ RJ45) |
| RS485 ×4 | **TI SN65HVD75**, SOIC-8 | 3.3 V half-duplex RS485 transceiver (DE pin3 / RE̅ pin2) |
| Xtal ×2 | 25.000 MHz | one = MCU HSE; one = IP101GRI reference (PHY makes 50 MHz RMII clock internally) |

Internal flash is only 128 KB — the stock app lives in external flash and is
loaded by a small internal bootloader. See ROADMAP M0.

## Accessing the stock unit (for reference / config capture)

- **Web UI:** `http://<ip>/` — HTTP Basic auth, default **admin / admin**.
  Server is "webnet 2.0.0" (flaky under scripted access; use a browser).
- **Telnet (port 23):** this is a **serial DATA socket** (RS485 Port 1 default
  TCP server), *not* a shell. The N540 is bare-metal — no OS, no filesystem.
- **UDP discovery/config:** broadcast on **port 1901**, keyword `FF010102`
  (from USR EthernetTool `broadcast.json`).
- **Firmware flashing (stock):** USR tools (USR-M4.exe) push a `.bin` over a
  TFTP-style upload (`firmware.bin`/`octet`) with a hardware-match header.
  → The vendor path rejects non-USR images, so **our firmware needs SWD**.
- Flash region sizes hinted by EthernetTool `config.json`: param ≤ 52 KB,
  web ≤ 300 KB.

## Debug headers (from board photos)

- **5-pin header** (pin-1 arrow): almost certainly **SWD** —
  SWDIO/SWCLK/NRST/GND/3V3 in some order. Buzz out with a multimeter
  (SWDIO=PA13, SWCLK=PA14 on the MCU; GND to ground plane; 3V3 to a cap).
- **`T R G` 3-pin header:** **TX / RX / GND** UART console (3.3 V TTL).

## Pin map — TO RECOVER (fill `board.h`)

Everything below is a **placeholder guess** until the dump + continuity test.

| Function | Signal | Guess | Confirmed? |
|---|---|---|---|
| Heartbeat LED | GPIO | PE2 | ❌ TODO(dump) |
| RS485 #1 | USART1 TX/RX/DE | PA9 / PA10 / PA8 | ❌ |
| RS485 #2 | USART3 TX/RX/DE | PD8 / PD9 / PD10 | ❌ |
| RS485 #3 | UART4 TX/RX/DE | PC10 / PC11 / PA15 | ❌ |
| RS485 #4 | USART6 TX/RX/DE | PG14 / PG9 / PG12 | ❌ |
| Ethernet | RMII (AF11) | standard RMII pins | ❌ |
| Eth PHY addr | MDIO | 0x01 (strap default) | ❌ |
| Eth PHY model | — | **IC Plus IP101GRI** | ✅ |
| RS485 xcvr | — | **TI SN65HVD75** ×4 | ✅ |

### How we'll recover it
1. **Firmware dump** (SWD internal + CH341A external) → disassemble / inspect
   the RCC + GPIO AF config and ETH init to read pins directly.
2. **Continuity testing** — buzz MCU pins to transceivers, PHY, LEDs.
3. **Scope/logic probe** — confirm which UART is live per RS485 port.
