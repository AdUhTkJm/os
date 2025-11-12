OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS {
  . = 0x80200000;

  .text : {
    __text_begin = .;
    *(.text*)
    __text_end = .;
  }

  .rodata : {
    __rodata_begin = .;
    *(.rodata*)
    __rodata_end = .;
  }

  .data : {
    __data_begin = .;*
    *(.sdata .sdata.* .gnu.linkonce.s.*)
    *(.data .data.* .gnu.linkonce.d.*)
    __data_end = .;
  }

  .bss : {
    __bss_begin = .;
    *(.sbss .sbss.*)
    *(.bss .bss.* .gnu.linkonce.b.*)
    *(COMMON)
    __bss_end = .;
  }

  . = ALIGN(4096);
  __pt_root = .; /* Root of page table */
  . += 4096;

  . = ALIGN(16);
  . += 131072;
  __stack_top = .;
  
  /* Kernel End and stack cannot overlap. */
  . += 1;
  . = ALIGN(4096);
  __kernel_end = .;
}
