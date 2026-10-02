"""Physical melee (MELEE-DESIGN 2.2): the strike points on every first-person gun mesh, in gun mesh space.

For each gun mesh the umodel exports hold (work/research/reload/psk/Var_Flk_P/SkeletalMesh3/*.psk), at the gun's own
idle pose (frame 0 of its WeaponIdleAnim: the drawn pose, and the gun's pose through the game's melee, whose
WeaponFireAnim(2) is that idle, DefaultWeapon.ini) and per upgrade level (CurrentUpgradeLevel -1..2):
  * the barrel axis: the muzzle bone's Z row (tag_barrell; the Thompson's tag_barrel) and its Barrel_Player socket;
  * BUTT: the rear-most face of the visible geometry (the butt plate): the faces facing back (outward normal within
    45 deg of -axis) within BUTT_DEPTH of the rear-most vertex, the connected patch holding the rear-most vertex --
    its area-weighted centre, its extent across, its normal, the bone it rides on and the point in that bone's frame;
  * FRONT: the same at the front (muzzle face, compensator, launcher);
  * BAYONET (the M12's altFire_bayonet, level 2): the tip, and the blade as a segment (base at the gun's front face);
  * GRIP (pistols): the bottom of the grip along the grip axis (the Colt: the magazine bone's +Y; the C96: the grip's
    principal axis), the magazine base;
  * the MP40's level-2 "Dagger" (a separate mesh, DE_MP40_altFire_Knife, on the arms' LeftHand KnifeSocket in melee).
Optional: --swing replays the arms' own melee animations (VM_AnimSet_*) and reports which strike point leads.

Conventions (checked by this script, section "conventions"):
  * engine mesh space: +X = the gun's left, +Y = down, +Z = towards the muzzle (RELOAD-DESIGN 0.2, skeletons.md);
  * psk bone translation t -> engine (x, -y, z); bind rotation q -> engine (x, -y, z, w), the root's w negated (as the
    cooked RefSkeleton holds them: work/research/reload/engskel_all.txt); psa keys likewise (reload_grips.key_local);
  * psk PNTS0000 point p -> engine (x, -y, z) (each bone's geometry sits on the bone in the same convention);
  * VTXW0000 wedge -> point index (u16 here: fewer than 65536 points); FACE0000 = 3 wedge indices (u16), material;
    with the points in engine coordinates every mesh's signed volume is positive: outward normal = (b-a)x(c-a);
  * RAWWEIGHTS (weight, point, bone): every point has exactly one influence at 1.0 (rigid parts).

    python tools/melee_points.py [<export root>] [--swing] [--inc]
    python tools/melee_points.py --inc > src/mohavr/melee_points.inc

The default export root is work/research/reload (the umodel exports reload_grips.py reads). --swing replays the arms'
own melee animations with every bone parented to the root (the psa carries no hierarchy): a rough check only -- the
cooked chain Root -> HipsOffset -> RightProp turns in the melees (MELEE-DESIGN, the geometry check).
"""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import reload_grips as rg  # noqa: E402  (read_psk, read_psa, key_local, qmat, mmul, inv, xform)

BUTT_DEPTH = 6.0     # units: a face counts for the butt plate if its centre is this close to the rear-most vertex
FRONT_DEPTH = 3.0    # units: the same at the front (muzzle faces are square to the bore)
FACING = math.cos(math.radians(45.0))

# key: mesh -> dict. levels: {level: (shown by the upgrade, hidden by it (replaced))}; hide_only: shown only in the
# alt-fire mode with a rifle grenade loaded (MOHAG43/K98/M1Garand/Springfield.uc UpdateMeshVisibility); tree_hidden:
# AnimTree BoneScale 0 and no upgrade shows it; internal: rounds/clips/rockets parked inside the gun (not a surface);
# the AnimTree controls -> bones: work/research/reload/upgmesh.txt; upgrade classes: upgrades.txt (DefaultWeapon.ini).
GUNS = [
    dict(mesh='US_Thompson_Rigged', att='Attachment_Thompson', aset='Thompson_Rigged_Anims', idle='thompson_gun_idle_1',
         muzzle='tag_barrel', kind='smg', impact=0.30,
         levels={0: (['upgrade_01_PistolGrip'], ['upgrade_01_hide_frontGrip']), 1: (['upgrade_02_compensator'], []),
                 2: (['upgrade_03_drum'], ['upgrade_03_hide_magazine'])},
         hide_only=[], tree_hidden=[], internal=[],
         swing=['thompson_melee_1', 'thompson_melee_23', 'thompson_melee_234']),
    dict(mesh='MP40_rigged', att='Attachment_MP40', aset='MP40_AnimSet', idle='mp40_gun_idle_1', muzzle='tag_barrell',
         kind='smg', impact=0.40,
         levels={0: (['upgrade_01_tapedMagazine'], []),
                 1: (['upgrade_02_64rdMagazine', 'upgrade_02_64rdWell'],
                     ['upgrade_02_hide_stockWell', 'upgrade_01_tapedMagazine', 'magazine']),
                 2: ([], [])},  # "Dagger": the separate KnifeMesh on the arms (MOHA_MP40.uc:139-158)
         hide_only=[], tree_hidden=['dagger'], internal=['bullet'], swing=['mp40_melee_1', 'mp40_melee_3']),
    dict(mesh='DE_STG44_Rigged', att='Attachment_Stg44', aset='DE_STG44_AnimSet', idle='stg_gun_idle_1',
         muzzle='tag_barrell', kind='rifle', impact=0.32,
         levels={0: ([], []), 1: (['upgrade_02_tapedMagazines'], []), 2: ([], [])},
         hide_only=[], tree_hidden=['altFire_scope', 'scope_clip'],  # STG44Scope: the scope mode's (MOHAStg44.uc:183)
         internal=[], swing=['stg_melee_1', 'stg_melee_3a', 'stg_melee_3b']),
    dict(mesh='US_BAR1918_Rigged', att='Attachment_Bar', aset='US_BAR_AnimSet', idle='bar_gun_idle_1',
         muzzle='tag_barrell', kind='rifle', impact=0.30,
         levels={0: (['upgrade_01_compensator'], []), 1: (['upgrade_02_flipSight'], []), 2: (['upgrade_03_20rdMag'], [])},
         hide_only=[], tree_hidden=[], internal=[], swing=['bar_melee']),
    dict(mesh='DE_G43_Rigged', att='Attachment_G43', aset='DE_G43_AnimSet', idle='g43_gun_idle_1', muzzle='tag_barrell',
         kind='rifle', impact=0.31,
         levels={0: (['upgrade_01_20rdMag'], []), 1: (['upgrade_02_scope'], []), 2: (['altFire_grenLauncher'], [])},
         hide_only=['altFire_grenade'], tree_hidden=[], internal=['bullet'], swing=['g43_melee']),
    dict(mesh='US_M1911A1_Pistol_Rigged', att='Attachment_Colt45', aset='US_M1911A1_AnimSet', idle='colt45_gun_idle',
         muzzle='tag_barrell', kind='pistol', impact=0.30, levels={}, hide_only=[], tree_hidden=[], internal=['bullet'],
         grip=('bone', 'magazine'), swing=['colt45_melee']),
    dict(mesh='DE_Mauser_Rigged', att='Attachment_Mauser', aset='Mauser_AnimSet', idle='mauser_gun_idle',
         muzzle='tag_barrell', kind='pistol', impact=0.30,
         levels={0: (['upgrade_01_buttstock'], []), 1: (['upgrade_02_magazine'], []), 2: ([], [])},
         hide_only=[], tree_hidden=[], internal=['clip'], grip=('slices', 'RootOffset'), swing=['mauser_melee_1', 'mauser_melee_2'], swing_level={'mauser_melee_1': -1}),
    dict(mesh='US_Garand_Rigged', att='Attachment_M1Garand', aset='Garand_AnimSet', idle='m1garand_gun_idle_1',
         muzzle='tag_barrell', kind='rifle', impact=0.30,
         levels={0: ([], []), 1: ([], []), 2: (['upgrade_03_gren_launcher'], [])},
         hide_only=['upgrade_03_grenade_hide'], tree_hidden=[], internal=['clip'], swing=['m1garand_melee']),
    dict(mesh='DE_K98_Rigged', att='Attachment_K98', aset='K98_AnimSet', idle='k98_gun_idle_1', muzzle='tag_barrell',
         kind='rifle', impact=0.30,
         levels={0: (['upgrade_01_polished_bolt'], ['upgrade_01_hide_nasty_bolt']), 1: (['upgrade_02_stripperClip'], []),
                 2: (['altFire_grenAttachment'], [])},
         hide_only=['altFire_grenade_hide'], tree_hidden=[], internal=['bullet1', 'bullet2', 'upgrade_02_stripperClip'],
         swing=['k98_melee']),
    dict(mesh='US_1903sniper_Rigged', att='Attachment_Springfield', aset='Springfield_AnimSet',
         idle='springfield_gun_idle_1', muzzle='tag_barrell', kind='rifle', impact=0.30,
         levels={0: (['upgrade_01_polished_bolt'], ['upgrade_01_hide_nasty_bolt']), 1: (['upgrade_03_stripper_clip'], []),
                 2: (['tag_barrell'], [])},  # SpringfieldSniperGrenadeLauncherBarrel scales tag_barrell (launcher tube)
         hide_only=['altFire_gren_launch'], tree_hidden=['upgrade_02_8xscope'],
         internal=['bullet01', 'bullet02', 'upgrade_03_stripper_clip'], swing=['springfield_melee', 'springfield_melee_1']),
    dict(mesh='US_M12shotgun_Rigged', att='Attachment_M12CombatShotgun', aset='US_M12shotgun_AnimSet',
         idle='shotgun_gun_idle', muzzle='tag_barrell', kind='shotgun', impact=0.33,
         levels={0: (['upgrade_01_recoil_pad'], []), 1: ([], []), 2: (['altFire_bayonet'], [])},
         hide_only=[], tree_hidden=[], internal=['shell'], bayonet='altFire_bayonet',
         swing=['shotgun_melee_1', 'shotgun_melee_2']),
    dict(mesh='US_M18recoilless_Rigged', att='Attachment_M18RecoillessRifle', aset='US_M18_AnimSet', idle='m18_gun_idle',
         muzzle='tag_barrell', kind='launcher', impact=0.25, levels={}, hide_only=[], tree_hidden=[], internal=['shell'],
         swing=['m18_melee'], arms='VM_AnimSet_Bazooka'),
    dict(mesh='DE_Panzerschreck_Rigged', att='Attachment_Panzerschreck', aset='DE_Panzerschreck_Anim_Set',
         idle='panzerschreck_gun_idle', muzzle='tag_barrell', kind='launcher', impact=0.50, levels={}, hide_only=[],
         tree_hidden=[], internal=['Projectile'], swing=['panzerschreck_melee'], arms='VM_AnimSet_Bazooka'),
]
KNIFE = dict(mesh='DE_MP40_altFire_Knife', socket=('KnifeSocket', 'LeftHand', (8.0, 2.5, 0.0), (-20024, 0, 3640)))
# known engine values to cross-check (ENGINE-NOTES 5am, 5bk, 5bl; skeletons.md)
KNOWN = [('DE_Mauser_Rigged', 'tag_barrell', (0.0, -7.70, 27.26)), ('US_M1911A1_Pistol_Rigged', 'tag_barrell', (0.0, -7.40, 19.60)),
         ('US_M1911A1_Pistol_Rigged', 'gunSlide', (0.0, -7.00, 2.50)), ('DE_Mauser_Rigged', 'Bolt', (0.0, -8.42, 0.65)),
         ('DE_G43_Rigged', 'Bolt', (0.0, -9.16, 19.70)), ('US_BAR1918_Rigged', 'Bolt', (2.00, -6.60, 12.30)),
         ('US_Thompson_Rigged', 'Bolt', (0.0, -11.0, -3.5))]


# ---------------------------------------------------------------------------------------------------------- reading
def _chunks(d):
    p = 0
    while p + 32 <= len(d):
        cid = d[p:p + 20].split(b'\0')[0].decode('latin1')
        _, ds, dc = struct.unpack_from('<iii', d, p + 20)
        yield cid, ds, dc, p + 32
        p += 32 + ds * dc


def read_mesh(path):
    """Raw psk: points (file values), wedges -> point, faces -> 3 wedge, bones (reload_grips form), weights."""
    d = open(path, 'rb').read()
    m = dict(bones=rg.read_psk(path))
    for cid, ds, dc, p in _chunks(d):
        if cid.startswith('PNTS'):
            m['pts'] = [struct.unpack_from('<3f', d, p + ds * i) for i in range(dc)]
        elif cid.startswith('VTXW'):
            big = len(m['pts']) > 65536
            m['wedge'] = [struct.unpack_from('<I' if big else '<H', d, p + ds * i)[0] for i in range(dc)]
        elif cid.startswith('FACE'):
            fmt = '<HHH' if cid.startswith('FACE0000') else '<III'
            m['faces'] = [struct.unpack_from(fmt, d, p + ds * i) for i in range(dc)]
        elif cid.startswith('RAWWEIGHTS'):
            m['w'] = [struct.unpack_from('<fii', d, p + ds * i) for i in range(dc)]
    return m


def props_sockets(mesh):
    """Sockets from the umodel props dump (raw engine values: not mirrored)."""
    path = os.path.join(PSKDIR, mesh + '.props.txt')
    out, cur = {}, None
    for line in open(path, encoding='latin1'):
        s = line.strip()
        if s.startswith('SocketName ='):
            cur = {'name': s.split('=', 1)[1].strip()}
        elif cur is not None and s.startswith('BoneName ='):
            cur['bone'] = s.split('=', 1)[1].strip()
        elif cur is not None and s.startswith('RelativeLocation ='):
            v = s.split('{', 1)[1].split('}')[0]
            cur['loc'] = tuple(float(kv.split('=')[1]) for kv in v.split(','))
        elif cur is not None and s.startswith('RelativeRotation ='):
            v = s.split('{', 1)[1].split('}')[0]
            kv = dict((a.split('=')[0].strip(), int(float(a.split('=')[1]))) for a in v.split(','))
            cur['rot'] = (kv['Pitch'], kv['Yaw'], kv['Roll'])
            out[cur['name']] = cur
            cur = None
    return out


# ------------------------------------------------------------------------------------------------------------- math
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a): return math.sqrt(dot(a, a))
def unit(a):
    n = norm(a)
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-12 else (0.0, 0.0, 0.0)
def v3(s, v): return '(%s)' % ', '.join(('%' + s) % c for c in v)


def engine_q(b, i):
    x, y, z, w = b['q']
    return (x, -y, z, -w) if i == 0 else (x, -y, z, w)


def bind_cs(bones, i):
    """The bone's bind (RefSkeleton) frame in engine mesh space (row vectors, reload_grips convention)."""
    m = None
    while True:
        b = bones[i]
        l = rg.qmat(engine_q(b, i), (b['t'][0], -b['t'][1], b['t'][2]))
        m = l if m is None else rg.mmul(m, l)
        if i == 0 or b['parent'] == i:
            return m
        i = b['parent']


def pose_cs(psa, seq, bones, i, f):
    """The bone's frame at frame f of seq: psa keys where the AnimSet has the bone, the bind local otherwise."""
    m = None
    while True:
        b = bones[i]
        if b['name'] in psa['bones']:
            l = rg.key_local(psa, seq, b['name'], f)
        else:
            l = rg.qmat(engine_q(b, i), (b['t'][0], -b['t'][1], b['t'][2]))
        m = l if m is None else rg.mmul(m, l)
        if i == 0 or b['parent'] == i:
            return m
        i = b['parent']


def rot_angle(a, b):
    r = [[sum(a[i][k] * b[j][k] for k in range(3)) for j in range(3)] for i in range(3)]
    return math.degrees(math.acos(max(-1.0, min(1.0, (r[0][0] + r[1][1] + r[2][2] - 1) / 2))))


def rotator_x(pitch, yaw, roll):
    """UE3 FRotationMatrix rows (X, Y, Z axes) for a Rotator in 65536ths."""
    p, y, r = (v * math.pi / 32768.0 for v in (pitch, yaw, roll))
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    return [(cp * cy, cp * sy, sp), (sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp),
            (-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp)]


# --------------------------------------------------------------------------------------------------------- the mesh
class Gun:
    def __init__(self, g):
        self.g = g
        self.m = read_mesh(os.path.join(PSKDIR, g['mesh'] + '.psk'))
        self.bones = self.m['bones']
        self.names = [b['name'] for b in self.bones]
        self.psa = rg.read_psa(os.path.join(PSADIR, g['aset'] + '.psa'))
        self.seq = self.psa['seqs'][g['idle']]
        self.B = [bind_cs(self.bones, i) for i in range(len(self.bones))]
        self.P = [pose_cs(self.psa, self.seq, self.bones, i, 0) for i in range(len(self.bones))]
        self.skin = [rg.mmul(rg.inv(self.B[i]), self.P[i]) for i in range(len(self.bones))]
        inf = {}
        for w, pi, bi in self.m['w']:
            inf.setdefault(pi, []).append((w, bi))
        self.inf = inf
        self.owner = [max(inf[i])[1] for i in range(len(self.m['pts']))]
        self.bind_pts = [(p[0], -p[1], p[2]) for p in self.m['pts']]
        self.pts = [rg.xform(self.bind_pts[i], self.skin[self.owner[i]]) for i in range(len(self.bind_pts))]
        wd = self.m['wedge']
        self.tris = [(wd[a], wd[b], wd[c]) for a, b, c in self.m['faces']]
        # the winding's orientation in engine coordinates: the closed mesh's signed volume (> 0: (b-a)x(c-a) is outward)
        self.volume = sum(dot(self.bind_pts[a], cross(self.bind_pts[b], self.bind_pts[c])) for a, b, c in self.tris) / 6.0
        self.outward = 1.0 if self.volume > 0 else -1.0
        mi = self.names.index(g['muzzle'])
        self.muzzle = self.P[mi]
        self.axis = unit(tuple(self.muzzle[2][:3]))  # the muzzle bone's Z row

    def hidden(self, level, alt=False):
        g = self.g
        hide = set(g['tree_hidden']) | set(g['internal'])
        if not alt:
            hide |= set(g['hide_only'])
        for L, (shown, replaced) in g['levels'].items():
            if L > level:
                hide |= set(shown)
            else:
                hide |= set(replaced)
        # a scale-0 bone hides its children too (component-space composition)
        idx = {self.names.index(n) for n in hide if n in self.names}
        changed = True
        while changed:
            changed = False
            for i, b in enumerate(self.bones):
                if i not in idx and i != 0 and b['parent'] in idx:
                    idx.add(i)
                    changed = True
        return idx

    def visible_tris(self, hidden):
        out = []
        for t in self.tris:
            if any(self.owner[v] in hidden for v in t):
                continue
            a, b, c = (self.pts[v] for v in t)
            n = cross(sub(b, a), sub(c, a))  # engine coords, psk winding; outward per the signed volume
            ar = 0.5 * norm(n)
            if ar < 1e-9:
                continue
            out.append((t, unit(mul(n, self.outward)), ar, mul(add(add(a, b), c), 1.0 / 3.0)))
        return out

    def patch(self, hidden, direction, depth, only_bones=None, mode='cap'):
        """The face patch at the extreme along direction: the faces facing that way (outward normal within 45 deg)
        whose centre is within depth of the extreme vertex -- all of them (mode 'cap'), or only the connected piece
        holding the extreme-most face (mode 'piece': a butt plate, not the stock face a recoil pad covers)."""
        vis = [i for i in range(len(self.pts)) if self.owner[i] not in hidden and
               (only_bones is None or self.owner[i] in only_bones)]
        smax = max(dot(self.pts[i], direction) for i in vis)
        ext = max(vis, key=lambda i: dot(self.pts[i], direction))
        cand = [f for f in self.visible_tris(hidden) if dot(f[1], direction) >= FACING and
                dot(f[3], direction) >= smax - depth and (only_bones is None or self.owner[f[0][0]] in only_bones)]
        if not cand:
            # a point (a blade's tip): no face faces that way -- the points within depth of the extreme instead
            vs = [i for i in vis if dot(self.pts[i], direction) >= smax - depth]
            cen = mul(tuple(sum(self.pts[i][k] for i in vs) for k in range(3)), 1.0 / len(vs))
            lo = tuple(min(self.pts[i][k] for i in vs) for k in range(3))
            hi = tuple(max(self.pts[i][k] for i in vs) for k in range(3))
            bone = max(set(self.owner[i] for i in vs), key=lambda b: sum(1 for i in vs if self.owner[i] == b))
            return dict(c=cen, n=direction, area=0.0, lo=lo, hi=hi, bone=bone, bones={bone: 1.0}, ext=self.pts[ext],
                        ext_bone=self.owner[ext], smax=smax, faces=0, r=max(norm(sub(self.pts[i], cen)) for i in vs),
                        pointed=True)
        # connectivity through shared points (and coincident positions: the exports split vertices at UV seams)
        key = lambda v: tuple(round(c, 3) for c in self.pts[v])
        parent = list(range(len(cand)))

        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a
        seen = {}
        for fi, f in enumerate(cand):
            for v in f[0]:
                k = key(v)
                if k in seen:
                    parent[find(fi)] = find(seen[k])
                else:
                    seen[k] = fi
        # the whole cap (every face facing that way within depth), and its connected pieces for the record
        pieces = {}
        for fi in range(len(cand)):
            pieces.setdefault(find(fi), []).append(cand[fi])
        plist = []
        for pc in pieces.values():
            pa = sum(f[2] for f in pc)
            plist.append((pa, mul(tuple(sum(f[3][k] * f[2] for f in pc) for k in range(3)), 1.0 / pa),
                          self.owner[pc[0][0][0]]))
        plist.sort(key=lambda x: -x[0])
        if mode == 'piece':
            best = max(range(len(cand)), key=lambda fi: dot(cand[fi][3], direction))
            comp = [cand[fi] for fi in range(len(cand)) if find(fi) == find(best)]
        else:
            comp = cand
        area = sum(f[2] for f in comp)
        cen = mul(tuple(sum(f[3][k] * f[2] for f in comp) for k in range(3)), 1.0 / area)
        nrm = unit(tuple(sum(f[1][k] * f[2] for f in comp) for k in range(3)))
        vs = {v for f in comp for v in f[0]}
        lo = tuple(min(self.pts[v][k] for v in vs) for k in range(3))
        hi = tuple(max(self.pts[v][k] for v in vs) for k in range(3))
        bones = {}
        for f in comp:
            bones[self.owner[f[0][0]]] = bones.get(self.owner[f[0][0]], 0.0) + f[2]
        bone = max(bones, key=bones.get)
        rad = max(norm(sub(self.pts[v], cen)) for v in vs)
        return dict(c=cen, n=nrm, area=area, lo=lo, hi=hi, bone=bone, bones=bones, ext=self.pts[ext],
                    ext_bone=self.owner[ext], smax=smax, faces=len(comp), r=rad, pieces=plist,
                    vpos=[self.pts[v] for v in vs])

    def bore_point(self, s):
        """The bore line (through the muzzle bone along the axis) at s along the axis."""
        m = tuple(self.muzzle[3][:3])
        return add(m, mul(self.axis, s - dot(m, self.axis)))

    def section(self, bone, s0):
        """Cross-section of one bone's triangles with the plane dot(p, axis) = s0: centre, height (Y), thickness (X)."""
        hits = []
        for t in self.tris:
            if self.owner[t[0]] != bone:
                continue
            for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
                pa, pb = self.pts[a], self.pts[b]
                da, db = dot(pa, self.axis) - s0, dot(pb, self.axis) - s0
                if (da <= 0 < db) or (db <= 0 < da):
                    u = da / (da - db)
                    hits.append(add(pa, mul(sub(pb, pa), u)))
        if not hits:
            return None
        c = tuple((min(p[k] for p in hits) + max(p[k] for p in hits)) / 2 for k in range(3))
        return c, max(p[1] for p in hits) - min(p[1] for p in hits), max(p[0] for p in hits) - min(p[0] for p in hits)

    def motion(self, bone, p):
        """How far the idle point p, carried by its bone, strays over every frame of every sequence of the gun's
        AnimSet (the gun's own fire / reload / rechamber anims): (max distance, seq, frame)."""
        loc = self.local(bone, p)
        best = (0.0, '-', 0)
        for name, seq in self.psa['seqs'].items():
            for f in range(seq['frames']):
                q = rg.xform(loc, pose_cs(self.psa, seq, self.bones, bone, f))
                d = math.dist(q, p)
                if d > best[0]:
                    best = (d, name, f)
        return best

    def local(self, bone, p):
        """A mesh-space (idle) point in the bone's own frame (so the mod can carry it with the bone's space base)."""
        return rg.xform(p, rg.inv(self.P[bone]))


def fmt_patch(gun, pt, label):
    if pt is None:
        return '    %-6s none' % label
    bn = gun.names[pt['bone']]
    loc = gun.local(pt['bone'], pt['c'])
    tot = sum(pt['bones'].values()) or 1.0
    others = ', '.join('%s %.0f%%' % (gun.names[b], 100 * a / tot) for b, a in sorted(pt['bones'].items(),
                                                                                      key=lambda x: -x[1]) if b != pt['bone'])
    mv = gun.motion(pt['bone'], pt['c'])
    pieces = '\n           rides on %s: over the gun\'s %d sequences it strays up to %.2f u from the idle (%s f%d)' % (
        bn, len(gun.psa['seqs']), mv[0], mv[1], mv[2])
    if len(pt.get('pieces', [])) > 1:
        pieces += '\n           %d pieces: %s' % (len(pt['pieces']), '; '.join(
            '%.1f u^2 at %s on %s' % (a, v3('.1f', c), gun.names[b]) for a, c, b in pt['pieces'][:4]))
    return ('    %-6s centre %s on %s (in its frame %s)%s%s\n'
            '           normal %s, area %.1f u^2 (%d faces), X %.1f..%.1f  Y %.1f..%.1f  Z %.1f..%.1f, r %.1f;'
            ' extreme vertex %s on %s%s' % (
                label, v3('6.2f', pt['c']), bn, v3('.2f', loc), (' [+ ' + others + ']') if others else '',
                ' [pointed: the points within the depth]' if pt.get('pointed') else '',
                v3('5.2f', pt['n']), pt['area'], pt['faces'], pt['lo'][0], pt['hi'][0], pt['lo'][1], pt['hi'][1],
                pt['lo'][2], pt['hi'][2], pt['r'], v3('.2f', pt['ext']), gun.names[pt['ext_bone']], pieces))


# ------------------------------------------------------------------------------------------------------- the checks
def conventions(guns):
    print('== conventions')
    by = {gun.g['mesh']: gun for gun in guns}
    for mesh, bone, want in KNOWN:
        gun = by[mesh]
        i = gun.names.index(bone)
        got = tuple(gun.P[i][3][:3])
        print('  %-26s %-12s idle %s  known %s  diff %.3f' % (mesh, bone, v3('7.2f', got), v3('6.2f', want),
                                                                math.dist(got, want)))
    # the C96's ShellEject_Player socket: tag_eject + (0, 0, -10) in its frame (ENGINE-NOTES 5bl)
    gun = by['DE_Mauser_Rigged']
    s = props_sockets('DE_Mauser_Rigged')['ShellEject_Player']
    p = rg.xform(s['loc'], gun.P[gun.names.index(s['bone'])])
    near = min(math.dist(p, q) for q in gun.pts)
    print('  C96 ShellEject_Player on %s %s -> mesh %s (nearest vertex %.2f u; the [S] kick1 brass "25 units behind'
          ' the drawn muzzle": %.1f)' % (s['bone'], v3('.2f', s['loc']), v3('.2f', p), near, gun.P[gun.names.index('tag_barrell')][3][2] - p[2]))
    # rotations: idle vs bind for the bones with a non-identity bind rotation
    for gun in guns:
        for i, b in enumerate(gun.bones):
            if abs(b['q'][0]) + abs(b['q'][1]) + abs(b['q'][2]) > 1e-3 and b['name'] in gun.psa['bones']:
                print('  %-26s %-12s bind q %s: idle vs bind %.1f deg (engine q = (x,-y,z,w))' % (
                    gun.g['mesh'], b['name'], v3('.3f', b['q']), rot_angle(gun.B[i], gun.P[i])))
    # vertices: same Y convention as the bones? For every bone with geometry that sits 4+ units off the bore plane
    # (|engine y| >= 4, where the sign decides), the distance from the bone to its own geometry's bbox centre with the
    # points' Y negated (as the bones') and with the file's Y kept.
    rows = []
    for gun in guns:
        for i, b in enumerate(gun.bones):
            vs = [gun.bind_pts[j] for j in range(len(gun.bind_pts)) if gun.owner[j] == i]
            bp = tuple(gun.B[i][3][:3])
            if not vs or abs(bp[1]) < 4.0:
                continue
            c = tuple((min(p[k] for p in vs) + max(p[k] for p in vs)) / 2 for k in range(3))
            cf = (c[0], -c[1], c[2])
            rows.append((gun.g['mesh'], b['name'], bp[1], math.dist(bp, c), math.dist(bp, cf)))
    # and X: bones 1.5+ units off the centre plane, the points' X kept vs negated (umodel mirrors Y only)
    xr = []
    for gun in guns:
        for i, b in enumerate(gun.bones):
            vs = [gun.bind_pts[j] for j in range(len(gun.bind_pts)) if gun.owner[j] == i]
            bp = tuple(gun.B[i][3][:3])
            if not vs or abs(bp[0]) < 1.5:
                continue
            c = tuple((min(p[k] for p in vs) + max(p[k] for p in vs)) / 2 for k in range(3))
            xr.append((gun.g['mesh'], b['name'], bp[0], math.dist(bp, c), math.dist(bp, (-c[0], c[1], c[2]))))
    print('  vertices, X: %d of %d bones 1.5+ units off the centre plane sit on their geometry with X kept: %s' % (
        sum(1 for r in xr if r[3] < r[4]), len(xr), ', '.join('%s %s x %.1f: %.1f vs %.1f' % r for r in xr)))
    good = [r for r in rows if r[3] < r[4]]
    print('  vertices: %d of %d bones 4+ units off the bore plane sit on their own geometry with the points\' Y negated'
          ' as the bones\' (median bone->bbox centre %.1f u negated vs %.1f u kept); exceptions: %s' % (
              len(good), len(rows), sorted(r[3] for r in rows)[len(rows) // 2], sorted(r[4] for r in rows)[len(rows) // 2],
              ['%s %s y %.1f: %.1f vs %.1f' % r for r in rows if r[3] >= r[4]]))
    # weights and winding
    multi = sum(1 for gun in guns for v in gun.inf.values() if len(v) != 1 or abs(v[0][0] - 1.0) > 1e-6)
    print('  weights: points with other than one influence at 1.0: %d (of %d)' % (
        multi, sum(len(gun.inf) for gun in guns)))
    for gun in guns:
        vol = gun.volume
        sock = props_sockets(gun.g['mesh']).get('Barrel_Player')
        sx = None
        if sock:
            bi = gun.names.index(sock['bone'])
            rx = rotator_x(*sock['rot'])[0]
            sx = unit(tuple(sum(rx[k] * gun.P[bi][k][j] for k in range(3)) for j in range(3)))
        print('  %-26s signed volume (psk winding, engine coords) %9.0f -> outward = %s; muzzle %s Z row %s;'
              ' Barrel_Player X %s' % (gun.g['mesh'], vol, '-cross' if vol < 0 else '+cross', gun.g['muzzle'],
                                       v3('.3f', gun.axis), v3('.3f', sx) if sx else '-'))


# --------------------------------------------------------------------------------------------------------- analysis
def grip_axis(gun, hidden):
    kind, arg = gun.g['grip']
    if kind == 'bone':
        i = gun.names.index(arg)
        return unit(tuple(gun.P[i][1][:3])), 'the %s bone\'s +Y' % arg
    # the C96 (broomhandle): the grip is the frame below the bore behind the trigger guard (Z < 1: the side profile has
    # nothing below Y -1.2 between Z 0.9 and 3.9; the grip box has points only at Y -2..-1 and 4..7.1); its axis runs
    # from the centroid of its top slice (Y -2.5..-0.5) to that of its bottom slice (the lowest 1.5 units)
    root = gun.names.index(arg)
    pts = [gun.pts[i] for i in range(len(gun.pts)) if gun.owner[i] == root and gun.pts[i][1] > -2.5 and gun.pts[i][2] < 1.0]
    ybot = max(p[1] for p in pts)
    top = [p for p in pts if p[1] <= -0.5]
    bot = [p for p in pts if p[1] >= ybot - 1.5]
    ct = mul(tuple(sum(p[k] for p in top) for k in range(3)), 1.0 / len(top))
    cb = mul(tuple(sum(p[k] for p in bot) for k in range(3)), 1.0 / len(bot))
    return unit(sub(cb, ct)), 'the grip from its top slice %s to its bottom slice %s (%d/%d points)' % (
        v3('.2f', ct), v3('.2f', cb), len(top), len(bot))


def analyse(gun, out_rows):
    g = gun.g
    print('\n== %s  (%s; idle %s)' % (g['mesh'], g['att'], g['idle']))
    socks = props_sockets(g['mesh'])
    mp = tuple(gun.muzzle[3][:3])
    bp = socks.get('Barrel_Player')
    bpp = rg.xform(bp['loc'], gun.P[gun.names.index(bp['bone'])]) if bp else None
    print('  barrel axis %s through %s at %s; Barrel_Player %s' % (
        v3('.4f', gun.axis), g['muzzle'], v3('.2f', mp), v3('.2f', bpp) if bpp else '-'))
    print('  parts at the idle (bone: points, X / Y / Z extent; [moved] = idle differs from bind; (internal) / (hidden)):')
    allhid = set(g['internal']) | set(g['tree_hidden'])
    for i, b in enumerate(gun.bones):
        vs = [gun.pts[j] for j in range(len(gun.pts)) if gun.owner[j] == i]
        if not vs:
            continue
        moved = math.dist(gun.B[i][3][:3], gun.P[i][3][:3]) > 0.05 or rot_angle(gun.B[i], gun.P[i]) > 0.5
        lv = [L for L, (sh, rp) in g['levels'].items() if b['name'] in sh]
        rl = [L for L, (sh, rp) in g['levels'].items() if b['name'] in rp]
        note = ('(internal)' if b['name'] in g['internal'] else '(hidden: AnimTree/scope mode)' if b['name'] in g['tree_hidden']
                else '(alt-fire grenade)' if b['name'] in g['hide_only'] else
                ('(level >= %d)' % lv[0]) if lv else ('(until level %d)' % (rl[0] - 1)) if rl else '')
        print('    %-28s %5d  X %6.1f..%6.1f  Y %6.1f..%6.1f  Z %6.1f..%6.1f %s %s' % (
            b['name'], len(vs), min(p[0] for p in vs), max(p[0] for p in vs), min(p[1] for p in vs), max(p[1] for p in vs),
            min(p[2] for p in vs), max(p[2] for p in vs), '[moved]' if moved else '', note))
    levels = sorted(set([-1] + list(g['levels'].keys())))
    prev = {}
    for L in levels:
        hid = gun.hidden(L)
        shown = sorted({gun.names[gun.owner[i]] for i in range(len(gun.pts)) if gun.owner[i] not in hid})
        butt = gun.patch(hid, mul(gun.axis, -1.0), BUTT_DEPTH, mode='piece')
        nobay = hid | ({gun.names.index(g['bayonet'])} if g.get('bayonet') else set())
        front = gun.patch(nobay, gun.axis, FRONT_DEPTH)  # the muzzle end; a bayonet is its own row
        front['bore'] = gun.bore_point(front['smax'])
        front['bore_r'] = max(norm(sub(q, front['bore'])) for q in front['vpos'])
        key = (round(butt['c'][2], 2), round(front['c'][2], 2), gun.names[butt['bone']], gun.names[front['bone']])
        tag = 'level %d' % L if L >= 0 else 'no upgrade (-1)'
        print('  -- %s: geometry on %s' % (tag, ', '.join(shown)))
        if prev.get('key') == key:
            print('     (butt and front as the level before)')
        else:
            print(fmt_patch(gun, butt, 'BUTT'))
            print(fmt_patch(gun, front, 'FRONT'))
            print('           bore point at the front plane %s (in %s\'s frame %s), the cap within %.1f of it;'
                  ' the muzzle bone %s is %+.2f along the bore from that plane' % (
                      v3('.2f', front['bore']), gun.names[front['bone']],
                      v3('.2f', gun.local(front['bone'], front['bore'])), front['bore_r'], g['muzzle'],
                      dot(tuple(gun.muzzle[3][:3]), gun.axis) - front['smax']))
        prev['key'] = key
        out_rows.append((g['att'], L, 'butt', gun, butt))
        out_rows.append((g['att'], L, 'front', gun, front))
        if g['hide_only'] and any(n in gun.names for n in g['hide_only']) and L == max(levels):
            hid2 = gun.hidden(L, alt=True)
            f2 = gun.patch(hid2, gun.axis, FRONT_DEPTH)
            f2['bore'] = gun.bore_point(f2['smax'])
            f2['bore_r'] = max(norm(sub(q, f2['bore'])) for q in f2['vpos'])
            print(fmt_patch(gun, f2, 'FRONT*') + '\n           (* alt-fire mode with a rifle grenade loaded: %s shown);'
                  ' bore point %s' % (', '.join(g['hide_only']), v3('.2f', f2['bore'])))
            out_rows.append((g['att'], L, 'front_alt', gun, f2))
        if g.get('bayonet') and gun.names.index(g['bayonet']) not in hid:
            bayonet(gun, hid, out_rows, L, front)
        if g['kind'] == 'pistol':
            grip(gun, hid, out_rows, L)


def bayonet(gun, hid, out_rows, L, front):
    bi = gun.names.index(gun.g['bayonet'])
    a = gun.axis
    sbody = front['smax']
    vs = [gun.pts[i] for i in range(len(gun.pts)) if gun.owner[i] == bi]
    tip = max(vs, key=lambda p: dot(p, a))
    stip = dot(tip, a)
    smin = min(dot(p, a) for p in vs)
    print('    BAYONET %s (%d points, s %.1f..%.1f): the gun\'s own front at s %.1f; tip %s, %.1f beyond the muzzle' % (
        gun.g['bayonet'], len(vs), smin, stip, sbody, v3('.2f', tip), stip - sbody))
    print('           sections across the blade (the bone\'s triangles cut by planes square to the bore):')
    secs = []
    for s0 in [smin + 0.5, (smin + sbody) / 2, sbody - 0.5, sbody + 0.5] + [sbody + (stip - sbody) * k / 8 for k in range(1, 8)] + [stip - 0.5]:
        r = gun.section(bi, s0)
        if r:
            secs.append((s0, r))
            print('           s %6.1f: centre %s  height (Y) %5.2f  thickness (X) %4.2f' % (s0, v3('6.2f', r[0]), r[1], r[2]))
    beyond = [x for x in secs if x[0] > sbody]
    base = beyond[0][1][0] if beyond else tip
    half = max(x[1][1] for x in beyond) / 2 if beyond else 0.0
    print('           blade segment: base %s -> tip %s (length %.1f, half-height up to %.2f); in %s\'s frame %s -> %s' % (
        v3('.2f', base), v3('.2f', tip), math.dist(base, tip), half, gun.g['bayonet'], v3('.2f', gun.local(bi, base)),
        v3('.2f', gun.local(bi, tip))))
    mv = gun.motion(bi, tip)
    print('           rides on %s: over the gun\'s sequences the tip strays up to %.2f u from the idle (%s f%d)' % (
        gun.g['bayonet'], mv[0], mv[1], mv[2]))
    out_rows.append((gun.g['att'], L, 'bayonet', gun, dict(c=tip, base=base, bone=bi, n=gun.axis, r=half,
                                                             lo=tip, hi=tip, area=0, ext=tip, bones={bi: 1.0})))


def grip(gun, hid, out_rows, L):
    ax, how = grip_axis(gun, hid)
    frame = {gun.names.index('RootOffset')}
    pt = gun.patch(hid, ax, 2.0, only_bones=frame, mode='piece')
    print('    grip axis %s (%s)' % (v3('.3f', ax), how))
    print(fmt_patch(gun, pt, 'GRIP'))
    if gun.g['grip'][0] == 'bone':
        mi = gun.names.index(gun.g['grip'][1])
        pm = gun.patch(hid, ax, 2.0, only_bones={mi}, mode='piece')
        print(fmt_patch(gun, pm, 'MAGBASE'))
        out_rows.append((gun.g['att'], L, 'magbase', gun, pm))
    out_rows.append((gun.g['att'], L, 'grip', gun, pt))


def knife():
    m = read_mesh(os.path.join(PSKDIR, KNIFE['mesh'] + '.psk'))
    pts = [(p[0], -p[1], p[2]) for p in m['pts']]
    tip = max(pts, key=lambda p: p[2])
    lo = tuple(min(p[k] for p in pts) for k in range(3))
    hi = tuple(max(p[k] for p in pts) for k in range(3))
    print('\n== %s (Attachment_MP40.KnifeMesh; MOHAUpgradeMP40_2 "Dagger", level 2)' % KNIFE['mesh'])
    print('  %d points on one bone (Root); X %.2f..%.2f Y %.2f..%.2f Z %.2f..%.2f; tip %s' % (
        len(pts), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], v3('.2f', tip)))
    for z0 in (0.0, 4.0, 8.0, 16.0, 24.0, 30.0):
        sl = [p for p in pts if abs(p[2] - z0) <= 1.0]
        if sl:
            print('    z %5.1f: Y %.2f..%.2f  X %.2f..%.2f  [%d pts]' % (z0, min(p[1] for p in sl), max(p[1] for p in sl),
                                                                     min(p[0] for p in sl), max(p[0] for p in sl), len(sl)))
    name, bone, loc, rot = KNIFE['socket']
    r = rotator_x(*rot)
    # the socket frame: rows = rotator rows, origin = loc (bone frame); a knife point p -> bone frame p*R + loc
    tipb = tuple(sum(tip[k] * r[k][j] for k in range(3)) + loc[j] for j in range(3))
    print('  attached to VM_Arms %s on %s at %s, RelativeRotation (P %d, Y %d, R %d)'
          ' (MOHAGame.xxx ViewModel_Mesh.SkeletalMesh.VM_Arms.SkeletalMeshSocket_14):' % (name, bone, v3('.1f', loc), *rot))
    print('  the knife\'s +Z in %s\'s frame %s; the tip in %s\'s frame %s (engine bone space, UE3 FRotationMatrix;'
          ' unverified in game)' % (bone, v3('.3f', r[2]), bone, v3('.2f', tipb)))


# ------------------------------------------------------------------------------------------------------------ swing
def swing(guns, out_rows):
    print('\n== the game\'s own melee swing (arms AnimSet, RightProp = the gun mesh frame; displacement in the gun\'s'
          ' frame-0 axes: +Z = the frame-0 barrel = forward)')
    arms = {}
    for gun in guns:
        aset = gun.g.get('arms', 'VM_AnimSet_NoBazooka')
        if aset not in arms:
            path = os.path.join(ARMSDIR, aset + '.psa')
            a = rg.read_psa(path)
            data = open(path, 'rb').read()
            for cid, ds, dc, p in _chunks(data):
                if cid.startswith('BONENAMES'):
                    a['parents'] = [struct.unpack_from('<Iii', data, p + i * ds + 64)[2] for i in range(dc)]
            arms[aset] = a
        a = arms[aset]

        def cs(seq, bone, f):
            i, m = a['bones'].index(bone), None
            while True:
                l = rg.key_local(a, seq, a['bones'][i], f)
                m = l if m is None else rg.mmul(m, l)
                if i == 0 or a['parents'][i] == i:
                    return m
                i = a['parents'][i]
        for sname in gun.g['swing']:
            L = gun.g.get('swing_level', {}).get(sname, max([-1] + list(gun.g['levels'].keys())))
            pts = {}
            for att, lv, kind, gg, pt in out_rows:
                if gg is gun and lv == L and pt is not None and kind in ('butt', 'front', 'bayonet', 'grip'):
                    pts[kind] = pt.get('bore', pt['c'])
            if sname not in a['seqs']:
                print('  %-24s missing' % sname)
                continue
            s = a['seqs'][sname]
            R0 = cs(s, 'RightProp', 0)
            R0i = rg.inv(R0)
            n = s['frames']
            traj = {k: [] for k in pts}
            for f in range(n):
                M = rg.mmul(cs(s, 'RightProp', f), R0i)  # frame f's gun in frame 0's gun frame
                for k, p in pts.items():
                    traj[k].append(rg.xform(p, M))
            fi = int(round(gun.g['impact'] * s['rate']))
            parts = []
            lead = max(traj, key=lambda k: traj[k][min(fi, n - 1)][2] - traj[k][0][2])
            for k, tr in traj.items():
                dz = [q[2] - tr[0][2] for q in tr]
                vz = [(tr[min(f + 1, n - 1)][2] - tr[max(f - 1, 0)][2]) * s['rate'] / (2 if 0 < f < n - 1 else 1) for f in range(n)]
                fpk = max(range(n), key=lambda f: dz[f])
                fv = max(range(n), key=lambda f: vz[f])
                tot = [math.dist(q, tr[0]) for q in tr]
                parts.append('%s: fwd max %+.1f @f%d, vfwd max %.0f u/s @f%d, at impact f%d fwd %+.1f moved %.1f' % (
                    k, dz[fpk], fpk, vz[fv], fv, fi, dz[min(fi, n - 1)], tot[min(fi, n - 1)]))
            lat = {k: max(abs(tr[f][0] - tr[f - 1][0]) * s['rate'] for f in range(1, n)) for k, tr in traj.items()}
            print('  %-26s %-20s (points of level %d) %d f @%.0f/s, impact %.2f s -> leading at impact: %s\n      %s\n'
                  '      peak sideways (X) speed: %s' % (gun.g['mesh'], sname, L, n, s['rate'], gun.g['impact'], lead,
                                                         '\n      '.join(parts),
                                                         ', '.join('%s %.0f u/s' % kv for kv in lat.items())))


# ------------------------------------------------------------------------------------------------------------ table
def table(rows):
    """One line per gun and strike part, levels with the same point merged (mesh space at the idle, units)."""
    print('\n== TABLE: strike points in gun mesh space at the idle (+X left, +Y down, +Z muzzle; units ~ 0.9 cm)')
    print('   %-28s %-26s %-9s %-7s %-26s %-22s %s' % ('attachment', 'mesh', 'kind', 'levels', 'point', 'bone', 'extent / note'))
    merged = []
    last = {}
    for att, L, kind, gun, pt in rows:
        if pt is None:
            continue
        c = pt.get('bore', pt['c'])
        key = (att, kind, tuple(round(v, 2) for v in c), pt['bone'])
        m = last.get((att, kind))
        if m is not None and m[0] == key and m[2] == L - 1:
            m[2] = L
        else:
            m = [key, L, L, gun, pt]
            merged.append(m)
            last[(att, kind)] = m
    order = {'butt': 0, 'front': 1, 'front_alt': 2, 'bayonet': 3, 'grip': 4, 'magbase': 5}
    merged.sort(key=lambda m: ([g['att'] for g in GUNS].index(m[0][0]), order[m[0][1]], m[1]))
    for key, l0, l1, gun, pt in merged:
        att, kind, c, bone = key
        lv = ('%d' % l0 if l0 == l1 else '%d..%d' % (l0, l1)).replace('-1', 'none')
        if kind == 'butt':
            note = 'plate X %.1f..%.1f Y %.1f..%.1f, normal %s, r %.1f' % (pt['lo'][0], pt['hi'][0], pt['lo'][1], pt['hi'][1],
                                                                        v3('.2f', pt['n']), pt['r'])
        elif kind in ('front', 'front_alt'):
            note = 'bore at the front plane; cap within %.1f%s' % (pt['bore_r'], ' (rifle grenade loaded)' if kind == 'front_alt' else '')
        elif kind == 'bayonet':
            note = 'tip; blade from %s, half-height <= %.2f' % (v3('.2f', pt['base']), pt['r'])
        else:
            note = 'normal %s, X %.1f..%.1f Z %.1f..%.1f' % (v3('.2f', pt['n']), pt['lo'][0], pt['hi'][0], pt['lo'][2], pt['hi'][2])
        print('   %-28s %-26s %-9s %-7s %-26s %-22s %s' % (att, gun.g['mesh'], kind, lv, v3('.2f', c), gun.names[bone], note))


# -------------------------------------------------------------------------------------------------------------- inc
def emit_inc(rows):
    out = ['// Generated by tools/melee_points.py from the game\'s gun meshes (umodel psk exports) -- do not edit.',
           '// Per gun and upgrade level (CurrentUpgradeLevel; -1 = none): strike parts in the gun mesh frame at the idle',
           '// (+X left, +Y down, +Z muzzle; units), the bone each rides on and the point in that bone\'s frame.',
           'struct MeleeStrike {', '    const char* gun;      // attachment class',
           '    int         level;    // upgrade level the row is for', '    const char* kind;     // "butt", "front", "front_alt", "bayonet", "grip"',
           '    const char* bone;', '    float       mesh[3];  // centre (front: the bore at the front plane; bayonet: the tip)', '    float       local[3]; // in the bone\'s frame',
           '    float       normal[3];', '    float       radius;   // patch radius about the centre', '    float       base[3];  // bayonet: the blade\'s base (else = mesh)',
           '};', 'static const MeleeStrike kMeleeStrikes[] = {']
    for att, L, kind, gun, pt in rows:
        if pt is None:
            continue
        c = pt.get('bore', pt['c'])
        r = pt.get('bore_r', pt['r'])
        loc = gun.local(pt['bone'], c)
        base = pt.get('base', c)
        out.append('    {"%s", %d, "%s", "%s", {%.2ff, %.2ff, %.2ff}, {%.2ff, %.2ff, %.2ff}, {%.3ff, %.3ff, %.3ff}, %.1ff,'
                   ' {%.2ff, %.2ff, %.2ff}},' % ((att, L, kind, gun.names[pt['bone']]) + tuple(c) + tuple(loc) +
                                                  tuple(pt['n']) + (r,) + tuple(base)))
    out.append('};')
    print('\n'.join(out))


def main(argv):
    global PSKDIR, PSADIR, ARMSDIR
    root = next((a for a in argv if not a.startswith('--')), os.path.join(REPO, 'work', 'research', 'reload'))
    PSKDIR = os.path.join(root, 'psk', 'Var_Flk_P', 'SkeletalMesh3')
    PSADIR = os.path.join(root, 'psa', 'Var_Flk_P', 'AnimSet')
    ARMSDIR = os.path.join(root, 'psa', 'MOHAGame', 'AnimSet')
    guns = [Gun(g) for g in GUNS]
    rows = []
    if '--inc' not in argv:
        conventions(guns)
    for gun in guns:
        if '--inc' in argv:
            import io
            import contextlib
            with contextlib.redirect_stdout(io.StringIO()):
                analyse(gun, rows)
        else:
            analyse(gun, rows)
    if '--inc' in argv:
        emit_inc(rows)
        return
    knife()
    if '--swing' in argv:
        swing(guns, rows)
    table(rows)


PSKDIR = PSADIR = ARMSDIR = None
if __name__ == '__main__':
    main(sys.argv[1:])
