"""Check the host's desktop mirror (Bridge.Mirror) from outside, the way the player sees it.

    python tools/mirror_check.py [--out logs/shots/x.png]

Prints JSON: whether the "MOHAVR mirror" window exists / is visible, its rect vs the game's client
area, the screen pixels over the game's client area (mean luma, fraction of near-white pixels --
the bare 9On12 window is white), and which window is hit at the game's centre (click-through: must
be the game, not the mirror).
"""
import argparse
import ctypes
import json
from ctypes import wintypes

from PIL import ImageGrab, ImageStat

u = ctypes.windll.user32
u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2: physical pixels


def rect_of(h, client):
    r = wintypes.RECT()
    if client:
        u.GetClientRect(h, ctypes.byref(r))
        p = wintypes.POINT(0, 0)
        u.ClientToScreen(h, ctypes.byref(p))
        return [p.x, p.y, p.x + r.right, p.y + r.bottom]
    u.GetWindowRect(h, ctypes.byref(r))
    return [r.left, r.top, r.right, r.bottom]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    a = ap.parse_args()
    game = u.FindWindowW(None, "Medal of Honor Airborne")
    mirror = u.FindWindowW("MOHAVR_Mirror", None)
    res = {"game": bool(game), "mirror": bool(mirror),
           "mirrorVisible": bool(mirror and u.IsWindowVisible(mirror)),
           "gameForeground": bool(game) and u.GetForegroundWindow() == game}
    if game:
        gr = rect_of(game, True)
        res["gameClient"] = gr
        if mirror:
            res["mirrorRect"] = rect_of(mirror, False)
        shot = ImageGrab.grab(bbox=tuple(gr), all_screens=True)
        if a.out:
            shot.save(a.out)
        img = shot.convert("L")
        res["meanLuma"] = round(ImageStat.Stat(img).mean[0], 1)
        hist = img.histogram()
        res["whiteFraction"] = round(sum(hist[245:]) / max(1, img.width * img.height), 3)
        cx, cy = (gr[0] + gr[2]) // 2, (gr[1] + gr[3]) // 2
        hit = u.WindowFromPoint(wintypes.POINT(cx, cy))
        root = u.GetAncestor(hit, 2)  # GA_ROOT
        res["hitIsGame"] = root == game
        res["hitIsMirror"] = bool(mirror) and root == mirror
    print(json.dumps(res))


if __name__ == "__main__":
    main()
