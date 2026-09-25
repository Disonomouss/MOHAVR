"""Run a console command inside MOHA through the mod (Debug.GameCommands=1; tests only).

    python tools/game_cmd.py Suicide              # the PlayerController's exec: die -> checkpoint reload
    python tools/game_cmd.py "HideWeapon 1"

The mod reads and deletes %TEMP%\\MOHAVR\\game_cmd.txt twice a second on the game thread and runs each
line through ULocalPlayer::Exec (the console's path); MOHAVR.log says whether the game handled it.
"""
import os
import sys
import tempfile
from pathlib import Path


def main(args):
    if not args:
        print(__doc__)
        return 2
    d = Path(tempfile.gettempdir()) / "MOHAVR"
    d.mkdir(exist_ok=True)
    tmp = d / "game_cmd.txt.tmp"
    tmp.write_text(" ".join(args) + "\n", encoding="utf-8")
    os.replace(tmp, d / "game_cmd.txt")
    print("sent:", " ".join(args))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
