"""MOHA.exe's large-address-aware flag (IMAGE_FILE_LARGE_ADDRESS_AWARE, 0x20 in the COFF header's Characteristics): the
game may then use up to 4 GB of address space on 64-bit Windows instead of 2 GB (D57: the first mission ran out). The
player's setup sets it on request (installer\\MOHAVR.iss) with a backup; this tool reads it, and sets or clears it for tests.

    python tools/laa.py status [exe]
    python tools/laa.py set [exe]      # keeps <exe>.mohavr-backup first (once)
    python tools/laa.py clear [exe]
"""
import os
import shutil
import struct
import sys

GAME = r'C:\Program Files (x86)\Steam\steamapps\common\Medal of Honor Airborne\UnrealEngine3\Binaries\MOHA.exe'
LAA = 0x0020


def offset(data):
    assert data[:2] == b'MZ', 'not an exe'
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[pe:pe + 4] == b'PE\0\0', 'no PE header'
    return pe + 4 + 18  # Machine, NumberOfSections, TimeDateStamp, PointerToSymbolTable, NumberOfSymbols, SizeOfOptionalHeader


def main(argv):
    cmd = argv[0] if argv else 'status'
    exe = argv[1] if len(argv) > 1 else GAME
    data = bytearray(open(exe, 'rb').read())
    o = offset(data)
    ch = struct.unpack_from('<H', data, o)[0]
    if cmd == 'status':
        print('%s: Characteristics 0x%04X -- large address aware: %s' % (exe, ch, 'yes' if ch & LAA else 'no'))
        return
    want = ch | LAA if cmd == 'set' else ch & ~LAA
    if want == ch:
        print('unchanged (0x%04X)' % ch)
        return
    if cmd == 'set' and not os.path.exists(exe + '.mohavr-backup'):
        shutil.copy2(exe, exe + '.mohavr-backup')
    struct.pack_into('<H', data, o, want)
    open(exe, 'r+b').write(data)
    print('%s: Characteristics 0x%04X -> 0x%04X' % (exe, ch, want))


if __name__ == '__main__':
    main(sys.argv[1:])
