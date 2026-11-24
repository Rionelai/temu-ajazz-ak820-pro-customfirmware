# Recovery guide: bricked AK820 PRO (board `SG-8975US-ARGB-1K Ver:02`)

Symptoms: no lights, black screen, Windows shows nothing at all when you plug it in
(not even "Unknown USB device"). Typical cause: the **2.4G upgrade tool** was run with the
keyboard (not the dongle) connected by USB, or a firmware update was interrupted.

You need: a soldering iron, flux + solder wick, a thin wire, a sewing needle, a Windows PC.
No programmer is needed for the main chip.

## 1. Open it and check your board

Remove the keycaps/switches, open the case, and look at the back of the PCB. This guide is for
`SG-8975US-ARGB-1K` / `SG-00721 Ver:02` with chips `HFDCKD101` (U7) and `HFD8K1000` (U8).
See [HARDWARE.md](HARDWARE.md) for photos-worth of details.

## 2. Check for the solder bridge on U7 pins 25/26

Hold the board so crystal `Y1` is below U7. On U7's bottom edge (facing `Y1`), count from the
left corner: pin 25, pin 26. With a magnifier, check they are **not** joined by solder; with a
multimeter in continuity mode they must **not** beep. If they are bridged: flux, fine solder wick,
1-2 s, lift straight up.

## 3. Enter the ROM bootloader (the "needle trick")

1. Solder a thin wire to the `GND3` dot next to `Y1`; attach a sewing needle to its other end.
2. Keyboard unplugged, back switch on **wired**.
3. Hold the needle on **U7 pin 26 (PB22)**: the 2nd pin from the left corner on the `Y1` side.
4. Plug in USB. After ~1 s Windows shows a new device `USB\VID_1A86&PID_55E0`. Lift the needle;
   the bootloader stays ~15-20 s.

`tools/usb_watch.ps1` beeps and prints the device when it appears.

## 4. Driver and flasher

- Install WCH's official driver **CH372DRV** (<https://www.wch-ic.com/downloads/CH372DRV_ZIP.html>).
- Get **wchisp nightly** (<https://github.com/ch32-rs/wchisp/releases/tag/nightly>) - the v0.3.0
  release only works with WinUSB; the nightly also works with the WCH driver.
  Put `C:\Windows\System32\CH375DLL64.DLL` next to `wchisp.exe`.
- Test (read-only): enter the bootloader, then `wchisp info` → should print
  `Chip: CH584[0x8416] (Code Flash: 448KiB ...)`.

## 5. Flash the replacement firmware

```powershell
.\tools\flash_u7.ps1 -Hex .\firmware\out\keyboard.hex -Wchisp C:\path\to\wchisp.exe
```

Run it, then do the needle trick; it flashes, verifies and resets. The ROM bootloader can't be
overwritten, so you can always repeat this.

## 6. (Only if your keys are wired differently) map your keys

Flash `firmware/out/keyfinder.hex` instead, then run `tools\kf_map.ps1`. It asks for each key;
bridge that key's hot-swap socket (two metal tabs) with tweezers. It writes `keymap.csv`.
Then: `python tools\gen_keymap.py` and rebuild (`sh firmware/build.sh keyboard`).
