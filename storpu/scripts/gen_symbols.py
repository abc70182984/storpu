import argparse

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection


def export_symbols(elf, output):
    with open(elf, 'rb') as fin:
        elffile = ELFFile(fin)

        symbol_tables = [(idx, s)
                         for idx, s in enumerate(elffile.iter_sections())
                         if isinstance(s, SymbolTableSection)]

        with open(output, 'w') as fout:
            fout.write('/* Auto-generated.  Do not edit. */\n\n')
            fout.write('#pragma once\n\n')

            for _, section in symbol_tables:
                if section.name != '.symtab':
                    continue

                if section['sh_entsize'] == 0:
                    continue

                for _, symbol in enumerate(section.iter_symbols()):
                    sym_type = symbol['st_info']['type']
                    sym_bind = symbol['st_info']['bind']
                    sym_value = symbol['st_value']

                    if sym_type == 'STT_FUNC' and sym_bind == 'STB_GLOBAL':
                        fout.write(
                            f'#define ENTRY_{symbol.name}\t\t0x{sym_value:x}\n'
                        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Generate symbol list header')
    parser.add_argument('library', type=str, help='Input library')
    parser.add_argument('output', type=str, help='Output header')
    args = parser.parse_args()

    export_symbols(args.library, args.output)
