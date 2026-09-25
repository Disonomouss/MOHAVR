"""Address-space budget of the running MOHA.exe (32-bit, not large-address-aware: 2 GB user space).

Walks the target's address space with VirtualQueryEx from outside the process and reports
what matters for fitting D3D9On12 + OpenXR in (ENGINE-NOTES 2, 5c):
  committed, reserved, free, and the LARGEST FREE CONTIGUOUS BLOCKS -- big allocations
  (heaps, swapchains, DLL images) need contiguous space, so fragmentation matters more than
  the free total.

    python tools/vmmap.py            # summary
    python tools/vmmap.py --json     # one JSON line (for logs / comparisons)
    python tools/vmmap.py --modules  # also the biggest image (DLL) mappings
"""
import ctypes
import ctypes.wintypes as wt
import json
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

MEM_COMMIT, MEM_RESERVE, MEM_FREE = 0x1000, 0x2000, 0x10000
MEM_IMAGE, MEM_MAPPED, MEM_PRIVATE = 0x1000000, 0x40000, 0x20000
USER_TOP = 0x7FFF0000  # 2 GB user space for a non-LAA 32-bit process


class MBI(ctypes.Structure):  # MEMORY_BASIC_INFORMATION as seen by a 64-bit caller
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_uint64), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


def moha_pid() -> int:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq MOHA.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == "moha.exe":
            return int(parts[1])
    return 0


def walk(pid: int):
    h = k32.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION | VM_READ
    if not h:
        raise OSError(f"OpenProcess failed: {ctypes.get_last_error()}")
    regions = []
    addr = 0x10000
    mbi = MBI()
    try:
        while addr < USER_TOP:
            if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            size = min(mbi.RegionSize, USER_TOP - mbi.BaseAddress)
            regions.append((mbi.BaseAddress, size, mbi.State, mbi.Type, mbi.AllocationBase))
            addr = mbi.BaseAddress + mbi.RegionSize
        mods = {}
        for base, size, state, typ, alloc in regions:
            if typ == MEM_IMAGE and state != MEM_FREE:
                mods.setdefault(alloc, 0)
                mods[alloc] += size
        names = {}
        for alloc in mods:
            buf = ctypes.create_unicode_buffer(260)
            if psapi.GetMappedFileNameW(h, ctypes.c_void_p(alloc), buf, 260):
                names[alloc] = buf.value.rsplit("\\", 1)[-1]
    finally:
        k32.CloseHandle(h)
    return regions, mods, names


def summarize(regions):
    mb = 1024 * 1024
    s = {"committed": 0, "reserved": 0, "free": 0, "image": 0, "mapped": 0, "private": 0}
    free_blocks = []
    for base, size, state, typ, _ in regions:
        if state == MEM_FREE:
            s["free"] += size
            free_blocks.append((size, base))
            continue
        s["committed" if state == MEM_COMMIT else "reserved"] += size
        s["image" if typ == MEM_IMAGE else "mapped" if typ == MEM_MAPPED else "private"] += size
    free_blocks.sort(reverse=True)
    out = {k: round(v / mb, 1) for k, v in s.items()}
    out["used"] = round((s["committed"] + s["reserved"]) / mb, 1)
    out["largest_free"] = [round(sz / mb, 1) for sz, _ in free_blocks[:5]]
    out["largest_free_at"] = [f"0x{b:08X}" for _, b in free_blocks[:5]]
    return out


def main():
    pid = moha_pid()
    if not pid:
        print("MOHA is not running", file=sys.stderr)
        return 1
    regions, mods, names = walk(pid)
    s = summarize(regions)
    s["pid"] = pid
    if "--json" in sys.argv:
        print(json.dumps(s))
    else:
        print(f"MOHA pid {pid}: used {s['used']} MB of 2048 (committed {s['committed']}, reserved {s['reserved']}), "
              f"free {s['free']} MB")
        print(f"  by type: image {s['image']}, mapped {s['mapped']}, private {s['private']} MB")
        print(f"  largest free blocks (MB): {s['largest_free']}  at {s['largest_free_at']}")
    if "--modules" in sys.argv:
        for alloc, size in sorted(mods.items(), key=lambda kv: -kv[1])[:15]:
            print(f"  {size / 1048576:7.1f} MB  0x{alloc:08X}  {names.get(alloc, '?')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
