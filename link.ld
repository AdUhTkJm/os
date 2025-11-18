OUTPUT_ARCH(riscv)
ENTRY(_start)

VIRT_OFFSET = 0xffffffc000000000;

SECTIONS {
  . = 0x80200000;
  .text.low : {
    *(.text.low)
  }

  LMA_START = 0x80203000;

  . = 0xffffffc080203000;
  __kernel_begin = .;
  /* The .text.high section is linked to the high VMA, but loaded at LMA_START */
  .text.high : AT(LMA_START) {
    *(.text.high)
  }

  LMA_NEXT = LOADADDR(.text.high) + SIZEOF(.text.high);
  .text : AT(LMA_NEXT) {
    __text_begin = .;
    *(.text*)
    __text_end = .;
  }

  LMA_NEXT = LOADADDR(.text) + SIZEOF(.text);
  .rodata : AT(LMA_NEXT) {
    __rodata_begin = .;
    *(.rodata* .srodata*)
    __rodata_end = .;
  }

  LMA_NEXT = LOADADDR(.rodata) + SIZEOF(.rodata);
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
}
