"""Drive MOHAVR's virtual Xbox pad from a script (Input.Controllers=1; the simulator has no controllers).

    python tools/pad_cmd.py --ly 1 --dur 2                 # left stick forward for 2 s (Xbox state)
    python tools/pad_cmd.py --rx 1 --dur 0.5               # turn right
    python tools/pad_cmd.py --rt 1 --dur 0.3               # fire
    python tools/pad_cmd.py --buttons start --dur 0.2      # pause menu
    python tools/pad_cmd.py --raw --press b --dur 0.2      # CONTROLLER input: the B button (-> reload via [Controls])
    python tools/pad_cmd.py --raw --ry -1 --dur 0.2        # controller: flick the right stick down (-> crouch)
    python tools/pad_cmd.py --seq "ly=1 dur=1" "dur=0.5" "buttons=y dur=0.2"   # several states in order

The host reads and deletes %TEMP%\\MOHAVR\\pad_cmd.txt and plays each line for its duration; while a test
state plays it replaces the controllers entirely. Without --raw the values are an Xbox pad state (buttons: a b x
y lb rb ls rs start back up down left right); with --raw they are controller input that goes through the
mapping (press: a b x y lgrip rgrip lthumb rthumb menu). Sticks -1..1 (y up = +), triggers 0..1.
"""
import argparse
import os
import tempfile
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    for k in ("lx", "ly", "rx", "ry", "lt", "rt"):
        ap.add_argument(f"--{k}", type=float, default=0.0)
    ap.add_argument("--buttons", default="")
    ap.add_argument("--press", default="")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--dur", type=float, default=0.5)
    ap.add_argument("--seq", nargs="+", help="raw lines, e.g. 'ly=1 dur=2' or 'raw=1 press=b dur=0.2'")
    a = ap.parse_args()
    if a.seq:
        lines = a.seq
    else:
        parts = ["raw=1"] if a.raw else []
        parts += [f"{k}={getattr(a, k)}" for k in ("lx", "ly", "rx", "ry", "lt", "rt") if getattr(a, k)]
        if a.buttons:
            parts.append(f"buttons={a.buttons}")
        if a.press:
            parts.append(f"press={a.press}")
        parts.append(f"dur={a.dur}")
        lines = [" ".join(parts)]
    d = Path(tempfile.gettempdir()) / "MOHAVR"
    d.mkdir(exist_ok=True)
    tmp = d / "pad_cmd.txt.tmp"
    tmp.write_text("\n".join(lines) + "\n")
    os.replace(tmp, d / "pad_cmd.txt")
    print("sent:", " | ".join(lines))


if __name__ == "__main__":
    main()
