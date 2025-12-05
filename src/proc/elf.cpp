#include "elf.h"
#include "../mem/vma.h"
#include "../mem/kalloc.h"
#include "../mem/ptable.h"

namespace os {

static expected<va_t> load_interp(file *ldso, pcb_t *pcb) {
  if (!ldso)
    return -ENOENT;
  
  elf_header header;
  ldso->read(&header, sizeof(header));

  if (strncmp(header.e_ident, "\x7f""ELF", 4) != 0)
    return -ENOEXEC;
  if (header.e_machine != EM_MACHINE)
    return -ENOEXEC;

  ldso->seek(header.e_phoff, file::begin);
  // The random offset.
  if (header.e_type != ET_DYN)
    return -ENOEXEC;

  auto loadbase = interp_pos;
  
  unique_ptr<char> interp = nullptr;

  for (unsigned i = 0; i < header.e_phnum; i++) {
    program_header phdr;
    ldso->read(&phdr, sizeof(phdr));
    if (phdr.p_type == PT_LOAD) {
      if (phdr.p_memsz < phdr.p_filesz)
        return -ENOEXEC;
    
      // Read the content of the mapped section.
      SeekGuard guard(ldso, phdr.p_offset);

      va_t va = phdr.p_vaddr + loadbase;

      int prot = 0;
      if (phdr.p_flags & PF_R) prot |= PROT_READ;
      if (phdr.p_flags & PF_W) prot |= PROT_WRITE;
      if (phdr.p_flags & PF_X) prot |= PROT_EXEC;
      vma_t vma = {
        .begin = va, .end = va + phdr.p_memsz, .prot = prot, .flags = MAP_PRIVATE | VMA_IS_PT_LOAD,
        .backup = ldso, .offset = phdr.p_offset, .maxread = phdr.p_filesz
      };
      ldso->ref();
      pcb->vma.push_back(vma);
    }
  }

  return expected(header.e_entry + loadbase);
}

expected<auxv> load_elf(file *content, pcb_t *pcb) {
  if (!content)
    return -ENOENT;
  
  elf_header header;
  content->read(&header, sizeof(header));

  if (strncmp(header.e_ident, "\x7f""ELF", 4) != 0)
    return -ENOEXEC;
  if (header.e_machine != EM_MACHINE)
    return -ENOEXEC;

  // It is very likely that pcb_p == scheduler.active.
  // We must be aware: many other procedures implicitly touches it.
  pcb->status = Init;

  content->seek(header.e_phoff, file::begin);
  // The random offset.
  auto loadbase = 0;
  if (header.e_type == ET_DYN)
    loadbase = 0x4000'0000; // TODO: randomize
  auto pc = loadbase + header.e_entry;
  
  unique_ptr<char> interp = nullptr;

  for (unsigned i = 0; i < header.e_phnum; i++) {
    program_header phdr;
    content->read(&phdr, sizeof(phdr));
    if (phdr.p_type == PT_LOAD) {
      if (phdr.p_memsz < phdr.p_filesz)
        return -ENOEXEC;
    
      // Read the content of the mapped section.
      SeekGuard guard(content, phdr.p_offset);
      va_t va = phdr.p_vaddr + loadbase;

      int prot = 0;
      if (phdr.p_flags & PF_R) prot |= PROT_READ;
      if (phdr.p_flags & PF_W) prot |= PROT_WRITE;
      if (phdr.p_flags & PF_X) prot |= PROT_EXEC;
      vma_t vma = {
        .begin = va, .end = va + phdr.p_memsz, .prot = prot, .flags = MAP_PRIVATE | VMA_IS_PT_LOAD,
        .backup = content, .offset = phdr.p_offset, .maxread = phdr.p_filesz
      };
      content->ref();
      pcb->vma.push_back(vma);
    }

    if (phdr.p_type == PT_INTERP) {
      // We expect a single PT_INTERP section.
      if (interp)
        return -ENOEXEC;

      SeekGuard guard(content, phdr.p_offset);
      interp.reset((char *) vmalloc(phdr.p_filesz + 1));
      content->read(interp.get(), phdr.p_filesz);
      interp[phdr.p_filesz] = 0;
    }
  }

  struct auxv auxv;
  auxv.used = false;
  if (interp) {
    int fd = pcb->open_file(interp.get(), O_RDONLY);
    if (fd < 0)
      return fd;
    file *ldso = pcb->ftbl[fd];
    auto ret = load_interp(ldso, pcb);
    if (!ret)
      return ret.error();
    pc = *ret;
    auxv.entry = loadbase + header.e_entry;
    auxv.phdr = loadbase + header.e_phoff;
    auxv.phnum = header.e_phnum;
    auxv.used = true;
    pcb->close_file(fd);
  }

  init_user(pcb);
  pcb->pc = pc;
  return auxv;
}

}
