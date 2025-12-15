OUTPUT_ARCH(riscv)
ENTRY(_start)

VIRT_OFFSET = 0xffffffc000000000;

SECTIONS {
  . = 0x90000000;
  .text.low : {
    *(.text.low)
  }

  LMA_START = 0x90002000;

  . = 0xffffffc090002000;
  __kernel_begin = .;
  .text.high : AT(LMA_START) {
    *(.text.high)
  }

  . = ALIGN(8);
  LMA_NEXT = . - VIRT_OFFSET;
  .text : AT(LMA_NEXT) {
    __text_begin = .;
    *(.text*)
    __text_end = .;
  }

  . = ALIGN(8);
  LMA_NEXT = . - VIRT_OFFSET;
  .rodata : AT(LMA_NEXT) {
    __rodata_begin = .;
    *(.rodata* .srodata*)
    __rodata_end = .;
  }

  . = ALIGN(8);
  LMA_NEXT = . - VIRT_OFFSET;
  .data : AT(LMA_NEXT) {
    __data_begin = .;
    *(.sdata .sdata.* .gnu.linkonce.s.*)
    *(.data .data.* .gnu.linkonce.d.*)
    __data_end = .;
  }

  /*
  * The .bss section does not need an AT() directive because it is
  * uninitialized data; it only occupies space in the VMA, not the LMA.
  */
  .bss : {
    __bss_begin = .;
    *(.sbss .sbss.*)
    *(.bss .bss.* .gnu.linkonce.b.*)
    *(COMMON)
    __bss_end = .;
  }

  . = ALIGN(16);
  . += 131072;
  __stack_top = .;
  
  /* Kernel End and stack cannot overlap. */
  . += 1;
  . = ALIGN(4096);
  __kernel_end = .;

  /* Leave 16MB for free list. */
  . += 16777216;
}
