/* Everything lives in U8's SRAM at 0x20000400 (mailbox at 0x20000300, stack top 0x20001F00). */
ENTRY(_start)
SECTIONS {
  . = 0x20000400;
  .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) *(.srodata*) }
  .data : { *(.data*) *(.sdata*) }
  .bss  : { *(.bss*) *(.sbss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
