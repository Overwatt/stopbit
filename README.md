<img src="docs/images/logo.svg" width="96" alt="">

# lino — open firmware for the USR-N540

Custom, open firmware for the **USR-N540** (4-port RS485 ↔ Ethernet gateway),
built on bare-metal **STM32H750VBT6** with ST's HAL and LwIP.

This is a for-fun project to replace the closed vendor firmware with something
open, hackable, observable, and tailored — not to re-implement USR's entire
product. See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the plan and
[`docs/HARDWARE.md`](docs/HARDWARE.md) for what we know about the board.

## Hardware (from chip markings + pusr.com)

| | |
|---|---|
| MCU | STM32H750VBT6 (Cortex-M7, LQFP100), run at 400 MHz |
| Internal flash | 128 KB @ `0x08000000` |
| External flash | GigaDevice GD25Q16 (2 MB SPI NOR) + a second GD SOIC-8 |
| Clock | 25 MHz HSE crystal |
| I/O | 4× RS485 (half-duplex), 1× 10/100 Ethernet (external PHY, model TBD) |
| Debug | SWD (5-pin header), `T R G` 3-pin UART console |

> **Pin map is not reverse-engineered yet.** Every pin lives in
> [`src/board/board.h`](src/board/board.h), marked `TODO(dump)`. Those values
> are placeholder guesses. Correcting that one file after the firmware dump is
> what makes this build real on the hardware.

## Toolchain

Everything needed is already standard: `arm-none-eabi-gcc`, `cmake` (≥3.20),
`ninja`, `git`, and `openocd` (with an ST-Link) for flashing. The ST
HAL/CMSIS are fetched automatically by CMake (pinned versions) on first
configure.

## Build

```sh
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build
```

Outputs: `build/lino-n540.elf`, `.bin`, `.hex`, plus a size report and `.map`.

## Flash (ST-Link + OpenOCD)

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "program build/lino-n540.elf verify reset exit"
```

> ⚠️ **Do not flash until we have a verified backup of the stock firmware.**
> Flashing wipes the vendor bootloader in internal flash. The RDP level must be
> checked first (see ROADMAP milestone 0). This repo's `.gitignore` keeps
> `dumps/` out of git — store backups there, and copy them somewhere durable.

## Layout

```
cmake/      toolchain file
linker/     STM32H750 linker script (memory map)
src/board/  board.h — THE pin map (single source of truth)
src/core/   boot, clock, HAL config, interrupt handlers, main
src/drivers/ led, rs485 (more to come: eth, phy)
src/app/    gateway logic (later milestones)
docs/       ROADMAP.md, HARDWARE.md
```

## Status

Milestone 1 (boot → 400 MHz → heartbeat LED → 4× RS485 echo) scaffolded.
Pin map pending the bench dump. Ethernet/LwIP/gateway are later milestones.
