"""Set the OpenXR Simulator's head pose (the same command file its MCP server's set_head_pose writes).

    python tools/sim_pose.py --yaw 30                 # degrees; position defaults to the simulator's (0, 1.7, 0)
    python tools/sim_pose.py --pitch 20 --roll 10
    python tools/sim_pose.py --x 0.1 --y 1.7 --z 0    # metres

The runtime polls %LOCALAPPDATA%\\OpenXR-Simulator\\head_pose_command.json. Sign conventions are
the simulator's; tools/harness tests print what the game logged so they can be checked.
"""
import argparse
import json
import math
import os
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--x", type=float, default=0.0)
    ap.add_argument("--y", type=float, default=1.7)
    ap.add_argument("--z", type=float, default=0.0)
    ap.add_argument("--yaw", type=float, default=0.0)
    ap.add_argument("--pitch", type=float, default=0.0)
    ap.add_argument("--roll", type=float, default=None)
    a = ap.parse_args()
    payload = {"x": a.x, "y": a.y, "z": a.z, "yaw": math.radians(a.yaw), "pitch": math.radians(a.pitch)}
    if a.roll is not None:
        payload["roll"] = math.radians(a.roll)
    d = Path(os.environ["LOCALAPPDATA"]) / "OpenXR-Simulator"
    d.mkdir(parents=True, exist_ok=True)
    tmp = d / "head_pose_command.json.tmp"
    tmp.write_text(json.dumps(payload))
    os.replace(tmp, d / "head_pose_command.json")
    print(f"head pose command: {payload}")


if __name__ == "__main__":
    main()
