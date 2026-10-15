#!/usr/bin/env python3
"""Roots for code discovery from the prepared boot image (out/.../boot-linked.bin, BBPROBE5).

Writes one "kind address" line per root (hex, image offsets): entry, module init, export (native
libc bindings), reloc (a relative relocation whose value lands in an executable segment: function
pointers, vtables, initializer arrays), and "import-slot address" for each import relocation
target (calls through it leave the image). Also "exec start size" per executable segment.
usage: bootinfo.py boot-linked.bin > roots.txt
"""
import struct
import sys


def main():
    d = open(sys.argv[1], 'rb').read()
    magic = d[:8]
    if magic != b'BBPROBE5':
        sys.exit(f'unsupported boot file {magic!r} (expected BBPROBE5)')
    p = 8

    def u64():
        nonlocal p
        v = struct.unpack_from('<Q', d, p)[0]
        p += 8
        return v

    size, entry, ns, nr, import_count, capabilities = (u64() for _ in range(6))
    u64()  # procparam
    for _ in range(4):
        u64()  # main TLS
    modules = []
    for _ in range(u64()):
        base, msize, init = u64(), u64(), u64()
        for _ in range(4):
            u64()
        modules.append((base, msize, init))
    bindings = []
    for _ in range(u64()):
        index, address, kind = u64(), u64(), u64()
        bindings.append((address, kind))
    segments = [(u64(), u64(), u64()) for _ in range(ns)]
    p += 128 * import_count  # names
    execs = [(a, s) for a, s, fl in segments if fl & 1]

    def executable(x):
        return any(a <= x < a + s for a, s in execs)

    print(f'entry {entry:x}')
    for a, s in execs:
        print(f'exec {a:x} {s:x}')
    for base, msize, init in modules:
        print(f'init {init:x}')
    for address, kind in bindings:
        if kind == 1 and executable(address):
            print(f'export {address:x}')
    for _ in range(nr):
        target, kind, value, addend = u64(), u64(), u64(), u64()
        if kind == 0 and executable(value):
            print(f'reloc {value:x} {target:x}')
        elif kind in (1, 2):
            print(f'import-slot {target:x}')


if __name__ == '__main__':
    main()
