"""Scopes (SCOPE-DESIGN 2.2): where each scoped gun's scope tube is, in gun mesh space at the idle.

For every gun with a scope part (the Springfield's upgrade_02_hide_scope, the G43's upgrade_02_scope, the StG44's
altFire_scope), from the umodel psk exports tools/melee_points.py reads (same conventions: engine mesh space, +X = the
gun's left, +Y = down, +Z = towards the muzzle):
  * the tube's sections square to the bore (the scope bone's triangles cut by planes along the barrel axis): their
    centres and sizes along the tube;
  * the eyepiece: the section nearest the rear end (its centre and radius), and the objective: likewise at the front.
The M18's telescope is part of its body bone (RootOffset): a row may name a part of its bone -- a box in mesh space, the
largest connected piece of the bone's triangles inside it (the box alone would take two screw heads too), a wider top
ring, and the eyepiece's glass (the back-facing disc recessed in the eyecup: the published radius is the glass / 0.9, the
host's glass_, so the lens is the drawn glass). work/research/m18scope.

    python tools/scope_points.py              # the report
    python tools/scope_points.py --inc > src/mohavr/scope_points.inc
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import melee_points as mp  # noqa: E402

# attachment class -> (mesh, the scope bone(s), the upgrade level from which the scope is on[, a part of the bone])
SCOPES = [
    ('Attachment_Springfield', 'US_1903sniper_Rigged', ['upgrade_02_hide_scope'], -1),
    ('Attachment_G43', 'DE_G43_Rigged', ['upgrade_02_scope'], 1),
    ('Attachment_Stg44', 'DE_STG44_Rigged', ['altFire_scope'], 2),
    ('Attachment_M18RecoillessRifle', 'US_M18recoilless_Rigged', ['RootOffset'], -1,
     dict(box=((2.4, 7.4), (-16.1, -11.1), (-9.1, 23.4)), ring=5.0, glass=True)),
]
END = 1.0  # units in from each end: the section used for the eyepiece / objective
TUBE = 3.0  # units: a section's top ring (the tube on its mount) -- the hits within this of the section's top


def top_section(gun, bones, s0):
    """The section square to the bore at s0, kept to its top ring (the scope tube, not the mount below it): centre,
    height, width -- or None."""
    hits = []
    for t in gun.tris:
        if gun.owner[t[0]] not in bones:
            continue
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            pa, pb = gun.pts[a], gun.pts[b]
            da, db = mp.dot(pa, gun.axis) - s0, mp.dot(pb, gun.axis) - s0
            if (da <= 0 < db) or (db <= 0 < da):
                u = da / (da - db)
                hits.append(mp.add(pa, mp.mul(mp.sub(pb, pa), u)))
    if not hits:
        return None
    top = min(p[1] for p in hits)  # +Y is down: the top is the smallest Y
    ring = [p for p in hits if p[1] <= top + TUBE]
    c = tuple((min(p[k] for p in ring) + max(p[k] for p in ring)) / 2 for k in range(3))
    return c, max(p[1] for p in ring) - min(p[1] for p in ring), max(p[0] for p in ring) - min(p[0] for p in ring)


def analyse(gun, bone_names, quiet):
    bones = [gun.names.index(n) for n in bone_names]
    vs = [gun.pts[i] for i in range(len(gun.pts)) if gun.owner[i] in bones]
    a = gun.axis
    s = [mp.dot(p, a) for p in vs]
    s0, s1 = min(s), max(s)
    secs = []
    n = 24
    for k in range(n + 1):
        sk = s0 + END + (s1 - s0 - 2 * END) * k / n
        r = top_section(gun, set(bones), sk)
        if r:
            secs.append((sk, r))
    if not quiet:
        print('\n== %s: %s (%d points), along the bore s %.1f..%.1f' % (gun.g['mesh'], ', '.join(bone_names), len(vs), s0, s1))
        for sk, (c, h, t) in secs:
            print('   s %6.1f: centre %s  height (Y) %5.2f  width (X) %5.2f' % (sk, mp.v3('6.2f', c), h, t))
    rear, front = secs[0], secs[-1]
    # the eyepiece's radius: the rear section's half height and half width (an eyepiece bell: the larger)
    r_eye = 0.5 * max(rear[1][1], rear[1][2])
    r_obj = 0.5 * max(front[1][1], front[1][2])
    eye = mp.add(rear[1][0], mp.mul(a, (s0 - rear[0])))   # moved to the very rear end along the axis
    obj = mp.add(front[1][0], mp.mul(a, (s1 - front[0])))
    if not quiet:
        print('   EYEPIECE %s r %.2f   OBJECTIVE %s r %.2f   tube %.1f units; bore axis %s; the eyepiece is %.2f above'
              ' the bore line' % (mp.v3('.2f', eye), r_eye, mp.v3('.2f', obj), r_obj, s1 - s0, mp.v3('.4f', a),
                                  -(eye[1] - gun.bore_point(mp.dot(eye, a))[1])))
    return eye, obj, r_eye, bones[0]


def select_part(gun, bone_name, part):
    """Keep only the largest connected piece of the bone's triangles inside part['box'] (the other points' owner -1)."""
    bi = gun.names.index(bone_name)
    box = part['box']
    inbox = lambda p: all(box[k][0] < p[k] < box[k][1] for k in range(3))
    cand = [t for t in gun.tris if gun.owner[t[0]] == bi and all(inbox(gun.pts[v]) for v in t)]
    key = lambda v: tuple(round(c, 2) for c in gun.pts[v])
    par = {}

    def find(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x

    for t in cand:
        ks = [key(v) for v in t]
        for k in ks:
            par.setdefault(k, k)
        for x, y in ((ks[0], ks[1]), (ks[1], ks[2])):
            rx, ry = find(x), find(y)
            if rx != ry:
                par[rx] = ry
    groups = {}
    for t in cand:
        groups.setdefault(find(key(t[0])), []).append(t)
    keep = max(groups.values(), key=len)
    inpiece = {v for t in keep for v in t}
    gun.tris = keep
    gun.owner = [o if i in inpiece else -1 for i, o in enumerate(gun.owner)]


def glass_radius(gun, eye, tris):
    """The eyepiece's glass: the faces within 25 deg of facing back within 1 unit of the rear end; its radius (0 none)."""
    a = gun.axis
    s0 = mp.dot(eye, a)
    r = 0.0
    for t in tris:
        A, B, C = (gun.pts[v] for v in t)
        n = mp.unit(mp.mul(mp.cross(mp.sub(B, A), mp.sub(C, A)), gun.outward))
        if mp.dot(n, a) > -math.cos(math.radians(25)):
            continue
        if mp.dot(mp.mul(mp.add(mp.add(A, B), C), 1 / 3), a) - s0 > 1.0:
            continue
        for p in (A, B, C):
            q = mp.sub(p, eye)
            r = max(r, mp.norm(mp.sub(q, mp.mul(a, mp.dot(q, a)))))
    return r


def main(argv):
    root = os.path.join(mp.REPO, 'work', 'research', 'reload')
    mp.PSKDIR = os.path.join(root, 'psk', 'Var_Flk_P', 'SkeletalMesh3')
    mp.PSADIR = os.path.join(root, 'psa', 'Var_Flk_P', 'AnimSet')
    quiet = '--inc' in argv
    rows = []
    global TUBE
    for row in SCOPES:
        att, mesh, bones, level = row[:4]
        part = row[4] if len(row) > 4 else None
        g = next(x for x in mp.GUNS if x['mesh'] == mesh)
        gun = mp.Gun(g)
        tube = TUBE
        if part:
            select_part(gun, bones[0], part)
            TUBE = part.get('ring', TUBE)
        eye, obj, r, bi = analyse(gun, bones, quiet)
        TUBE = tube
        if part and part.get('glass'):
            rg = glass_radius(gun, eye, gun.tris)
            if not quiet:
                print('   the eyepiece glass r %.2f (the eyecup %.2f): published %.2f' % (rg, r, rg / 0.9 if rg > 0 else r))
            if rg > 0:
                r = rg / 0.9
        rows.append((att, level, bones[0], eye, obj, r, gun.local(bi, eye), gun.local(bi, obj)))
    if quiet:
        out = ['// Generated by tools/scope_points.py from the game\'s gun meshes (umodel psk exports) -- do not edit.',
               '// Per scoped gun: the scope tube in the gun mesh frame at the idle (+X left, +Y down, +Z muzzle; units).',
               'struct ScopeTube {', '    const char* gun;          // attachment class',
               '    int         level;        // the upgrade level from which the scope is on (-1: always)',
               '    const char* bone;         // the scope part', '    float       eyepiece[3];  // the eyepiece\'s centre (the rear end)',
               '    float       objective[3]; // the objective\'s centre (the front end)', '    float       radius;       // the eyepiece\'s radius',
               '    float       eyeLocal[3];  // the eyepiece in the bone\'s frame (carried by its live pose: the StG44\'s scope mounts)',
               '    float       objLocal[3];  // the objective likewise',
               '};', 'static const ScopeTube kScopeTubes[] = {']
        for att, level, bone, eye, obj, r, el, ol in rows:
            out.append('    {"%s", %d, "%s", {%.2ff, %.2ff, %.2ff}, {%.2ff, %.2ff, %.2ff}, %.2ff, {%.3ff, %.3ff, %.3ff}, {%.3ff, %.3ff, %.3ff}},' % (
                (att, level, bone) + tuple(eye) + tuple(obj) + (r,) + tuple(el) + tuple(ol)))
        out.append('};')
        print('\n'.join(out))


if __name__ == '__main__':
    main(sys.argv[1:])
