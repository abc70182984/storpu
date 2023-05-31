#include <string.h>
#include <errno.h>

#include <types.h>
#include <storpu/vm.h>
#include <storpu/thread.h>
#include <slab.h>

#include "ldso.h"

void ldso_die(void) { thread_exit(-1UL); }

static int elf_unpack(char* hdr, ElfW(Ehdr) * *elf_hdr, ElfW(Phdr) * *prog_hdr)
{
    *elf_hdr = (ElfW(Ehdr)*)hdr;
    *prog_hdr = (ElfW(Phdr)*)(hdr + (*elf_hdr)->e_phoff);

    return 0;
}

static void ldso_call_initfini_function(ElfW(Addr) func)
{
    ((void (*)(void))(uintptr_t)func)();
}

static void ldso_call_init_function(struct so_info* si)
{
    if (si->init_array_size == 0 && (si->init_called || !si->init)) return;

    if (!si->init_called && si->init) {
        si->init_called = TRUE;
        si->init();
    }

    while (si->init_array_size > 0) {
        ElfW(Addr) init = *si->init_array++;
        si->init_array_size--;
        ldso_call_initfini_function(init);
    }
}

int ldso_init_context(struct vm_context* ctx)
{
    struct so_info* si_main;

    SLABALLOC(si_main);
    if (!si_main) return ENOMEM;

    ctx->so_info = si_main;
    ctx->tls_max_index = 1;

    memset(si_main, 0, sizeof(*si_main));

    si_main->ctx = ctx;
    elf_unpack(ctx->load_base, &si_main->ehdr, &si_main->phdr);

    si_main->relocbase = ctx->load_base;

    ldso_process_phdr(si_main, si_main->phdr, si_main->ehdr->e_phnum);
    ldso_process_dynamic(si_main);

    ldso_tls_allocate_offset(si_main);

    ldso_relocate_objects(si_main);

    ldso_tls_initial_allocation(ctx);

    ldso_call_init_function(si_main);

    return 0;
}
