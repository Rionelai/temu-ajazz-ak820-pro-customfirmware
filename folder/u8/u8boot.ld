/* U8 bootloader: first 2 KB of U8's flash (code at the 0x0 alias, like ch32fun for V00x).
   Only .text/.rodata and one .bss buffer (no .data: nothing to copy at start). */
ENTRY(_start)
MEMORY {
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 2K
  RAM (rw)   : ORIGIN = 0x20000000, LENGTH = 8K
}
SECTIONS {
  .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) *(.srodata*) } > FLASH
  .bss (NOLOAD) : { *(.bss*) *(.sbss*) *(COMMON) } > RAM
  /DISCARD/ : { *(.data*) *(.sdata*) *(.comment) *(.riscv.attributes) }
}
