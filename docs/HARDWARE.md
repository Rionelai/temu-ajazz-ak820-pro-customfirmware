# Hardware notes: AJAZZ AK820 PRO, PCB `SG-8975US-ARGB-1K` `SG-00721 Ver:02`

Everything here was measured on one board (Temu, box label batch `980879`,
serial `SGAK820PRORGBGY…`, PCB printed `8411-8975-0005` / `2026/08/24`). Other batches may differ.

This is **not** the board covered by [fpb/ajazz-ak820-pro](https://github.com/fpb/ajazz-ak820-pro)
(that one has an 80-pin HFD80CP100 = SONiX SN32F299 and a separate CH582F Bluetooth module).
None of that repo's firmware or bootloader tricks apply here.

## Chips

| Ref | Marking | What it really is | Notes |
|---|---|---|---|
| U7 | `HFDCKD101` (HFD logo) | **WCH CH584M** (QFN48, RISC-V, BLE/2.4G, USB full-speed) | main MCU: key matrix, knob, USB, radio. Chip ID `0x8416`, 448 KiB flash, ROM bootloader V2.30 |
| U8 | `HFD8K1000` | **WCH CH32V006** (QingKe V2C RV32EC, 62 KiB flash, 8 KiB RAM) | helper chip, link to U7 via `TX1`/`RX1`; read-protected; **not** needed for keys |
| U9 | `PUYA P25Q…` | SPI flash | screen assets |
| Y1 | `32.000` | 32 MHz crystal for U7 | |
| Y2 | `24.000` | 24 MHz crystal for U8 | |
| ANT1 | chip antenna | 2.4 GHz | |

"HFD" parts are rebranded WCH silicon; Ajazz's own updater is literally labelled `WCH@HFD`.

## U7 (CH584M) pins that matter

QFN48, pin 1 dot next to the coil `L2`; the side facing crystal `Y1` holds pins 25-36
(PB23, PB22, PB21, …). See the CH585/CH584 datasheet (`openwch/ch585`, `Datasheet/`).

| Pin | GPIO | Use on this board |
|---|---|---|
| 26 | PB22 | **BOOT**: hold at GND while plugging in USB to enter the ROM bootloader |
| 25 | PB23 | reset-capable pin, right next to 26 (corner pin) |
| 15/16 | PB11/PB10 | USB D+ / D- (full speed; the CH584 has no high-speed USB) |
| - | PB0/PB1 | volume knob encoder (A/B), pull-ups, both contacts change once per click |
| - | PA7, PA8, PA9, … | key matrix (exact map: `keymap.csv`) |
| - | PA11-PA15, PB5, PB6, … | key matrix |
| - | PA0 | **RGB LED chain data** (LED0), see "Per-key RGB" |
| - | PB20-PB23 | held high by U8 (pull-up/pull-down both read 1); go high ~20 s after boot (leave alone) |

Config read with `wchisp info` (factory state):

```
CFG_RESET_EN 0  (PB23 not used as reset)
CFG_DEBUG_EN 0  (2-wire debug off)
CFG_BOOT_EN  1  (ROM bootloader enabled)
CFG_ROM_READ 0  (code read-out protected)
DOWNLOAD_CFG PB22 (default)
```

## Per-key RGB

One addressable chain on **U7 PA0**, idle low, colour byte order G,R,B, **Esc is the first LED**,
F1 the second, then the keys in order. There is a pulse-stretching stage between PA0 and the LEDs
(PA0 reads low even with its pull-up on), so normal WS2812 timing shows **all white**. Timing that
works: **"0" = 100 ns high, "1" = 1000 ns high, 2.5 us per bit**; anything from ~200 ns high reads
as "1". Driver: `firmware/keyboard/rgb.c`.

## Known factory defect: solder bridge on U7 pins 25/26

Our board had a tiny **solder bridge between U7 pin 25 (PB23) and pin 26 (PB22)**. With it,
pulling PB22 low for the bootloader also pulls PB23, and the bootloader never appears.
After removing it with flux + solder wick, the bootloader shows up every time.
**Check this first** if the needle trick does nothing.

## U8 (CH32V006) findings

Reached with a Raspberry Pi Pico as a WCH single-wire debugger (`picorvd`, patched; see
`research/u8-probe/`) on pad `SWDIO1` (+ `GND5`, 1 kΩ pull-up to 3.3 V on the Pico side):

- DMSTATUS `0x004c0c82`, misa `0x40801014` (RV32EMC), marchid `0xdc68d843` (QingKe V2C),
  chip ID `0x00620620` @ `0x1FFFF704`, flash size `0x3E` (62 KiB)
- option bytes `08f700ff`: RDPR = `FF` → **read-protected**; flash reads fail even from code
  running in RAM
- does **not** drive the RGB LEDs (U7 PA0 does, see "Per-key RGB")
- driving its PA4 resets it
- QingKe V2C = RV32EmC: hardware multiply only, **no divide** (a `div` traps -> reset); build
  RAM programs with `-march=rv32emc_zicsr -mabi=ilp32e` and avoid `/` and `%`
- the stock U8 firmware leaves every clock off (RCC APB2PCENR = 0, all GPIOs at reset state):
  it never initialises the screen, which is why the screen stays dark with our U7 firmware

### Screen + SPI flash (on U8, found 2026-11-13, research/u8-probe/flashscan.c, lcdscan.c)

| Signal | U8 pin | Notes |
|---|---|---|
| SPI SCK | PC5 | shared LCD + flash (CH32V006 SPI1 default pins) |
| SPI MOSI | PC6 | shared; GC9107 SDA |
| flash MISO | PC7 | PUYA flash, JEDEC manufacturer 0x85 |
| flash CS | PD2 | |
| LCD CS | **PC4** | VERIFIED: picture shows |
| LCD DC | **PD0** | VERIFIED |
| LCD RST | **PC3** | VERIFIED: LOW = LCD stops answering (lcdpins.c, automatic) |
| LCD backlight | **PC0** | VERIFIED: active HIGH, blinks with PC0 alone (lcdtest -DBLINK_ONE=16) |

`lcdtest.c` (minimal init: 01, FE, EF, 11, 3A=05, 36=00, 29, window 0..131) draws a picture
from U8 RAM through the Pico probe (`tools/u8probe.ps1 -Load`).

## What the original Ajazz tools contain

- `AJAZZ AK820Pro` driver V1.0.0.5 (same for batch 980879 on a-jazz.com): settings only, no
  firmware; update URL in `config.xml` is empty; looks for USB `0C45:8009/800A`.
- `2.4G_upgrade.exe` = `HFD_IAPTool`: embeds **only the 2.4G dongle firmware** (XOR-obfuscated
  Intel HEX, CH58x BLE lib, "2.4G Dongle", base 0x1000). **Running it with the keyboard plugged
  in by USB writes this dongle firmware onto the keyboard and bricks it** - this is what
  happened to our board. `research/dongle-firmware/` has the tools that unpacked it.
- Ajazz web drivers (`ajazz.driveall.cn`, `ajazz.shinetek-ic.net`): the first has an OTA
  feature but an empty `update.json`; the second hosts firmware for SF09-based AK820 variants
  (USB `1A2C:A128`), a different chip family.
