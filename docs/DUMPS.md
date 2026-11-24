# Firmware dumps

Backups of the stock USR-N540 firmware. **Binaries are gitignored** (`dumps/`) —
they live on this machine and on `swd-host:~/n540_dumps/`. Keep a durable copy.

## Go/no-go: RDP is OPEN ✅

`FLASH_OPTSR_CUR` (`0x5200201C`) read `0x1bc6aaf0` → RDP byte `0xAA` = **Level 0**.
Chip ID `0x20036450` = STM32H750 (dev `0x450`), **silicon rev V** (480 MHz-capable).
SWD is fully open: we can read everything *and* flash our own firmware.

## What we have

| File | Source | Size | Status |
|---|---|---|---|
| `internal_bootloader.bin` | internal flash `0x08000000` | 128 KB | ✅ complete, authoritative |
| `qspi_flash1_2MB.bin` | QSPI flash #1, mem-mapped `0x90000000` | 2 MB | ⚠️ 15/16 (see gap) |

**Gap:** `0x90160000–0x9017FFFF` (128 KB, "chunk 11") is `0xFF`-filled — it faults
reliably on SWD memory-mapped reads at any speed. Likely an unused/special window
(it sits between app content and the empty `0xFF` tail). Top up via CH341A if it
turns out to be real content.

**Not yet dumped:** the **second GD25Q16** (2 MB). It is *not* in the `0x90000000`
memory-mapped window, so SWD can't reach it — needs the **CH341A + SOIC-8 clip**
(hold the MCU in reset so it releases the SPI bus).

## How the dump was done (SWD via swd-host)

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
