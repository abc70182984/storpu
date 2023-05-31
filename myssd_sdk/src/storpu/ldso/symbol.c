#include <string.h>

#include <types.h>
#include <storpu/symbols.h>

#include "ldso.h"

#define _SYM(name, addr)                           \
    {                                              \
        (name), { .st_value = (ElfW(Addr))(addr) } \
    }

/* clang-format off */
static struct sym_def {
    const char* name;
    ElfW(Sym) sym;
} exported_symbols[] = {
    _SYM_LIST
    _SYM(NULL, NULL),
};
/* clang-format on */

#undef _SYM

static struct so_info si_self;

static ElfW(Sym) * ldso_lookup_exported_symbol(const char* name)
{
    struct sym_def* sym;

    for (sym = exported_symbols; sym->name; sym++) {
        if (!strcmp(sym->name, name)) return &sym->sym;
    }

    return NULL;
}

unsigned long ldso_elf_hash(const char* name)
{
    const unsigned char* p = (const unsigned char*)name;
    unsigned long h = 0;
    unsigned long g;
    unsigned long c;

    for (; (c = *p) != '\0'; p++) {
        h <<= 4;
        h += c;
        if ((g = h & 0xf0000000) != 0) {
            h ^= g;
            h ^= g >> 24;
        }
    }
    return h;
}

ElfW(Sym) * ldso_lookup_symbol_obj(const char* name, unsigned long hash,
                                   struct so_info* si, int in_plt)
{
    unsigned long symnum;

    if (!si->nbuckets) {
        return NULL;
    }

    for (symnum = si->buckets[hash % si->nbuckets]; symnum != 0;
         symnum = si->chains[symnum]) {

        ElfW(Sym)* sym = si->symtab + symnum;
        char* str = si->strtab + sym->st_name;

        if (strcmp(name, str)) continue;

        if (sym->st_shndx == SHN_UNDEF &&
            (in_plt || sym->st_value == 0 ||
             ELFW(ST_TYPE)(sym->st_info) != STT_FUNC))
            continue;

        return sym;
    }

    return NULL;
}

static ElfW(Sym) * ldso_lookup_symbol(const char* name, unsigned long hash,
                                      struct so_info* so, struct so_info** obj,
                                      int in_plt)
{
    ElfW(Sym)* def = NULL;
    ElfW(Sym)* sym = NULL;
    struct so_info* def_obj = NULL;

    sym = ldso_lookup_symbol_obj(name, hash, so, in_plt);
    if (sym) {
        def = sym;
        def_obj = so;
    }

    if (!def || ELFW(ST_BIND)(def->st_info) == STB_WEAK) {
        sym = ldso_lookup_exported_symbol(name);

        if (sym) {
            def = sym;
            def_obj = &si_self;
        }
    }

    if (def) {
        *obj = def_obj;
    }

    return def;
}

ElfW(Sym) * ldso_find_sym(struct so_info* si, unsigned long symnum,
                          struct so_info** obj, int in_plt)
{
    ElfW(Sym) * sym;
    ElfW(Sym)* def = NULL;
    struct so_info* def_obj = NULL;

    sym = si->symtab + symnum;
    char* name = si->strtab + sym->st_name;

    if (ELFW(ST_BIND)(sym->st_info) != STB_LOCAL) {
        unsigned long hash = ldso_elf_hash(name);
        def = ldso_lookup_symbol(name, hash, si, &def_obj, in_plt);
    } else {
        def = sym;
        def_obj = si;
    }

    if (def) {
        *obj = def_obj;
    }

    return def;
}

ElfW(Sym) * ldso_find_plt_sym(struct so_info* si, unsigned long symnum,
                              struct so_info** obj)
{
    return ldso_find_sym(si, symnum, obj, TRUE);
}
