"""Send test commands to MOHAVR-host's in-headset menu (the simulator can't press controller buttons).

    python tools/menu_cmd.py toggle
    python tools/menu_cmd.py right right right      # world scale +15 (when World scale is selected)
    python tools/menu_cmd.py toggle down down down select   # open, move to Recentre, select it

Commands: toggle up down left right select back. The host reads and deletes
%TEMP%\\MOHAVR\\host_cmd.txt once per XR frame and queues its lines, applying ONE per frame
(one button press each). Items: World scale, Height, Turning, Recentre, Reset world scale, Close;
the menu opens on World scale.
"""
import os
import time
import sys
import tempfile
from pathlib import Path

VALID = {"toggle", "up", "down", "left", "right", "select", "back"}


def main(cmds):
    bad = [c for c in cmds if c not in VALID]
    if bad:
        print(f"unknown command(s): {bad}; valid: {sorted(VALID)}")
        return 2
    d = Path(tempfile.gettempdir()) / "MOHAVR"
    d.mkdir(exist_ok=True)
    tmp = d / "host_cmd.txt.tmp"
    for _ in range(200):  # the host takes the file once a frame: don't replace an unread command (see pad_cmd.py)
        if not (d / "host_cmd.txt").exists():
            break
        time.sleep(0.01)
    tmp.write_text("\n".join(cmds) + "\n")
    os.replace(tmp, d / "host_cmd.txt")
    print("sent:", " ".join(cmds))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
