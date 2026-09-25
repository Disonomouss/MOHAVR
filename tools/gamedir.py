"""Resolve the Medal of Honor Airborne install directory for the analysis scripts.

Order: the MOHAVR_GAMEDIR environment variable, then tools/gamedir.txt, then give up with
a clear message. One edit point when the project moves to another machine.
"""
import os
import pathlib


def game_dir() -> pathlib.Path:
    env = os.environ.get("MOHAVR_GAMEDIR")
    if env:
        return pathlib.Path(env)

    cfg = pathlib.Path(__file__).with_name("gamedir.txt")
    if cfg.exists():
        for line in cfg.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                return pathlib.Path(line)

    raise SystemExit(
        "Cannot find the Medal of Honor Airborne install directory.\n"
        "Set the MOHAVR_GAMEDIR environment variable, or put the path on a line in\n"
        "tools/gamedir.txt."
    )


def game_exe() -> pathlib.Path:
    return game_dir() / "UnrealEngine3" / "Binaries" / "MOHA.exe"
