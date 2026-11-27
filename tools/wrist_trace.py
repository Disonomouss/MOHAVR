"""The drawn wrists against the eye, from "armik: drawn" lines (`mohavr arm trace`, or the stance recorder).

    python tools/wrist_trace.py MOHAVR.log [more.txt ...]

Per file: how far the first wrist got from where it started, and each wrist's largest step in one frame (cm).
A jump of the gun in the hands shows here; the trace's "hand in ctrl" does not show a two-handed turn, because
that frame is the gun's and turns with it (D116).
"""
import math
import re
import sys

PAT = re.compile(r'wrists ([-\d.]+) ([-\d.]+) ([-\d.]+) / ([-\d.]+) ([-\d.]+) ([-\d.]+);')

for path in sys.argv[1:]:
    with open(path, encoding='utf-8-sig', errors='replace') as f:
        rows = [[float(x) for x in m.groups()] for m in (PAT.search(line) for line in f) if m]
    if len(rows) < 2:
        print('%s: %d "armik: drawn" line(s) -- nothing to compare' % (path, len(rows)))
        continue
    a = max(math.dist(r[:3], q[:3]) for r, q in zip(rows, rows[1:]))
    b = max(math.dist(r[3:], q[3:]) for r, q in zip(rows, rows[1:]))
    far = max(math.dist(r[:3], rows[0][:3]) for r in rows)
    print('%s: %d frames -- the first wrist got %.1f cm from its start, largest step in a frame %.2f cm; the second wrist\'s largest step %.2f cm'
          % (path, len(rows), far, a, b))
