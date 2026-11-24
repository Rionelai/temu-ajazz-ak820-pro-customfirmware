#!/bin/sh
# Build firmware for the AK820 Pro main chip U7 (WCH CH584).
#   sh firmware/build.sh keyboard    -> out/keyboard.hex   app at 0x2000 (installed via the IAP)
#   sh firmware/build.sh keyfinder   -> out/keyfinder.hex  app at 0x2000
#   sh firmware/build.sh iap         -> out/iap.hex        8 KiB USB updater at 0x0
#   sh firmware/build.sh full        -> out/full.bin       iap + keyboard, for the first install
#                                       with wchisp through the ROM bootloader (needle trick)
# Needs: git, python, and a RISC-V GCC (xPack riscv-none-elf-gcc 15.x tested). Set RISCV_PREFIX if
# it is not on PATH, e.g. RISCV_PREFIX=/c/tools/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf
# The WCH CH585/CH584 SDK (openwch/ch585) is cloned into firmware/sdk on first use; it is not
# part of this repo.
set -e
TARGET="${1:-keyboard}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SDKROOT="$HERE/sdk/ch585"
SDK="$SDKROOT/EVT/EXAM/SRC"
LOCALGCC="$HERE/../.toolchain/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf"
if [ -n "$RISCV_PREFIX" ]; then B="$RISCV_PREFIX"
elif [ -x "$LOCALGCC-gcc.exe" ] || [ -x "$LOCALGCC-gcc" ]; then B="$LOCALGCC"   # repo-local .toolchain/
else B="riscv-none-elf"; fi
[ -d "$SDKROOT" ] || git clone --depth 1 https://github.com/openwch/ch585 "$SDKROOT"

if [ "$TARGET" = "full" ]; then
  sh "$0" iap || exit 1
  sh "$0" keyboard || exit 1
  python -I - "$HERE/out/iap.bin" "$HERE/out/keyboard.bin" "$HERE/out/full.bin" <<'EOF'
import sys
iap, app, out = (open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read(), sys.argv[3])
assert len(iap) <= 0x2000, f'IAP is {len(iap)} bytes, more than 8 KiB'
open(out, 'wb').write(iap + b'\xff' * (0x2000 - len(iap)) + app)   # app linked at 0x2000
print(f'full image: iap {len(iap)} B + keyboard {len(app)} B at 0x2000 -> {out}')
EOF
  exit 0
fi
[ -d "$HERE/$TARGET" ] || { echo "unknown target: $TARGET"; exit 1; }

LD="$HERE/common/link_app.ld"; STARTUP="$HERE/common/startup.S"
[ "$TARGET" = "iap" ] && { LD="$HERE/iap/link_iap.ld"; STARTUP="$HERE/iap/startup_iap.S"; }

CFLAGS="-march=rv32imac_zicsr_zifencei -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore -Os \
 -ffunction-sections -fdata-sections -fno-common -DINT_SOFT \
 -I$HERE/$TARGET -I$HERE/common/inc -I$HERE/common -I$SDK/StdPeriphDriver/inc -I$SDK/RVMSIS -w"
OBJ="$HERE/out/obj-$TARGET"
rm -rf "$OBJ"; mkdir -p "$OBJ"

SRCS=""
for f in "$HERE/$TARGET"/*.c; do SRCS="$SRCS $f"; done
# The finders report over a USB serial port; the keyboard has its own HID USB stack.
case "$TARGET" in keyfinder|ledfinder|u8link) SRCS="$SRCS $HERE/common/usb_cdc.c";; esac
# Bluetooth (keyboard, bletest): WCH's BLE HAL + HID profiles + libCH58xBLE straight from the SDK.
# bletest also takes the example app (types test letters); keyboard has its own ble.c instead.
LIBS=""; ASMS=""
if [ "$TARGET" = "bletest" ] || [ "$TARGET" = "keyboard" ]; then
  BLE="$SDKROOT/EVT/EXAM/BLE"
  for f in "$BLE"/HAL/*.c "$BLE"/HID_Keyboard/Profile/*.c; do
    # keyboard has its own hidkbdservice.c (report IDs + consumer report for the knob) and
    # battservice.c (battery level may rise again)
    case "$TARGET:$(basename "$f")" in keyboard:hidkbdservice.c|keyboard:battservice.c) continue;; esac
    SRCS="$SRCS $f"
  done
  [ "$TARGET" = "bletest" ] && for f in "$BLE"/HID_Keyboard/APP/*.c; do SRCS="$SRCS $f"; done
  CFLAGS="$CFLAGS -I$BLE/HAL/include -I$BLE/HID_Keyboard/APP/include -I$BLE/HID_Keyboard/Profile/include -I$BLE/LIB"
  LIBS="-L$BLE/LIB -lCH58xBLE"
  # The real LLE_IRQHandler / Ecall handlers: without this file the startup's weak default
  # handler (an endless loop) wins and the chip hangs on the first radio interrupt.
  ASMS="$BLE/LIB/ble_task_scheduler.S"
  CFLAGS="$CFLAGS -DINTSYSCR_VAL=0x3"   # HW stacking + nesting on, like WCH's own BLE projects
  # keyboard: 64-byte settings reports over BLE need ATT MTU >= 67 (default 27-byte buffers = MTU 23)
  [ "$TARGET" = "keyboard" ] && CFLAGS="$CFLAGS -DBLE_BUFF_MAX_LEN=251 -DBLE_MEMHEAP_SIZE=(1024*12)"
fi
# The app targets share the IAP's DataFlash flag helper.
[ "$TARGET" != "iap" ] && SRCS="$SRCS $HERE/common/iap_flag.c"
for f in "$SDK"/StdPeriphDriver/CH58x_*.c; do
  case "$f" in *usbhost*) ;; *) SRCS="$SRCS $f";; esac
done

OBJS=""
for f in $SRCS; do
  o="$OBJ/$(basename "$f" .c).o"
  extra=""
  # usb_cdc.c / usb_hid.c are derived from WCH's USB example, whose own main() is renamed away
  case "$(basename "$f")" in usb_cdc.c|usb_hid.c) extra="-Dmain=wch_example_main";; esac
  $B-gcc $CFLAGS $extra -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
$B-gcc $CFLAGS -x assembler-with-cpp -c "$STARTUP" -o "$OBJ/startup.o"
for f in $ASMS; do
  $B-gcc $CFLAGS -x assembler-with-cpp -c "$f" -o "$OBJ/$(basename "$f" .S).o"
  OBJS="$OBJS $OBJ/$(basename "$f" .S).o"
done
rm -f "$HERE/out/$TARGET.elf" "$HERE/out/$TARGET.hex" "$HERE/out/$TARGET.bin"
if ! $B-gcc -march=rv32imac_zicsr_zifencei -mabi=ilp32 -nostartfiles -Wl,--gc-sections \
  -T "$LD" --specs=nano.specs --specs=nosys.specs \
  -o "$HERE/out/$TARGET.elf" "$OBJ/startup.o" $OBJS $LIBS -L"$SDK/StdPeriphDriver" -lISP585 \
  -Wl,-Map="$HERE/out/$TARGET.map" > "$OBJ/link.log" 2>&1; then
  cat "$OBJ/link.log"; echo "LINK FAILED"; exit 1
fi
grep -v 'is not implemented and will always fail\|does not take linker garbage' "$OBJ/link.log" || true
$B-objcopy -O ihex "$HERE/out/$TARGET.elf" "$HERE/out/$TARGET.hex"
$B-objcopy -O binary "$HERE/out/$TARGET.elf" "$HERE/out/$TARGET.bin"
$B-size "$HERE/out/$TARGET.elf"
echo "-> $HERE/out/$TARGET.hex"
