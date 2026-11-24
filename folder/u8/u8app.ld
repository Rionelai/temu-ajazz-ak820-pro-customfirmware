/* U8 screen firmware: after the 2 KB bootloader, up to the end of the 62 KB flash.
   .data is copied from flash and .bss zeroed by _start. Stack top 0x20001FF0 (boot magic above). */
ENTRY(_start)
MEMORY {
  FLASH (rx) : ORIGIN = 0x00000800, LENGTH = 60K
  RAM (rw)   : ORIGIN = 0x20000000, LENGTH = 8K
}
SECTIONS {
  .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) *(.srodata*) . = ALIGN(4); } > FLASH
  .data : ALIGN(4) { _sdata = .; *(.data*) *(.sdata*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
  _sidata = LOADADDR(.data);
  .bss (NOLOAD) : ALIGN(4) { _sbss = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  /DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
