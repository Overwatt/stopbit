# Firmware dumps

Backups of the stock USR-N540 firmware. **Binaries are gitignored** (`dumps/`) —
they live on this machine and on `swd-host:~/n540_dumps/`. Keep a durable copy.

## Go/no-go: RDP is OPEN ✅

`FLASH_OPTSR_CUR` (`0x5200201C`) read `0x1bc6aaf0` → RDP byte `0xAA` = **Level 0**.
Chip ID `0x20036450` = STM32H750 (dev `0x450`), **silicon rev V** (480 MHz-capable).
SWD is fully open: we can read everything *and* flash our own firmware.

## What we have

| File | Source | Status |
|---|---|---|
| `extflash0.bin` | QSPI bank 2 chip (stock app) | 2 MB, read twice, identical. SHA-256 `2d1ed206…` |
| `extflash1.bin` | SPI4 chip, CS=PE4 (stock config, littlefs) | 2 MB, read twice, identical. SHA-256 `c4542dc8…` |
| `internal_bootloader.bin` | internal flash, via SWD | 128 KB, single read, not cross-checked |
| `qspi_flash1_2MB.CORRUPT-swd.bin` | SWD memory-mapped read | **Bad**: dropped and bit-shifted words in 10 of 16 chunks |

The external dumps were read by our own loader over the network.

## How the SWD dump was done (superseded for external flash)

ST-Link/V2 is on **swd-host** (Ubuntu). openocd 0.12 installed there. Drive remotely:

```sh
ssh swd-host 'sudo openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "adapter speed 2000" -c "gdb_port disabled" -c "tcl_port disabled" \
  -c "telnet_port disabled" -c "init" -c "<commands>" -c "shutdown"'
```

SWD header pinout (verified): `1=3V3("V"), 2=GND, 3/4=SWDIO/NRST, 5=SWCLK`.
Connect-under-reset was *not* needed — plain connect works (firmware doesn't lock SWD).

**Dumping XIP QSPI is finicky** (the CPU runs code from the same flash):
- Internal flash + RDP read: `reset halt` then `dump_image`/`mdw` — easy.
- QSPI (`0x90000000`): must attach to the *running* firmware with `halt` (NOT
  `reset halt`, which deconfigures the QSPI mapping).
- A bus fault **poisons the AP for the rest of the session**, so dump **one chunk
  per fresh openocd session**, with a **~2 s settle** after killing the previous
  openocd. Hot code/data regions fault unless caught cleanly this way.
- Working recipe: 16 × 128 KB chunks, fresh session each, retry short ones.

## What the dumps told us

- **OS:** RT-Thread + LwIP 2.1.0 + mbedTLS 2.7.10 + usr_mqtt + SNMP + Modbus +
  websocket. FinSH/msh shell present (`list_device`, `list_if`, `version`, …).
- Internal 128 KB = the **"web_boot" networked bootloader** (build tree
  `G:\H7-2\H7-2-iteration\H7-2-web_boot\...`); main app runs XIP from QSPI.
- App build stamp seen: `2024-03-06`.
- Next: disassemble (Ghidra) to extract the GPIO/RCC/ETH/QSPI pin config → fill
  `board.h`. The RT-Thread BSP pin tables are the target.
