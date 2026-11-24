# ajazz-ak820pro-ch584-revive

Bring a **bricked AJAZZ AK820 PRO** back to life - the newer board revision sold on Temu
(PCB `SG-8975US-ARGB-1K`, `SG-00721 Ver:02`, main chip marked `HFDCKD101`), with an
open-source replacement firmware.

> Not affiliated with AJAZZ. No original AJAZZ firmware is included or needed.
> You open your keyboard and solder at your own risk.

## The short story

The keyboard died after the official **2.4G upgrade tool** was run with the keyboard connected
by USB: that tool only contains the **dongle** firmware and wrote it onto the keyboard. Afterwards
the keyboard was completely dead (no USB device at all).

What we found:

- The "HFD" chips are rebranded WCH parts: **U7 = WCH CH584** (main MCU), **U8 = WCH CH32V006**.
- U7 has a **ROM bootloader** that a bad update cannot erase. You reach it by holding pin 26
  (PB22) at GND while plugging in. On our board a **factory solder bridge** between pins 25/26
  blocked this; after removing it the bootloader appears every time.
- With [wchisp](https://github.com/ch32-rs/wchisp) you can flash U7 freely (its original firmware
  is read-protected and is not published anywhere, so we wrote our own).

## Status

| Feature | State |
|---|---|
| Recovery path (bootloader + flashing) | ✅ works |
| Key finder firmware (maps keys over a USB serial port) | ✅ works |
| Replacement firmware: keys over USB, knob = volume, knob press = mute | 🚧 builds; keymap being mapped |
| Snap Tap (SOCD) on A/D, Fn+S toggles | 🚧 in v1, untested |
| Web settings page (WebHID) | planned |
| RGB lighting, Bluetooth, screen, 2.4G dongle | planned |

## Quick start

1. Read **[docs/RECOVERY.md](docs/RECOVERY.md)** (solder bridge check, needle trick, driver).
2. Build: `sh firmware/build.sh keyboard` (needs git + xPack `riscv-none-elf-gcc`; the WCH SDK
   is fetched automatically). Prebuilt `.hex` files will be on the Releases page.
3. Flash: `.\tools\flash_u7.ps1 -Wchisp C:\path\to\wchisp.exe`, then do the needle trick.

## Repo layout

```
firmware/
  build.sh            build a target: keyboard | keyfinder
  common/             CH584 startup, linker script (96K RAM), USB CDC stack (WCH-derived)
  keyboard/           replacement keyboard firmware (keyboard.c, usb_hid.c, keymap.h)
  keyfinder/          key finder: reports pin pairs / knob over a USB serial port
tools/
  flash_u7.ps1        wait for the bootloader and flash with wchisp
  kf_map.ps1          guided key mapping -> keymap.csv
  gen_keymap.py       keymap.csv -> firmware/keyboard/keymap.h
  kf_serial.ps1       raw key-finder log
  usb_watch.ps1       beep when the bootloader (or anything) shows up on USB
docs/
  HARDWARE.md         chips, pins, config, what the Ajazz tools contain
  RECOVERY.md         step-by-step recovery
research/
  u8-probe/           Pi Pico as WCH single-wire debugger for U8 (picorvd patch, RAM programs)
  dongle-firmware/    tools that unpacked the 2.4G tool (the firmware itself is not included)
```

## Credits

- [fpb/ajazz-ak820-pro](https://github.com/fpb/ajazz-ak820-pro) - the older SN32 board, where this started
- [ch32-rs/wchisp](https://github.com/ch32-rs/wchisp), [openwch/ch585](https://github.com/openwch/ch585),
  [aappleby/picorvd](https://github.com/aappleby/picorvd)
