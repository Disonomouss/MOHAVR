"""Read the text of MOHA's -log console window (UE3 shipping build: -log opens a console,
it does not write Launch.log).

Attaches to the game's console from THIS process (never from the caller's shell), reads the
whole screen buffer with ReadConsoleOutputCharacterW, detaches, prints the non-empty lines.

    python tools/read_game_console.py            # all lines
    python tools/read_game_console.py --tail 40
    python tools/read_game_console.py --out logs/run.txt

Only what is still in the buffer can be read -- the buffer is finite (see --info), so poll
during a run rather than reading once at the end.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class COORD(ctypes.Structure):
    _fields_ = [("X", wt.SHORT), ("Y", wt.SHORT)]


class SMALL_RECT(ctypes.Structure):
    _fields_ = [("Left", wt.SHORT), ("Top", wt.SHORT), ("Right", wt.SHORT), ("Bottom", wt.SHORT)]


class CSBI(ctypes.Structure):
    _fields_ = [("dwSize", COORD), ("dwCursorPosition", COORD), ("wAttributes", wt.WORD),
                ("srWindow", SMALL_RECT), ("dwMaximumWindowSize", COORD)]


def moha_pid() -> int:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq MOHA.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == "moha.exe":
            return int(parts[1])
    return 0


def read(pid: int):
    k32.FreeConsole()
    if not k32.AttachConsole(pid):
        raise OSError(f"AttachConsole({pid}) failed: {ctypes.get_last_error()}")
    try:
        GENERIC_RW = 0xC0000000
        h = k32.CreateFileW("CONOUT$", GENERIC_RW, 3, None, 3, 0, None)
        info = CSBI()
        if not k32.GetConsoleScreenBufferInfo(h, ctypes.byref(info)):
            raise OSError(f"GetConsoleScreenBufferInfo failed: {ctypes.get_last_error()}")
        width, rows = info.dwSize.X, info.dwCursorPosition.Y + 1
        buf = ctypes.create_unicode_buffer(width)
        lines = []
        for y in range(rows):
            n = wt.DWORD()
            k32.ReadConsoleOutputCharacterW(h, buf, width, COORD(0, y), ctypes.byref(n))
            lines.append(buf.value[:n.value].rstrip())
        k32.CloseHandle(h)
        return lines, (width, info.dwSize.Y, info.dwCursorPosition.Y)
    finally:
        k32.FreeConsole()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tail", type=int, default=0)
    ap.add_argument("--out")
    ap.add_argument("--info", action="store_true")
    a = ap.parse_args()
    pid = moha_pid()
    if not pid:
        print("MOHA is not running", file=sys.stderr)
        return 1
    lines, (w, h, cur) = read(pid)
    lines = [l for l in lines if l]
    if a.info:
        print(f"pid {pid}: buffer {w}x{h}, cursor row {cur}, {len(lines)} non-empty lines", file=sys.stderr)
    if a.tail:
        lines = lines[-a.tail:]
    text = "\n".join(lines)
    if a.out:
        open(a.out, "w", encoding="utf-8").write(text + "\n")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
