#include <xil_printf.h>

#include <types.h>
#include <storpu/thread.h>

#include "ldso.h"

extern void ldso_bind_entry();
extern void ldso_tlsdesc_static();

static void ldso_setup_pltgot(struct so_info* si)
{
    si->pltgot[1] = (ElfW(Addr))si;
    si->pltgot[2] = (ElfW(Addr))ldso_bind_entry;
}

static void ldso_tlsdesc_fill(struct so_info* si, const ElfW(Rela) * rela,
                              ElfW(Addr) * where)
{
    ElfW(Sym) * sym;
    ElfW(Word) symnum = ELFW(R_SYM)(rela->r_info);
    struct so_info* def_obj = NULL;
    ElfW(Addr) offset = 0;

    if (symnum != 0) {
        sym = ldso_find_sym(si, symnum, &def_obj, 0);
        if (!sym) ldso_die();

        offset = sym->st_value;
    } else
        def_obj = si;

    offset += rela->r_addend;

    if (si->tls_done) {
        where[0] = (ElfW(Addr))ldso_tlsdesc_static;
        where[1] = si->tls_offset + offset + sizeof(struct tls_tcb);
    }
}

static int ldso_relocate_plt_lazy(struct so_info* si)
{
    ElfW(Rela) * rela;

    if (si->relocbase == NULL) return 0;

    for (rela = si->pltrela; rela < si->pltrelaend; rela++) {
        ElfW(Addr)* addr = (ElfW(Addr)*)((char*)si->relocbase + rela->r_offset);

        switch (ELFW(R_TYPE)(rela->r_info)) {
        case R_TYPE(JUMP_SLOT):
            *addr += (ElfW(Addr))si->relocbase;
            break;
        case R_TYPE(TLSDESC):
            ldso_tlsdesc_fill(si, rela, addr);
            break;
        }
    }

    return 0;
}

static int ldso_relocate_nonplt_objects(struct so_info* si)
{
    ElfW(Rela) * rela;

    if (si->rela) {
        for (rela = si->rela; rela < si->relaend; rela++) {
            ElfW(Word) r_type = ELFW(R_TYPE)(rela->r_info);
            ElfW(Word) symnum = ELFW(R_SYM)(rela->r_info);
            ElfW(Addr)* where = (ElfW(Addr)*)(si->relocbase + rela->r_offset);
            ElfW(Sym) * sym;
            struct so_info* def_obj = NULL;

            switch (r_type) {
            case R_TYPE(NONE):
                break;

            case R_TYPE(ABS64):
            case R_TYPE(GLOB_DAT):
                sym = ldso_find_sym(si, symnum, &def_obj, 0);
                if (!sym) continue;

                *where = (ElfW(Addr))def_obj->relocbase + sym->st_value +
                         rela->r_addend;
                break;

            case R_TYPE(RELATIVE):
                *where = (ElfW(Addr))si->relocbase + rela->r_addend;
                break;

            case R_TYPE(TLSDESC):
                ldso_tlsdesc_fill(si, rela, where);
                break;

            case R_TYPE(TLS_TPREL):
                sym = ldso_find_sym(si, symnum, &def_obj, 0);
                if (!sym) continue;

                if (!def_obj->tls_done && ldso_tls_allocate_offset(def_obj))
                    return -1;

                *where = (ElfW(Addr))(sym->st_value + def_obj->tls_offset +
                                      rela->r_addend + sizeof(struct tls_tcb));
                break;

            default:
                xil_printf("Unknown relocation type: %d\n", r_type);
                break;
            }
        }
    }

    return 0;
}

static int ldso_relocate_plt_object(struct so_info* si, ElfW(Rela) * rel,
                                    ElfW(Addr) * new_addr)
{
    ElfW(Addr)* where = (ElfW(Addr)*)(si->relocbase + rel->r_offset);
    unsigned long info = rel->r_info;

    ElfW(Sym) * sym;

    struct so_info* obj;
    sym = ldso_find_plt_sym(si, ELFW(R_SYM)(info), &obj);
    if (!sym) return -1;

    ElfW(Addr) addr = (ElfW(Addr))(obj->relocbase + sym->st_value);

    if (*where != addr) *where = addr;
    if (new_addr) *new_addr = addr;

    /* xprintf("%s -> %x\n", obj->strtab + sym->st_name, addr); */

    return 0;
}

int ldso_relocate_objects(struct so_info* si)
{
    if (ldso_relocate_plt_lazy(si) != 0) return -1;

    if (ldso_relocate_nonplt_objects(si) != 0) return -1;

    if (si->pltgot) ldso_setup_pltgot(si);

    return 0;
}

char* ldso_bind(struct so_info* si, ElfW(Word) reloff)
{
    ElfW(Rela)* rel = (ElfW(Rela)*)((char*)si->pltrela + reloff);
    ElfW(Addr) new_addr;
    int ret = ldso_relocate_plt_object(si, rel, &new_addr);

    if (ret) {
        ElfW(Sym)* sym = si->symtab + ELFW(R_SYM)(rel->r_info);
        char* name = si->strtab + sym->st_name;

        xil_printf("can't lookup symbol %s\n", name);
        thread_exit(-1);
        return NULL;
    }

    return (char*)new_addr;
}
