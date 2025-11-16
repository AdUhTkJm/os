#include "elf.h"
#include "../mem/ptable.h"

// See linux.

int os::load_elf(file *content) {
  auto *header = (elf_header_t *) content;
  if (strncmp(header->e_ident, "\x7f""ELF", 4) != 0)
    return 1;
  if (header->e_machine != EM_MACHINE)
    return 1;
  printk("etype = %d\n", header->e_type);

  auto *phdrs = (elf_phdr_t*) ((char*) header + header->e_phoff);
  // The random offset.
  auto offset = 0;
  if (header->e_type == ET_DYN)
    offset = 0; // TODO: randomize
  
  for (unsigned i = 0; i < header->e_phnum; i++) {
    const elf_phdr_t &phdr = phdrs[i];
    if (phdr.p_type == PT_LOAD) {
      char *text = (char *) header + phdr.p_offset;
      va_t va = phdr.p_vaddr + offset;
      if (phdr.p_memsz < phdr.p_filesz)
        return 1;
      va_t begin = os::rounddown<PAGE_SIZE>(va);
      va_t end = os::roundup<PAGE_SIZE>(va + phdr.p_memsz);
    }
  }
  return 0;
}
