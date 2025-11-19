#include "elf.h"
#include "../mem/vma.h"
#include "../mem/kalloc.h"
#include "../mem/ptable.h"

namespace os {

pcb_t *load_elf(file *content) {
  elf_header_t header;
  content->read(&header, sizeof(header));

  if (strncmp(header.e_ident, "\x7f""ELF", 4) != 0)
    return nullptr;
  if (header.e_machine != EM_MACHINE)
    return nullptr;
  printk("Basic check good\n");

  pcb_t *pcb_p = new pcb_t;
  auto &pcb = *pcb_p;
  pcb.status = Init;
  pcb.entry = header.e_entry;

  content->seek(header.e_phoff, file::begin);
  // The random offset.
  auto offset = 0;
  if (header.e_type == ET_DYN)
    offset = 0; // TODO: randomize
  
  for (unsigned i = 0; i < header.e_phnum; i++) {
    elf_phdr_t phdr;
    content->read(&phdr, sizeof(phdr));
    if (phdr.p_type == PT_LOAD) {
      if (phdr.p_memsz < phdr.p_filesz)
        return nullptr;
    
      // Read the content of the mapped section.
      auto before = content->seek(phdr.p_offset, file::begin);
      char *text = (char *) vmalloc(phdr.p_filesz);
      content->write(text, phdr.p_filesz);
      content->seek(before, file::begin);

      va_t va = phdr.p_vaddr + offset;
      va_t begin = os::rounddown<PAGE_SIZE>(va);
      va_t end = os::roundup<PAGE_SIZE>(va + phdr.p_memsz);

      int prot = 0;
      if (phdr.p_flags & PF_R) prot |= PROT_READ;
      if (phdr.p_flags & PF_W) prot |= PROT_WRITE;
      if (phdr.p_flags & PF_X) prot |= PROT_EXEC;
      vma_t vma = {
        .begin = begin, .end = end, .prot = prot, .flags = MAP_PRIVATE,
        .backup = content, .offset = phdr.p_offset
      };
      pcb.vma.push_back(vma);

      vfree(text);
    }
  }
  init(pcb_p);
  return pcb_p;
}

}
