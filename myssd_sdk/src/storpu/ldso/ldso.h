#ifndef _LDSO_H_
#define _LDSO_H_

#include <types.h>
#include <elf.h>

#ifdef __LP64__
#define ElfW(name) Elf64_##name
#define ELFW(name) ELF64_##name
#else
#define ElfW(name) Elf32_##name
#define ELFW(name) ELF32_##name
#endif

#ifdef __i386__
#define R_TYPE(name) R_386_##name
#elif defined(__x86_64__)
#define R_TYPE(name) R_X86_64_##name
#elif defined(__aarch64__)
#define R_TYPE(name) R_AARCH64_##name
#elif defined(__riscv)
#define R_TYPE(name) R_RISCV_##name
#endif

typedef void (*so_func_t)(void);

struct vm_context;

struct so_info {
    struct vm_context* ctx;
    ElfW(Ehdr) * ehdr;
    ElfW(Phdr) * phdr;
    unsigned int phnum;

    char* mapbase;
    size_t mapsize;
    char* relocbase;
    ElfW(Dyn) * dynamic;

    ElfW(Rel) * rel, *relend;
    ElfW(Rela) * rela, *relaend;
    ElfW(Rel) * pltrel, *pltrelend;
    ElfW(Rela) * pltrela, *pltrelaend;
    ElfW(Sym) * symtab;
    char* strtab;
    int strtabsz;
    ElfW(Addr) * pltgot;

    unsigned nbuckets;
    int* buckets;
    unsigned nchains;
    int* chains;

    size_t tls_index;
    void* tls_init;
    size_t tls_init_size;
    size_t tls_size;
    size_t tls_offset;
    size_t tls_align;
    int tls_done;

    so_func_t init;
    so_func_t fini;
    int init_done;
    int init_called;

    ElfW(Addr) * init_array;
    size_t init_array_size;
    ElfW(Addr) * fini_array;
    size_t fini_array_size;
};

void ldso_die(void);

int ldso_process_phdr(struct so_info* si, ElfW(Phdr) * phdr, int phnum);
int ldso_process_dynamic(struct so_info* si);

ElfW(Sym) * ldso_find_sym(struct so_info* si, unsigned long symnum,
                          struct so_info** obj, int in_plt);
ElfW(Sym) * ldso_find_plt_sym(struct so_info* si, unsigned long symnum,
                              struct so_info** obj);

int ldso_relocate_objects(struct so_info* si);

int ldso_tls_allocate_offset(struct so_info* si);
void ldso_tls_initial_allocation(struct vm_context* ctx);

#endif
