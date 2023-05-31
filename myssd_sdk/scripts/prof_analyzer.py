import struct
import argparse
import subprocess
import numpy as np
from PIL import Image

from rich.markup import escape
from rich.console import Console
from rich.table import Table
from rich.panel import Panel
from rich.columns import Columns


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument('samples', type=str)
    parser.add_argument('bin', type=str)
    parser.add_argument('-f',
                        '--flashout',
                        type=str,
                        default='flash_array.png')

    return parser.parse_args()


def load_samples(fin):
    mem_used = 0
    buf = bytearray()

    for l in fin.readlines():
        if not l.strip():
            continue

        if l.startswith("="):
            continue
        elif l.startswith("Mem used"):
            mem_used = int(l.split(':')[-1].strip())
        else:
            _, val = l.split(':')
            val = int(val.strip(), base=16)

            buf += struct.pack('I', val)

    buf = buf[:mem_used]

    samples = []
    flash_samples = []
    while len(buf) > 1:
        tag, buf = buf[0], buf[1:]

        if tag == 1:  # PC
            pc = struct.unpack('I', buf[:4])[0]
            buf = buf[4:]
            samples.append(pc)

        elif tag == 2:  # Flash
            channel = struct.unpack('B', buf[:1])[0]
            buf = buf[1:]
            die = struct.unpack('Q', buf[:8])[0]
            buf = buf[8:]
            flash_samples.append((channel, die))

    return samples, flash_samples


def get_symbols(bin):
    rc = subprocess.run(['nm', bin], capture_output=True, check=True)
    symbols = rc.stdout.decode('utf-8').split('\n')[:-1]
    symbols = [x.split(' ') for x in symbols]
    symbols = [(int(x, 16), y) for (x, _, y) in symbols]
    symbols = sorted(symbols, key=lambda x: x[0])

    return symbols


def get_sample_locs(samples, symbols):
    locs = dict()

    for s in samples:
        pc = s

        for (s, symbol), (e, _) in zip(symbols[:-1], symbols[1:]):
            if s <= pc < e:
                loc = symbol
                break
        else:
            loc = symbols[-1][1]

        if locs.get(loc) is None:
            locs[loc] = 1
        else:
            locs[loc] += 1

    return locs


def get_progress_bar(percentage, length):
    bar_length = length - 2
    bar = '#' * int(round(bar_length * percentage / 100.0))
    bar = bar.ljust(bar_length, '.')
    bar = f'[{bar}]'
    return escape(bar)


def truncate_string(s, n):
    if len(s) <= n:
        return s
    return s[:n - 3] + '...'


def print_report(samples, locs):
    slocs = sorted([(k, v) for k, v in locs.items()], key=lambda x: -x[1])
    total = sum([x[1] for x in slocs])
    table = Table()

    table.add_column('Name', style='cyan', no_wrap=True, justify='right')
    table.add_column('', style='magenta')
    table.add_column('Percentage', justify='right', style='green')

    for func, count in slocs:
        percentage = count / total * 100.0

        name = truncate_string(func, 35)
        prog = get_progress_bar(percentage, 30)
        p = str(count) + ' (' + f'{percentage:.1f}%'.rjust(6) + ')'
        table.add_row(name, prog, p)

        if percentage < 1.0:
            break

    rest = sum([count for _, count in slocs if count < total * 0.01])
    rest_percent = rest / total * 100.0

    table.add_row('<1%',
                  get_progress_bar(rest_percent, 30),
                  str(rest) + ' (' + f'{rest_percent:.1f}%'.rjust(6) + ')',
                  end_section=True)
    table.add_row('Total samples', get_progress_bar(100, 30),
                  f'{len(samples)} (100.0%)')

    console = Console()
    console.print(Panel(table, title='Functions'))


def plot_flash_samples(samples, out):
    channel = np.array([x for x, _ in samples], dtype=np.uint8)
    die = np.array([y for _, y in samples], dtype=np.uint64)

    die = np.unpackbits(die.view(dtype=np.uint8)).reshape((len(samples), 64))

    image = Image.new('RGB', (len(samples), 64), (255, 255, 255))

    for i in range(len(samples)):
        for j in range(8):
            if channel[i] & (1 << j):
                for k in range(8):
                    image.putpixel((i, j * 8 + k), (255, 0, 0))

        for j in range(64):
            if die[i, j]:
                image.putpixel((i, j), (0, 0, 0))

    channel = np.unpackbits(channel).reshape((len(samples), 8))
    channel = channel.sum(axis=0)
    channel_util = channel / len(samples) * 100.0

    panels = []
    for i in range(8):
        panels.append(
            f'{str(i)}{get_progress_bar(channel_util[i], 30)} {channel_util[i]:.1f}%'
        )

    console = Console()
    console.print(
        Panel(Columns(panels, padding=(0, 5)), title='Channel utilization'))

    image.save(out)


if __name__ == '__main__':
    args = parse_args()

    with open(args.samples, 'r') as fin:
        samples, flash_samples = load_samples(fin)
        symbols = get_symbols(args.bin)

        locs = get_sample_locs(samples, symbols)

        if flash_samples:
            plot_flash_samples(flash_samples, args.flashout)

        print_report(samples, locs)
