#include <errno.h>
#include <string.h>
#include <xil_printf.h>
#include <xil_cache.h>

#include <types.h>
#include <storpu/vm.h>

#include <elf.h>

/* #define ELF_DEBUG */

#ifdef __LP64__
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Phdr Elf_Phdr;
typedef Elf64_auxv_t Elf_auxv_t;
#else
typedef Elf32_Ehdr Elf_Ehdr;
typedef Elf32_Phdr Elf_Phdr;
typedef Elf32_auxv_t Elf_auxv_t;
#endif

struct exec_info;

typedef int (*libexec_allocator_t)(struct exec_info* execi, void* vaddr,
                                   size_t len, unsigned int prot_flags);
typedef int (*libexec_copymem_t)(struct exec_info* execi, unsigned long offset,
                                 void* vaddr, size_t len);
typedef int (*libexec_clearmem_t)(struct exec_info* execi, void* vaddr,
                                  size_t len);
typedef int (*libexec_clearproc_t)(struct exec_info* execi);

struct exec_info {
    struct vm_context* ctx;

    char* header;
    size_t header_len;

    libexec_allocator_t allocmem;
    libexec_allocator_t allocmem_prealloc;
    libexec_copymem_t copymem;
    libexec_clearmem_t clearmem;
    libexec_clearproc_t clearproc;

    void* callback_data;

    size_t load_offset;
    size_t text_size;
    size_t data_size;
    size_t stack_size;

    void* entry_point;
    void* stack_top;

    void* phdr;
    unsigned int phnum;
    void* load_base;
};

static int elf_check_header(Elf_Ehdr* elf_hdr)
{
    if (elf_hdr->e_ident[0] != 0x7f ||
        (elf_hdr->e_type != ET_EXEC && elf_hdr->e_type != ET_DYN)) {
        return ENOEXEC;
    }

    return 0;
}

static int elf_unpack(char* hdr, Elf_Ehdr** elf_hdr, Elf_Phdr** prog_hdr)
{
    *elf_hdr = (Elf_Ehdr*)hdr;
    *prog_hdr = (Elf_Phdr*)(hdr + (*elf_hdr)->e_phoff);

    return 0;
}

int vm_exec_elf(struct exec_info* execi)
{
    Elf_Ehdr* elf_hdr;
    Elf_Phdr* prog_hdr;
    unsigned long load_base = 0;
    int first = TRUE;
    int i, ret;

    if ((ret = elf_check_header((Elf_Ehdr*)execi->header)) != 0) return ret;

    if ((ret = elf_unpack(execi->header, &elf_hdr, &prog_hdr)) != 0) return ret;

    if (execi->clearproc) execi->clearproc(execi);

    execi->phnum = elf_hdr->e_phnum;

    for (i = 0; i < elf_hdr->e_phnum; i++) {
        Elf_Phdr* phdr = &prog_hdr[i];
        unsigned long foffset;
        unsigned long p_vaddr, vaddr;
        size_t fsize, memsize;
        int prot_flags = 0;

        if (phdr->p_flags & PF_R) prot_flags |= PROT_READ;
        if (phdr->p_flags & PF_W) prot_flags |= PROT_WRITE;
        if (phdr->p_flags & PF_X) prot_flags |= PROT_EXEC;

        if (phdr->p_type == PT_PHDR) execi->phdr = (void*)phdr->p_vaddr;

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
            continue; /* ignore */

        if ((phdr->p_vaddr % ARCH_PG_SIZE) != (phdr->p_offset % ARCH_PG_SIZE)) {
            xil_printf("libexec: unaligned ELF program?\n");
            return ENOEXEC;
        }

        foffset = phdr->p_offset;
        fsize = phdr->p_filesz;
        p_vaddr = vaddr = phdr->p_vaddr + execi->load_offset;
        memsize = phdr->p_memsz;

#ifdef ELF_DEBUG
        xil_printf("libexec: segment %d: vaddr: 0x%x, size: { file: 0x%x, mem: "
                   "0x%x }, foffset: 0x%x\n",
                   i, vaddr, fsize, memsize, foffset);
#endif

        /* align */
        size_t alignment = vaddr % ARCH_PG_SIZE;
        foffset -= alignment;
        vaddr -= alignment;
        fsize += alignment;
        memsize += alignment;

        memsize = roundup(memsize, ARCH_PG_SIZE);
        fsize = roundup(fsize, ARCH_PG_SIZE);

        if (first || load_base > vaddr) load_base = vaddr;
        first = FALSE;

        if ((phdr->p_flags & PF_X) != 0)
            execi->text_size = memsize;
        else {
            execi->data_size = memsize;
        }

        if (execi->allocmem(execi, (void*)vaddr, memsize, prot_flags) != 0) {
            if (execi->clearproc) execi->clearproc(execi);
            return ENOMEM;
        }

        if (execi->copymem(execi, foffset, (void*)vaddr, fsize) != 0) {
            if (execi->clearproc) execi->clearproc(execi);
            return ENOMEM;
        }

        /* clear remaining memory */
        size_t zero_len = p_vaddr - vaddr;
        if (zero_len) {
#ifdef ELF_DEBUG
            xil_printf("libexec: clear memory 0x%x - 0x%x\n", vaddr,
                       vaddr + zero_len);
#endif
            execi->clearmem(execi, (void*)vaddr, zero_len);
        }

        size_t fileend = p_vaddr + phdr->p_filesz;
        size_t memend = vaddr + memsize;
        zero_len = memend - fileend;
        if (zero_len) {
#ifdef ELF_DEBUG
            xil_printf("libexec: clear memory 0x%x - 0x%x\n", fileend,
                       fileend + zero_len);
#endif
            execi->clearmem(execi, (void*)fileend, zero_len);
        }
    }

    if (execi->allocmem_prealloc &&
        execi->allocmem_prealloc(execi, execi->stack_top - execi->stack_size,
                                 execi->stack_size,
                                 PROT_READ | PROT_WRITE) != 0) {
        if (execi->clearproc) execi->clearproc(execi);
        return ENOMEM;
    }

    execi->entry_point = (void*)elf_hdr->e_entry + execi->load_offset;
    execi->load_base = (void*)load_base;

    return 0;
}

static int do_allocmem(struct exec_info* execi, void* vaddr, size_t len,
                       unsigned int prot_flags)
{
    return vm_map(execi->ctx, vaddr, len, prot_flags | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0, NULL);
}

__attribute__((unused)) static int do_allocmem_prealloc(struct exec_info* execi,
                                                        void* vaddr, size_t len,
                                                        unsigned int prot_flags)
{
    return vm_map(execi->ctx, vaddr, len, prot_flags | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0, NULL);
}

static int do_copymem(struct exec_info* execi, unsigned long offset,
                      void* vaddr, size_t len)
{
    if (offset + len > execi->header_len) return ENOEXEC;

    memcpy((void*)vaddr, (char*)execi->header + offset, len);

    return 0;
}

static int do_clearmem(struct exec_info* execi, void* vaddr, size_t len)
{
    memset(vaddr, 0, len);
    return 0;
}

int vm_exec(struct vm_context* ctx, void* elf_base)
{
    struct exec_info execi;
    int ret;

    memset(&execi, 0, sizeof(execi));

    execi.ctx = ctx;
    execi.header = elf_base;
    execi.header_len = UINT64_MAX;

    execi.allocmem = do_allocmem;
    /* execi.allocmem_prealloc = do_allocmem_prealloc; */
    execi.copymem = do_copymem;
    execi.clearmem = do_clearmem;

    execi.load_offset = VM_USER_START;
    execi.stack_top = (void*)VM_STACK_TOP;
    execi.stack_size = (2 << 20);

    ret = vm_exec_elf(&execi);
    if (ret) return ret;

    /* vm_exec_elf() installs new instructions so icache must be invalidated.
     * ARM specification says if the instruction cache is a VIPT cache, after
     * the code modification the entire instruction cache must be invalidated.
     * This seems to be the case. */
    Xil_ICacheInvalidate();

    ctx->load_base = execi.load_base;

    return ldso_init_context(ctx);
}
