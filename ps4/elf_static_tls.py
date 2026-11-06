#!/usr/bin/env python3
# The PS4's loader runs an eboot only with DF_STATIC_TLS in its DT_FLAGS: OpenOrbis' lld writes it, the host's LLD (23)
# never does for an executable, and its eboot died before main. SW_THINLTO links with the host's lld and '-z now' (which
# reserves DT_FLAGS and DT_FLAGS_1); this turns them into what OpenOrbis' lld writes: STATIC_TLS, and PIE alone.
#   elf_static_tls.py <linked ELF>
import struct, sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
e_phoff, = struct.unpack_from('<Q', b, 0x20); e_phentsize, e_phnum = struct.unpack_from('<HH', b, 0x36)
dyn = None
for i in range(e_phnum):
    o = e_phoff + i * e_phentsize
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from('<IIQQQQ', b, o)
    if p_type == 2: dyn = (p_offset, p_filesz)
assert dyn, 'no PT_DYNAMIC'
done = set()
for o in range(dyn[0], dyn[0] + dyn[1], 16):
    tag, val = struct.unpack_from('<qQ', b, o)
    if tag == 30:  # DT_FLAGS: BIND_NOW -> STATIC_TLS (what OpenOrbis' lld writes)
        struct.pack_into('<qQ', b, o, tag, 0x10); done.add('flags')
    elif tag == 0x6ffffffb:  # DT_FLAGS_1: NOW|PIE -> PIE
        struct.pack_into('<qQ', b, o, tag, val & ~0x1); done.add('flags1')
    elif tag == 0: break
assert done == {'flags', 'flags1'}, done
open(p, 'wb').write(b)
print('patched', p)
