"""Send test commands to MOHAVR-host's in-headset menu (the simulator can't press controller buttons).

    python tools/menu_cmd.py toggle
    python tools/menu_cmd.py goto=worldscale right right right   # world scale +15 (the menu open)
    python tools/menu_cmd.py toggle select                       # open, Recentre (the first item)

Commands: toggle up down left right select back, and goto=<key> (D81): the open menu's item with that key
selected in its tab (or goto=<tab name>: that tab's row), e.g. "toggle goto=recoil left". Keys: menu.cpp kItemKeys
(recentre worldscale height resetscale gunhand resolution pacing holsterrings reloadrings | turning movedir sticks vignette damageflash fireshake seated crouch chute |
gunfit reddot recoil scope scopezoom melee gunnade grabpickup mghands giveall | manualreload pouchreload reloadgrip
reloadspots rackeject rackkeep | holsters offnade nadehold offpistol offknife knifegrip freehand handfwd handup handin
foresize rings | hudplace hudshow hudlayout hudbacking hudwrist hudscreen). The host reads and deletes
%TEMP%\\MOHAVR\\host_cmd.txt once per XR frame and queues its lines, applying ONE per frame (one button press each).
The menu opens on General's first item, Recentre. Older test scripts (work/research/tests) count steps through the
layout before D81; use goto= instead.
"""
import os
import time
import sys
import tempfile
from pathlib import Path

VALID = {"toggle", "up", "down", "left", "right", "select", "back"}


def main(cmds):
    bad = [c for c in cmds if c not in VALID and not c.startswith("goto=")]
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
