#ifndef ELF_H
#define ELF_H

#include "../utils/helper.h"
#include "../fs/vfs.h"
#include "pcb.h"

/*
See linux manual, elf(5).
Also see https://gist.github.com/x0nu11byt3/bcb35c3de461e5fb66173071a2379779.
*/

#define ET_NONE		0		/* No file type */
#define ET_REL		1		/* Relocatable file */
#define ET_EXEC		2		/* Executable file */
#define ET_DYN		3		/* Shared object file */
#define ET_CORE		4		/* Core file */
#define	ET_NUM		5		/* Number of defined types */
#define ET_LOOS		0xfe00		/* OS-specific range start */
#define ET_HIOS		0xfeff		/* OS-specific range end */
#define ET_LOPROC	0xff00		/* Processor-specific range start */
#define ET_HIPROC	0xffff		/* Processor-specific range end */

#define EM_RISCV	   243
#define EM_LOONGARCH 258
#ifdef __riscv
#  define EM_MACHINE EM_RISCV
#endif
#ifdef __loongarch__
#  define EM_MACHINE EM_LOONGARCH
#endif
#ifndef EM_MACHINE
#  define EM_MACHINE 0 /* Fallback for x86 (mainly for IDE). */
#endif

#define PF_X		(1 << 0)	/* Segment is executable */
#define PF_W		(1 << 1)	/* Segment is writable */
#define PF_R		(1 << 2)	/* Segment is readable */

namespace os {

struct elf_header_t {
  char e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

#define	PT_NULL		0		/* Program header table entry unused */
#define PT_LOAD		1		/* Loadable program segment */
#define PT_DYNAMIC	2		/* Dynamic linking information */
#define PT_INTERP	3		/* Program interpreter */
#define PT_NOTE		4		/* Auxiliary information */
#define PT_SHLIB	5		/* Reserved */
#define PT_PHDR		6		/* Entry for header table itself */
#define PT_TLS		7		/* Thread-local storage segment */
#define	PT_NUM		8		/* Number of defined types */

/* ELF Program header. */
struct elf_phdr_t {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
};

// Changes the content of `pcb` by parsing the ELF file.
result load_elf(file *content, pcb_t *pcb);

}

#endif
