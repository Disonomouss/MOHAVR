"""Unwrap MOHA.exe's SteamStub Variant 2.1 when Steamless 3.1.0.5 cannot.

Steamless identifies MOHA.exe as SteamStub 2.1 (x86) and gets through steps 1-4, then crashes in
step 5 (ArgumentOutOfRangeException): this stub leaves the code section unencrypted and its
"code section VA" field is 0, so Steamless links an invalid section and indexes -1.

With nothing encrypted, unwrapping is just: restore the original entry point. This does exactly
what Steamless would, using Steamless's own artefacts and its own offset logic
(Variant21.x86 GetSteamDrmpOffsets, v3.1.0.5):

    Steamless.CLI.exe --dumppayload --dumpdrmp work\\MOHA.exe   # crashes, but writes:
        work\\MOHA.exe.payload   (decoded payload)
        work\\SteamDRMP.dll      (decoded DRM dll)
    python tools\\unwrap_steamstub21.py work\\MOHA.exe

Output: <exe>.unpacked.exe -- identical to the input except AddressOfEntryPoint (and a zeroed
PE checksum). The .bind section is kept but never executes (= Steamless --keepbind).
"""
import re
import struct
import sys
from pathlib import Path

# Steamless Variant21.x86 Step4 patterns, in its order; the third one switches to fallback offsets.
PATTERNS = [
    ("8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? "
     "8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8D ?? ?? ?? ?? ?? 05", False),
    ("8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? "
     "8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B", False),
    ("8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? A3 ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? A3 ?? ?? ?? ?? "
     "8B ?? ?? ?? ?? ?? A3 ?? ?? ?? ?? 8B", True),
]
NO_ENCRYPTION = 0x4  # Steamless DrmFlags.NoEncryption


def find(data: bytes, pattern: str) -> int:
    rx = b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in pattern.split())
    m = re.search(rx, data, re.DOTALL)
    return m.start() if m else -1


def sections(pe: bytes):
    e = struct.unpack_from("<I", pe, 0x3C)[0]
    n = struct.unpack_from("<H", pe, e + 6)[0]
    opt = struct.unpack_from("<H", pe, e + 20)[0]
    for i in range(n):
        o = e + 24 + opt + 40 * i
        name = pe[o:o + 8].rstrip(b"\0").decode()
        vsize, va, rsize, raw = struct.unpack_from("<IIII", pe, o + 8)
        yield name, va, vsize, raw, rsize


def main(exe_path: str) -> int:
    exe = Path(exe_path)
    pe = bytearray(exe.read_bytes())
    payload = (exe.parent / (exe.name + ".payload")).read_bytes()
    drmp = (exe.parent / "SteamDRMP.dll").read_bytes()

    for pat, fallback in PATTERNS:
        at = find(drmp, pat)
        if at != -1:
            break
    else:
        print("FAIL: no Steamless offset pattern matched in SteamDRMP.dll")
        return 1
    blk = drmp[at:at + 1024]
    off_flags = struct.unpack_from("<i", blk, 2)[0]
    off_oep = struct.unpack_from("<i", blk, 25 if fallback else 26)[0]
    off_code = struct.unpack_from("<i", blk, 36 if fallback else 38)[0]

    flags = struct.unpack_from("<I", payload, off_flags)[0]
    oep_va = struct.unpack_from("<I", payload, off_oep)[0]
    code_va = struct.unpack_from("<I", payload, off_code)[0]

    e = struct.unpack_from("<I", pe, 0x3C)[0]
    image_base = struct.unpack_from("<I", pe, e + 52)[0]
    old_ep = struct.unpack_from("<I", pe, e + 40)[0]
    oep_rva = oep_va - image_base
    print(f"pattern #{[p for p, _ in PATTERNS].index(pat) + 1} at 0x{at:X} (fallback={fallback})")
    print(f"flags=0x{flags:X} (no-encryption={bool(flags & NO_ENCRYPTION)})  code-section VA=0x{code_va:X}")
    print(f"image base 0x{image_base:X}; entry 0x{old_ep:X} -> OEP VA 0x{oep_va:X} (RVA 0x{oep_rva:X})")

    if not flags & NO_ENCRYPTION:
        print("FAIL: code section is encrypted -- this script only handles the unencrypted case")
        return 1
    owner = next((s for s in sections(pe) if s[1] <= oep_rva < s[1] + s[2]), None)
    if not owner or owner[0] != ".text":
        print(f"FAIL: OEP is not inside .text (owner={owner and owner[0]})")
        return 1
    fo = owner[3] + (oep_rva - owner[1])
    head = bytes(pe[fo:fo + 10])
    print(f"OEP bytes: {head.hex(' ')}")
    # MSVC 2005 CRT entry: call __security_init_cookie ; jmp __tmainCRTStartup
    if not (head[0] == 0xE8 and head[5] == 0xE9):
        print("WARN: OEP does not look like the MSVC 'call; jmp' CRT stub -- inspect before trusting")

    struct.pack_into("<I", pe, e + 40, oep_rva)   # AddressOfEntryPoint
    struct.pack_into("<I", pe, e + 88, 0)         # CheckSum
    out = exe.with_name(exe.name + ".unpacked.exe")
    out.write_bytes(pe)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "work/MOHA.exe"))
