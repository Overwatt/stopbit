<img src="docs/images/logo.svg" width="96" alt="">

# Open firmware for the USR-N540

Firmware for the **USR-N540**, a 4-port RS485-to-Ethernet gateway, that replaces the
vendor's closed one. Bare-metal C on an STM32H750, with lwIP. A for-fun project: open,
hackable and observable, not a copy of the vendor's whole feature list.

Not made or endorsed by the vendor.

![The Status page: a verdict, a list of parts, and one port's report](docs/images/status.png)

*Example data.*

## What it does

- **RS485 to network.** Each port is a TCP server, TCP client or UDP endpoint, with its own
  line settings, 300 to 2,000,000 baud.
- **Status page.** A verdict that names the fault, every part's state, and each port's last
  ten minutes of traffic. Works on a phone, in light and dark, with no internet.
- **Listen only.** A port's transmitter can be turned off in hardware setup, so watching a
  live bus cannot disturb it.
- **Alert if quiet.** Per port: silence, or no client, for a chosen time.
- **MQTT,** with Home Assistant discovery that can be turned off.
- **Updates over the network,** from the web page or the command line, into two slots. An
  update that does not start is undone by the loader.
- **Console** (`tools/n540.py HOST`): the same settings, plus send, watch and traffic logs.
- **Reload button:** hold 3 to 15 seconds to get back to DHCP with no web password.

## How far it has been tested

On one gateway:

- All four ports with a USB-RS485 adapter, 9600 to 1,000,000 baud, both ways and
  request/reply, with no errors.
- A real bus at 38,400 baud: every packet checksum valid in each 30-second capture.
- Update, rollback and refusal of bad images.

Not tested: several ports busy at once, long or terminated buses, more than 1,000,000 baud,
power loss during an update, and silence on the wire in listen only (checked by counters,
not measured). [GATEWAY.md](docs/GATEWAY.md) and [NETWORK-UPDATE.md](docs/NETWORK-UPDATE.md)
have the details.

## Security, plainly

- Use it on a network you trust. Pages are HTTP, and **the ports' data sockets have no
  login**. A port can be limited to listed addresses, to read or to read and write; that
  goes by IP address, so it is not proof against someone who takes one over.
- Anyone on the network can view the pages. Changes need the web password, which is unset
  at first, or the update key. Neither crosses the network in clear.
- **The web password can install firmware**, which is full control. Replacing the loader and
  reading flash need the update key.
- HTTPS can be turned on ([HTTPS.md](docs/HTTPS.md)); plain HTTP stays available beside it.
- MQTT is unencrypted, and the gateway sends the broker's login in clear. With changes
  allowed over MQTT, anyone who can publish to the broker can make them.

## Build

Needs CMake 3.20+, Ninja, Python 3, git and Arm's GCC toolchain **with newlib**. Homebrew's
`arm-none-eabi-gcc` lacks it; unpack [Arm's](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
into `toolchain/` and CMake finds it. ST's HAL and CMSIS, and lwIP, are fetched at pinned
versions on the first configure.

```sh
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build
```

| Output | |
|---|---|
| `build/app.bin` | the gateway |
| `build/loader_flash.bin` | the loader, for internal flash |
| `build/loader_ram.bin` | the same loader, run from RAM over SWD |

The first configure creates `secrets/n540.key`, the update key, and compiles it into both.
**Back it up:** without it, only SWD can replace the loader.

## Install

**Already running this firmware:** choose `app.bin` on the Firmware page, or

```sh
tools/n540.py n540-xxxxxx.local update build/app.bin
```

The name comes from the gateway's MAC address. Then set the web password with
`tools/n540.py HOST passwd`.

**From the vendor's firmware** needs an ST-Link once. Read these first:

- It has been done on one unit. It erases the vendor's bootloader, and kept firmware later
  overwrites the rest. **Going back is unproven** and there is no tool for it: our backup of
  that bootloader is a single unverified read. Take your own
  backups and check them ([DUMPS.md](docs/DUMPS.md)).
- **An external watchdog resets the chip about every 1.6 s** unless PC10 toggles, even while
  halted, so long SWD flash writes get cut off and can leave the unit unbootable. Do not
  flash it over SWD directly.

Instead, start the loader from RAM, which writes nothing, and let it install itself:

```sh
tools/ramrun.sh HOST_WITH_STLINK          # OpenOCD over ssh; read the script first
tools/n540.py n540-xxxxxx.local selfupdate build/loader_flash.bin
tools/n540.py n540-xxxxxx.local update build/app.bin
```

## Hardware

| | |
|---|---|
| Processor | STM32H750VBT6, Cortex-M7 at 400 MHz |
| Flash | 128 KB internal (the loader), 2 × 2 MB GD25Q16 (firmware slots, kept firmware, settings) |
| Ethernet | IC Plus IP101G, 10/100 |
| RS485 | 4 × TI SN65HVD75 |
| Watchdog | UM706SS, external |

The app runs from RAM: internal flash is one 128 KB erase block, so it holds only the
loader. Pins, and how sure we are of each, are in [HARDWARE.md](docs/HARDWARE.md).

## Layout

```
src/loader/   boot, network update, slot choice
src/app/      ports, settings, MQTT, console, API
src/http/     web server shared by both
src/net/      Ethernet driver, lwIP glue
src/common/   flash, slots, watchdog, SHA-256
src/board/    the pin map
web/          the page, one file, built into the firmware
tools/        n540.py (updates, console), SWD scripts
tests/        host and on-device tests
docs/         GATEWAY, HARDWARE, NETWORK-UPDATE, DUMPS, HTTPS, ROADMAP
```

[PRODUCT.md](PRODUCT.md) and [DESIGN.md](DESIGN.md) record what the pages are for and how
they look. Plans are in [ROADMAP.md](docs/ROADMAP.md).

## License

[Apache 2.0](LICENSE)
