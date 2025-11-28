#include "elf.h"
#include "../mem/vma.h"
#include "../mem/kalloc.h"
#include "../mem/ptable.h"

namespace os {

result load_elf(file *content, pcb_t *pcb) {
  if (!content)
    return result::failure;
  
  elf_header_t header;
  content->read(&header, sizeof(header));

  if (strncmp(header.e_ident, "\x7f""ELF", 4) != 0)
    return result::failure;
  if (header.e_machine != EM_MACHINE)
    return result::failure;

  pcb->status = Init;
  pcb->pc = header.e_entry;

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
        return result::failure;
    
      // Read the content of the mapped section.
      auto before = content->seek(phdr.p_offset, file::begin);
      char *text = (char *) vmalloc(phdr.p_filesz);
      content->write(text, phdr.p_filesz);
      content->seek(before, file::begin);

      va_t va = phdr.p_vaddr + offset;

      int prot = 0;
      if (phdr.p_flags & PF_R) prot |= PROT_READ;
      if (phdr.p_flags & PF_W) prot |= PROT_WRITE;
      if (phdr.p_flags & PF_X) prot |= PROT_EXEC;
      vma_t vma = {
        .begin = va, .end = va + phdr.p_memsz, .prot = prot, .flags = MAP_PRIVATE,
        .backup = content, .offset = phdr.p_offset
      };
      pcb->vma.push_back(vma);

      vfree(text);
    }
  }

  init_user(pcb);
  init(pcb);
  return result::success;
}

}
