"""Capture what the OpenXR Simulator composited for the headset (both eyes, all layers) -- the
headset's view, unlike tools/harness.ps1 shot (the game's backbuffer).

    python tools/sim_shot.py logs/shots/x.png [--eye both|left|right] [--layer projection|quad|all] [--timeout 5]

Uses the simulator's own request file (the same one its MCP server's screenshot tool writes).
"""
import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path

SIM = Path(os.environ["LOCALAPPDATA"]) / "OpenXR-Simulator"
REQUEST = SIM / "screenshot_request.json"
OUTPUTS = [SIM / n for n in ("screenshot.png", "screenshot.bmp", "screenshot.jpg", "screenshot.jpeg")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--eye", default="both")
    ap.add_argument("--layer", default="projection", help="projection (the game's image), quad, or all (with the host's quads: menu, reticle)")
    ap.add_argument("--timeout", type=float, default=5.0)
    a = ap.parse_args()
    SIM.mkdir(parents=True, exist_ok=True)
    for p in OUTPUTS:
        p.unlink(missing_ok=True)
    t0 = time.time()
    REQUEST.write_text(json.dumps({"timestamp": t0, "eye": a.eye, "include_ui": True, "layer": a.layer, "requested_by": "sim_shot"}))
    while time.time() - t0 < a.timeout:
        for p in OUTPUTS:
            if p.exists() and p.stat().st_size > 0:
                time.sleep(0.2)  # let the writer finish
                out = Path(a.out)
                if p.suffix != out.suffix:
                    from PIL import Image
                    Image.open(p).save(out)
                else:
                    shutil.copyfile(p, out)
                p.unlink(missing_ok=True)
                REQUEST.unlink(missing_ok=True)
                print(f"saved {out}")
                return 0
        time.sleep(0.1)
    print("sim_shot: no screenshot from the simulator (is a session running?)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
