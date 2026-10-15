#!/usr/bin/env python3
"""Reads a testgen .vec file. usage: vecdump.py file.vec [form substring] [max cases]"""
import struct, sys

STATE = 1216  # sizeof(State): 16*8 + 8 + 56 + 512 + 256 + 256
IN = 64  # State is 64-byte aligned inside Case
CASE = IN + 2 * STATE

def forms(path):
    d = open(path, 'rb').read()
    assert d[:6] == b'BBVEC1'
    n = struct.unpack_from('<I', d, 8)[0]
    p = 12
    for _ in range(n):
        klen = struct.unpack_from('<H', d, p)[0]; p += 2
        key = d[p:p + klen].decode(); p += klen
        count = struct.unpack_from('<I', d, p)[0]; p += 4
        cases = []
        for _ in range(count):
            c = d[p:p + CASE]; p += CASE
            length, faulted = c[15], c[16]
            cases.append((c[:length], faulted, c[IN:IN + STATE], c[IN + STATE:]))
        yield key, cases

if __name__ == '__main__':
    want = sys.argv[2] if len(sys.argv) > 2 else ''
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    names = {4: 'SIGILL', 5: 'SIGTRAP', 8: 'SIGFPE', 10: 'SIGBUS', 11: 'SIGSEGV'}
    for key, cases in forms(sys.argv[1]):
        if want not in key:
            continue
        print(key)
        for b, f, i, o in cases[:limit]:
            gi = struct.unpack_from('<16Q', i); go = struct.unpack_from('<16Q', o)
            fi = struct.unpack_from('<Q', i, 128)[0]; fo = struct.unpack_from('<Q', o, 128)[0]
            print(f"  {b.hex():30} {names.get(f, f) if f else 'ok':8} rax {gi[0]:x}->{go[0]:x} flags {fi:x}->{fo:x}")
