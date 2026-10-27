#!/usr/bin/env python3
"""Time per function per thread from the PS4 sampler's samples.log (ps4/ps4_sampler.cpp, SAVAGEWIND_PS4_SAMPLE=1).

Usage: samples.py <samples.log> <savagewind.oelf> [--top N] [--thread TEXT] [--from S]

Adds up every window (from second S on) per thread, names the eboot addresses (llvm-symbolizer, nm where there's no
debug info: Mesa; recompiled game functions are f_<guest address>) and prints each thread's busiest functions. "sys" samples are in the system
libraries, attributed to the eboot code that called them.
"""
import argparse
import bisect
import collections
import re
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('oelf')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--thread', default='')
    ap.add_argument('--from', dest='start', type=float, default=0)
    a = ap.parse_args()

    head = re.compile(r'^== (\d+) s, (.*): (\d+) samples')
    row = re.compile(r'^\s+([\d.]+)% (elf|sys|jit) 0x([0-9a-f]+)')
    per = collections.defaultdict(collections.Counter)  # thread -> address -> samples
    total = collections.Counter()
    sys_n = collections.Counter()
    thread, n, keep = None, 0, False
    for line in open(a.log, errors='replace'):
        m = head.match(line)
        if m:
            thread, n = m.group(2), int(m.group(3))
            keep = float(m.group(1)) >= a.start and a.thread in thread
            if keep:
                total[thread] += n
            continue
        m = row.match(line)
        if m and keep:
            count = round(float(m.group(1)) * n / 100)
            if m.group(2) == 'elf':
                per[thread][int(m.group(3), 16)] += count
            else:
                sys_n[thread] += count

    # function names: llvm-symbolizer (the debug info: static functions on their own), the symbol table where it has
    # none (Mesa)
    addrs = sorted({x for c in per.values() for x in c})
    out = subprocess.run(['llvm-symbolizer', '--obj=' + a.oelf, '--functions=short', '--no-inlines'],
                         input='\n'.join(hex(x) for x in addrs), capture_output=True, text=True).stdout
    dbg = dict(zip(addrs, (b.split('\n')[0] for b in out.strip('\n').split('\n\n'))))
    syms = []
    for line in subprocess.run(['nm', '-n', '--defined-only', a.oelf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in 'Tt':
            syms.append((int(p[0], 16), p[2]))
    starts = [x for x, _ in syms]
    names = {}
    for x in addrs:
        i = bisect.bisect_right(starts, x) - 1
        names[x] = dbg[x] if dbg.get(x, '??') != '??' else syms[i][1] if i >= 0 else hex(x)

    for t in sorted(total, key=lambda t: -total[t]):
        fn = collections.Counter()
        for x, c in per[t].items():
            fn[names.get(x, hex(x))] += c
        n = total[t]
        busy = sum(fn.values()) + sys_n[t]
        print(f'== {t}: {n} samples ({busy / n * 100:.0f}% in eboot or system calls from it)')
        for name, c in fn.most_common(a.top):
            print(f'  {c / n * 100:6.2f}%  {name}')
        if sys_n[t]:
            print(f'  {sys_n[t] / n * 100:6.2f}%  (system libraries)')


if __name__ == '__main__':
    main()
