"""Screen-state checks for the harness (D9: the engine writes no log, so progress comes from
screenshots).

Each named check is a reference crop plus the box it was cut from, in 1920x1080 client
coordinates (the harness always runs windowed at 1920x1080). A screenshot "matches" when the
mean absolute difference over that box is below the check's threshold. Pick boxes on static
UI (the metal frame, titles), never on the animated menu background.

    python tools/screen_match.py shot.png                 # score every check, name the best
    python tools/screen_match.py shot.png mainmenu        # exit 0 if it matches, 1 if not
    python tools/screen_match.py --add NAME shot.png X0 Y0 X1 Y1 [--thr 12]   # new check
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

REF_DIR = Path(__file__).with_name("harness-ref")
INDEX = REF_DIR / "checks.json"


def load_index():
    return json.loads(INDEX.read_text()) if INDEX.exists() else {}


def score(shot: Image.Image, check: dict) -> float:
    ref = np.asarray(Image.open(REF_DIR / check["ref"]).convert("L"), dtype=np.float32)
    x0, y0, x1, y1 = check["box"]
    cur = np.asarray(shot.convert("L").crop((x0, y0, x1, y1)), dtype=np.float32)
    if cur.shape != ref.shape:
        return 255.0
    return float(np.abs(cur - ref).mean())


def main(argv):
    if argv and argv[0] == "--add":
        name, shot, *box = argv[1:7]
        thr = float(argv[argv.index("--thr") + 1]) if "--thr" in argv else 12.0
        x0, y0, x1, y1 = map(int, box)
        REF_DIR.mkdir(exist_ok=True)
        Image.open(shot).convert("RGB").crop((x0, y0, x1, y1)).save(REF_DIR / f"{name}.png")
        idx = load_index()
        idx[name] = {"ref": f"{name}.png", "box": [x0, y0, x1, y1], "thr": thr}
        INDEX.write_text(json.dumps(idx, indent=2) + "\n")
        print(f"added check '{name}' box {x0},{y0}-{x1},{y1} thr {thr}")
        return 0

    shot = Image.open(argv[0])
    if shot.size != (1920, 1080):
        # UE3's UI scales with the resolution, so any 16:9 frame compares after resizing.
        w, h = shot.size
        if abs(w / h - 16 / 9) > 0.01:
            print(f"screenshot is {shot.size}; checks are for 16:9 (1920x1080)")
            print("state: unknown")
            return 2
        shot = shot.convert("RGB").resize((1920, 1080), Image.BILINEAR)
    idx = load_index()
    if len(argv) > 1:
        c = idx[argv[1]]
        s = score(shot, c)
        ok = s <= c["thr"]
        print(f"{argv[1]}: {s:.1f} (thr {c['thr']}) -> {'MATCH' if ok else 'no'}")
        return 0 if ok else 1
    best = None
    for name, c in idx.items():
        s = score(shot, c)
        ok = s <= c["thr"]
        print(f"{name:24} {s:6.1f}  thr {c['thr']:5.1f}  {'MATCH' if ok else ''}")
        if ok and (best is None or s < best[1]):
            best = (name, s)
    # Loading screens are plain black (measured 2026-09-25); a named check always wins.
    mean = float(np.asarray(shot.convert("L"), dtype=np.float32).mean())
    print(f"{'(mean luminance)':24} {mean:6.1f}")
    state = best[0] if best else ("black" if mean < 8.0 else "unknown")
    print(f"state: {state}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
