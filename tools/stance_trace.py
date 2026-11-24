"""Read the per-Draw record of stance changes ("armik: drawn" lines) from a MOHAVR.log.

    python tools/stance_trace.py MOHAVR.log [-v]

Per run of consecutive frames: how far the gun moved and turned in the gun hand's frame, and the gun hand in its
controller's frame (with the largest step from one frame to the next). Both should stay constant. The mod records
the first six stance changes of every session (D113), and `mohavr arm trace` 3 s on demand. -v lists every frame.
"""
import math
import re
import sys

pat = re.compile(r'\[\s*([\d.]+) ms\].*drawn -- #(\d+) .*head z ([-\d.]+) act (-?\d+) comp ([-\d.]+)( two-handed)?; gun in hand ([-\d.]+) ([-\d.]+) '
                 r'([-\d.]+) / ([-\d.]+) ([-\d.]+) ([-\d.]+); hand in ctrl ([-\d.]+) ([-\d.]+) ([-\d.]+) / ([-\d.]+) ([-\d.]+) ([-\d.]+); lock ([-\d.]+)')


def ang(a, b):
    d = sum(x * y for x, y in zip(a, b)) / (math.sqrt(sum(x * x for x in a)) * math.sqrt(sum(x * x for x in b)) or 1.0)
    return math.degrees(math.acos(max(-1.0, min(1.0, d))))


for path in sys.argv[1:]:
    rows, marks = [], []
    for line in open(path, encoding='utf-8-sig', errors='replace'):
        m = pat.search(line)
        if m:
            g = m.groups()
            rows.append(dict(t=float(g[0]), headz=float(g[2]), act=int(g[3]), two=bool(g[5]), gp=[float(x) for x in g[6:9]], gx=[float(x) for x in g[9:12]],
                             hp=[float(x) for x in g[12:15]], hx=[float(x) for x in g[15:18]], lock=float(g[18])))
        elif 'crouch: the' in line:
            marks.append((len(rows), line[line.index('crouch:'):].strip()[:60]))
    print('==', path.replace('\\', '/').split('/')[-1], len(rows), 'frames')
    if not rows:
        continue
    # segments of consecutive frames (the recorder's windows are apart)
    seg, segs = [rows[0]], []
    for r in rows[1:]:
        if r['t'] - seg[-1]['t'] > 200:
            segs.append(seg)
            seg = []
        seg.append(r)
    segs.append(seg)
    for seg in segs:
        b = seg[0]
        gmax = max(math.dist(r['gp'], b['gp']) for r in seg)
        gang = max(ang(r['gx'], b['gx']) for r in seg)
        hmax = max(math.dist(r['hp'], b['hp']) for r in seg)
        hang = max(ang(r['hx'], b['hx']) for r in seg)
        gstep = max([math.dist(a['gp'], c['gp']) for a, c in zip(seg, seg[1:])] or [0])
        gastep = max([ang(a['gx'], c['gx']) for a, c in zip(seg, seg[1:])] or [0])
        hstep = max([math.dist(a['hp'], c['hp']) for a, c in zip(seg, seg[1:])] or [0])
        acts = sorted(set(r['act'] for r in seg))
        print('   %7.0f ms, %3d frames, acts %s%s: gun in hand moved %.2f (step %.2f) turned %.2f deg (step %.2f); hand in ctrl moved %.2f (step %.2f) turned %.2f deg'
              % (seg[0]['t'], len(seg), acts, ' two-handed' if any(r['two'] for r in seg) else '', gmax, gstep, gang, gastep, hmax, hstep, hang))
        if '-v' in sys.argv:
            for r in seg:
                print('      %7.0f act %2d lock %.2f headz %7.1f | gun %6.2f %6.2f %6.2f %5.2f deg | hand %6.2f %6.2f %6.2f %5.2f deg' % (
                    r['t'] - seg[0]['t'], r['act'], r['lock'], r['headz'] - b['headz'], r['gp'][0] - b['gp'][0], r['gp'][1] - b['gp'][1], r['gp'][2] - b['gp'][2],
                    ang(r['gx'], b['gx']), r['hp'][0] - b['hp'][0], r['hp'][1] - b['hp'][1], r['hp'][2] - b['hp'][2], ang(r['hx'], b['hx'])))
