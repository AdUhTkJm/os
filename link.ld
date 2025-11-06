OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS {
  . = 0x80200000;

  .text : {
    *(.text*)
  }

  .rodata : {
    *(.rodata*)
  }

  .data : {
    *(.data*)
  }

  .bss : {
    __bss_begin = .;
    *(.bss*)
    *(COMMON)
    __bss_end = .;
  }

  . = ALIGN(4);
  . += 131072;
  __stack_top = .;
}
