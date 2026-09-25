"""Per-eye temporal instability of a side-by-side stereo capture series (M4 flicker check).

For consecutive frames, the mean absolute luminance difference of the LEFT half and of the RIGHT
half. Ambient motion (smoke, idle sway) affects both eyes alike, so a right/left ratio well above
1 means one eye is popping (e.g. shared occlusion state). Also reports the fraction of pixels whose
change exceeds a threshold ("pops").

    python tools/flicker_metric.py shot1.png shot2.png ...
"""
import sys

import numpy as np
from PIL import Image


def main(paths):
    frames = [np.asarray(Image.open(p).convert("L"), dtype=np.float32) for p in paths]
    h, w = frames[0].shape
    half = w // 2
    dl, dr, pl, pr = [], [], [], []
    for a, b in zip(frames, frames[1:]):
        d = np.abs(a - b)
        dl.append(d[:, :half].mean())
        dr.append(d[:, half:].mean())
        pl.append((d[:, :half] > 40).mean())
        pr.append((d[:, half:] > 40).mean())
    L, R = float(np.mean(dl)), float(np.mean(dr))
    print(f"frames {len(frames)}: mean |dLuma| left {L:.2f}  right {R:.2f}  ratio R/L {R / max(L, 1e-6):.2f}")
    print(f"pixels changing >40: left {100 * np.mean(pl):.2f}%  right {100 * np.mean(pr):.2f}%")
    print("per pair (L/R):", " ".join(f"{x:.1f}/{y:.1f}" for x, y in zip(dl, dr)))


if __name__ == "__main__":
    main(sys.argv[1:])
