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

  for (unsigned i = 0; i < header.e_phnum; i++) {
    program_header phdr;
    ldso->read(&phdr, sizeof(phdr));
    if (phdr.p_type == PT_LOAD) {
      if (phdr.p_memsz < phdr.p_filesz)
        return -ENOEXEC;
    
      // Read the content of the mapped section.
      SeekGuard guard(ldso, phdr.p_offset);

      va_t va = phdr.p_vaddr + loadbase;
      va_t aligned = rounddown<PAGE_SIZE>(va);
      auto off = va - aligned;
      va_t end = roundup<PAGE_SIZE>(va + phdr.p_memsz);

      int prot = 0;
      if (phdr.p_flags & PF_R) prot |= PROT_READ;
      if (phdr.p_flags & PF_W) prot |= PROT_WRITE;
      if (phdr.p_flags & PF_X) prot |= PROT_EXEC;
      vma::vma_t vma = {
        aligned, end, prot, MAP_PRIVATE,
        ldso, phdr.p_offset - off, phdr.p_filesz + off
      };
      pcb->vma.insert(vma);
    }
  }

  return expected(header.e_entry + loadbase);
}

// When we emit an error, we must make sure that pcb and tcb isn't modified in any way,
// except for VMAs (which will be handled in the caller).
expected<auxv> load_elf(file *content, tcb_t *tcb) {
  if (!content)
    return -ENOENT;
  
  elf_header header;
  content->read(&header, sizeof(header));

  if (strncmp(header.e_ident, "\x7f""ELF", 4) != 0)
    return -ENOEXEC;
  if (header.e_machine != EM_MACHINE)
    return -ENOEXEC;

  // It is very likely that tcb == active().
  // We must be aware: many other procedures implicitly touches it.
  auto pcb = tcb->pcb;

  content->seek(header.e_phoff, file::begin);
  // The random offset.
  auto loadbase = 0;
  if (header.e_type == ET_DYN)
    loadbase = 0x4000'0000; // TODO: randomize
  auto pc = loadbase + header.e_entry;
  struct auxv auxv;
  
  unique_ptr<char> interp = nullptr;
  size_t loadmax = 0;

  for (unsigned i = 0; i < header.e_phnum; i++) {
    program_header phdr;
    content->read(&phdr, sizeof(phdr));
    if (phdr.p_type == PT_LOAD) {
      if (phdr.p_memsz < phdr.p_filesz)
        return -ENOEXEC;
    
      // Read the content of the mapped section.
      SeekGuard guard(content, phdr.p_offset);
      va_t va = phdr.p_vaddr + loadbase;

      va_t aligned = rounddown<PAGE_SIZE>(va);
      auto off = va - aligned;
      va_t end = roundup<PAGE_SIZE>(va + phdr.p_memsz);
      loadmax = max(loadmax, end);

      // Check whether program header is mapped inside this region.
      if (phdr.p_offset <= header.e_phoff && phdr.p_offset + phdr.p_filesz > header.e_phoff)
        auxv.phdr = va + header.e_phoff;

      int prot = 0;
      if (phdr.p_flags & PF_R) prot |= PROT_READ;
      if (phdr.p_flags & PF_W) prot |= PROT_WRITE;
      if (phdr.p_flags & PF_X) prot |= PROT_EXEC;
      vma::vma_t vma {
        aligned, end, prot, MAP_PRIVATE,
        content, phdr.p_offset - off, phdr.p_filesz + off
      };
      pcb->vma.insert(vma);
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

  auxv.interp = false;
  if (interp) {
    int fd = pcb->open_file(interp.get(), O_RDONLY);
    if (fd < 0)
      return fd;

    file *ldso = pcb->ftbl->at(fd);
    auto ret = load_interp(ldso, pcb);
    if (!ret) {
      pcb->close_file(fd);
      return ret.error();
    }
    
    pc = *ret;
    auxv.interp = true;
    pcb->close_file(fd);
  }

  auxv.entry = loadbase + header.e_entry;
  auxv.phnum = header.e_phnum;
  
  pcb->vma.heap_begin = loadmax;
  pcb->vma.heap_end = loadmax + PAGE_SIZE;
  // Insert a heap and a user stack out there.
  pcb->vma.insert(vma::vma_t {
    loadmax, loadmax + PAGE_SIZE,
    PROT_READ | PROT_WRITE, MAP_PRIVATE
  });
  // Allocate a stack. Note it grows downwards.
  pcb->vma.insert(vma::vma_t {
    stack_top - user_stack_size, tcb->usp = stack_top,
    PROT_READ | PROT_WRITE, MAP_PRIVATE
  });
  pcb->rlims[RLIMIT_STACK].rlim_cur = pcb->rlims[RLIMIT_STACK].rlim_max = user_stack_size;
  tcb->status = Init;
  tcb->pc = pc;
  return auxv;
}

}
