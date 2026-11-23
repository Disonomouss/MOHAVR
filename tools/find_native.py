"""Find a script native's exec function in the unpacked exe by its native-table name (UTF-16).

    python tools/find_native.py intAMOHAPlayerPawnexecPerformLandingDetection [more names] [--dis N]

Prints the name's address, the table entry pointing at it, the function's address and its first 64 bytes; --dis N
disassembles N bytes (capstone). Analysis only (work/MOHA.exe.unpacked.exe; standing rule 2).
"""
import struct
import sys
from pathlib import Path


def main(argv):
    names = [a for a in argv if not a.startswith("--")]
    dis = 0
    if "--dis" in argv:
        i = argv.index("--dis")
        dis = int(argv[i + 1], 0)
        names = [a for a in argv[:i] + argv[i + 2:] if not a.startswith("--")]
    if not names:
        print(__doc__)
        return 2
    data = (Path(__file__).resolve().parent.parent / "work" / "MOHA.exe.unpacked.exe").read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        vs, va, rs, ro = struct.unpack_from("<IIII", data, o + 8)
        secs.append((va, vs, ro, rs))

    def off2va(off):
        for va, vs, ro, rs in secs:
            if ro <= off < ro + rs:
                return base + va + off - ro
        return None

    def va2off(v):
        for va, vs, ro, rs in secs:
            if base + va <= v < base + va + rs:
                return v - base - va + ro
        return None

    for s in names:
        b = s.encode("utf-16-le") + b"\0\0"
        i = data.find(b)
        if i < 0:
            print(s, "not found")
        while i >= 0:
            sva = off2va(i)
            print(s, "string at", hex(sva))
            p = struct.pack("<I", sva)
            j = data.find(p)
            while j >= 0:
                fn = struct.unpack_from("<I", data, j + 4)[0]
                print("  entry at", hex(off2va(j)), "-> func", hex(fn))
                fo = va2off(fn)
                if fo:
                    print("   bytes", data[fo:fo + 64].hex(" "))
                    if dis:
                        import capstone
                        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
                        for ins in md.disasm(data[fo:fo + dis], fn):
                            print("   ", hex(ins.address), ins.bytes.hex(), ins.mnemonic, ins.op_str)
                j = data.find(p, j + 1)
            i = data.find(b, i + 1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
