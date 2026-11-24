#!/bin/sh
# Build the U8 (CH32V006, QingKe V2C = RV32EmC: multiply but no divide) firmware:
#   sh firmware/u8/build.sh   -> firmware/out/u8boot.bin (bootloader, Pico install only)
#                                firmware/out/u8app.bin  (screen firmware, tools/u8update.ps1)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
G="$HERE/../../.toolchain/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf"
[ -x "$G-gcc.exe" ] || [ -x "$G-gcc" ] || G="${RISCV_PREFIX:-riscv-none-elf}"
mkdir -p "$HERE/../out"
for t in u8boot u8app; do
  "$G-gcc" -march=rv32emc_zicsr -mabi=ilp32e -Os -nostdlib -ffreestanding -Wl,--no-warn-rwx-segments \
    -T "$HERE/$t.ld" "$HERE/$t.c" -o "$HERE/../out/$t.elf"
  "$G-objcopy" -O binary "$HERE/../out/$t.elf" "$HERE/../out/$t.bin"
  # the V2C core has only mul: div/rem/mulh* trap (gcc uses mulh for divisions by constants)
  if "$G-objdump" -d "$HERE/../out/$t.elf" | grep -qE '\s(div|rem|mulh)[su]*\s'; then echo "$t: div/rem/mulh instruction!"; exit 1; fi
  "$G-size" "$HERE/../out/$t.elf" | tail -1
done
