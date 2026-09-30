"""Bake the manual reload's hand grips from the game's own reload animations (round 31, D21).

For each converted gun, three grips: "mag" (the off hand on the seated magazine), "hold" (the magazine out, in the
hand), "bolt" (the hand on the handle / slide). Each is the frame of the arms' reload animation (VM_AnimSet_NoBazooka)
where the left hand is closest to that part, and gives:
  * the left hand's frame in the part's bone frame (gun mesh space; the gun mesh = the arms' RightProp frame, measured
    in-game for every gun: ENGINE-NOTES 5am), and
  * the 15 finger bones' frames in the hand's (the chains Hand -> X1 -> X2 -> X3, as the arms' RefSkeleton has them).
A grip whose best frame leaves the hand more than 13 units from the part is left out (the Colt's slide and magazine
grab, the C96's: the animation's left hand never touches them) -- the mod keeps the controller's hand there.

Input: the umodel exports of the game's packages (umodel -game=moha -export: MOHAGame.xxx for the arms AnimSet,
Var_Flk_P.xxx for the gun AnimSets and meshes), laid out as psa/MOHAGame/AnimSet, psa/Var_Flk_P/AnimSet,
psk/Var_Flk_P/SkeletalMesh3. Keys map to the engine as t = (x, -y, z), q = (x, -y, z, w) (the root's w negated),
checked against the cooked ref pose in Step 0.

    python tools/reload_grips.py <export dir> > src/mohavr/reload_grips.inc
"""
import math
import os
import struct
import sys


def _chunks(data):
    p, out = 0, []
    while p + 32 <= len(data):
        cid = data[p:p + 20].split(b'\0')[0].decode('latin1')
        _, dsize, dcount = struct.unpack_from('<iii', data, p + 20)
        p += 32
        out.append((cid, dsize, dcount, p))
        p += dsize * dcount
    return out


def _bone(data, off):
    name = data[off:off + 64].split(b'\0')[0].decode('latin1')
    _, _, parent = struct.unpack_from('<Iii', data, off + 64)
    return dict(name=name, parent=parent, q=struct.unpack_from('<4f', data, off + 76), t=struct.unpack_from('<3f', data, off + 92))


def read_psk(path):
    data = open(path, 'rb').read()
    for cid, dsize, dcount, p in _chunks(data):
        if cid.startswith('REFSKEL'):
            return [_bone(data, p + i * dsize) for i in range(dcount)]
    return []


def read_psa(path):
    data = open(path, 'rb').read()
    bones, seqs, keyoff = [], [], None
    for cid, dsize, dcount, p in _chunks(data):
        if cid.startswith('BONENAMES'):
            bones = [_bone(data, p + i * dsize)['name'] for i in range(dcount)]
        elif cid.startswith('ANIMINFO'):
            for i in range(dcount):
                o = p + i * dsize
                name = data[o:o + 64].split(b'\0')[0].decode('latin1')
                rate = struct.unpack_from('<f', data, o + 152)[0]
                first, nframes = struct.unpack_from('<ii', data, o + 160)
                seqs.append(dict(name=name, rate=rate, first=first, frames=nframes))
        elif cid.startswith('ANIMKEYS'):
            keyoff = (p, dsize)
    kp, ks = keyoff
    return dict(bones=bones, seqs={s['name']: s for s in seqs}, data=data, kp=kp, ks=ks)


def qmat(q, t):
    x, y, z, w = q
    x2, y2, z2 = x + x, y + y, z + z
    xx, xy, xz, yy, yz, zz = x * x2, x * y2, x * z2, y * y2, y * z2, z * z2
    wx, wy, wz = w * x2, w * y2, w * z2
    return [[1 - (yy + zz), xy + wz, xz - wy, 0], [xy - wz, 1 - (xx + zz), yz + wx, 0], [xz + wy, yz - wx, 1 - (xx + yy), 0],
            [t[0], t[1], t[2], 1]]


def mmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inv(m):
    r = [[m[j][i] for j in range(3)] for i in range(3)]
    t = m[3][:3]
    return [r[0] + [0], r[1] + [0], r[2] + [0], [-(t[0] * r[0][j] + t[1] * r[1][j] + t[2] * r[2][j]) for j in range(3)] + [1]]


def xform(v, m):
    return tuple(v[0] * m[0][j] + v[1] * m[1][j] + v[2] * m[2][j] + m[3][j] for j in range(3))


def key_local(psa, seq, bone, f):
    i = psa['bones'].index(bone)
    f = max(0, min(seq['frames'] - 1, f))
    o = psa['kp'] + ((seq['first'] + f) * len(psa['bones']) + i) * psa['ks']
    t = struct.unpack_from('<3f', psa['data'], o)
    q = struct.unpack_from('<4f', psa['data'], o + 12)
    q = (q[0], -q[1], q[2], q[3]) if i > 0 else (q[0], -q[1], q[2], -q[3])
    return qmat(q, (t[0], -t[1], t[2]))


def gun_cs(psa, psk, seq, bone, f):
    names = [b['name'] for b in psk]
    i, m = names.index(bone), None
    while True:
        b = psk[i]
        if b['name'] in psa['bones']:
            l = key_local(psa, seq, b['name'], f)
        else:
            q = b['q'] if i == 0 else (-b['q'][0], -b['q'][1], -b['q'][2], b['q'][3])
            l = qmat((q[0], -q[1], q[2], q[3]), (b['t'][0], -b['t'][1], b['t'][2]))
        m = l if m is None else mmul(m, l)
        if i == 0 or b['parent'] == i:
            return m
        i = b['parent']


FINGERS = ['LeftHand%s%d' % (f, k) for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky') for k in (1, 2, 3)]

# key: arms seq, gun AnimSet, gun seq, psk, magazine bone, grab point (mesh, the [ManualReload] line's), action bone,
# handle offset (BoltGrab), notify times (out, in, rack; vm_reload_notifies)
GUNS = [
    ('Attachment_G43', 'g43_reload_1', 'DE_G43_AnimSet', 'g43_gun_reload_1', 'DE_G43_Rigged', 'magazine', (0, 3.0, 20.0), 'Bolt', (2.4, -1.3, -4.9), (0.33, 1.18, 2.05)),
    ('Attachment_Stg44', 'stg_reload_1', 'DE_STG44_AnimSet', 'stg_gun_reload_1', 'DE_STG44_Rigged', 'single_magazine', (0, 11.5, 19.6), 'Bolt', (2.9, 0, 0), (0.41, 1.08, 1.66)),
    ('Attachment_Bar', 'bar_reload_2', 'US_BAR_AnimSet', 'bar_gun_reload_2', 'US_BAR1918_Rigged', 'magazine', (0, 8.5, 20.2), 'Bolt', (1.7, -0.1, 2.9), (0.35, 0.86, 1.57)),
    ('Attachment_Thompson', 'thompson_reload_3', 'Thompson_Rigged_Anims', 'thompson_gun_reload_3', 'US_Thompson_Rigged', 'upgrade_03_drum', (0, 5.0, 11.5), 'Bolt', (0, 1.0, 0.5), (0.50, 1.45, 2.52)),
    ('Attachment_MP40', 'mp40_reload_2', 'MP40_AnimSet', 'mp40_gun_reload_2', 'MP40_rigged', 'upgrade_02_64rdMagazine', (-1.2, 14.4, 30.8), 'Bolt', (4.5, 0, -3.4), (0.35, 0.97, 1.53)),
    ('Attachment_Colt45', 'colt45_reload', 'US_M1911A1_AnimSet', 'colt45_gun_reload', 'US_M1911A1_Pistol_Rigged', 'magazine', (0, 6.3, -1.3), 'gunSlide', (0.1, -0.3, -4.0), (0.05, 0.72, 1.45)),
    ('Attachment_Mauser', 'mauser_reload_3', 'Mauser_AnimSet', 'mauser_gun_reload_3', 'DE_Mauser_Rigged', 'upgrade_02_magazine', (-0.1, 6.7, 11.3), 'Bolt', (0, 0.5, -0.2), (0.20, 0.39, 1.45)),
]
MAX_DIST = 13.0

# GOAL A1: guns whose reload animation works the part with the RIGHT hand (the Garand's clip; work/research/goal/garand.md
# section 4): the grip is taken at an explicit frame from the right hand and mirrored into a left one. The arms rig
# mirrors a bone's local frame by negating ALL its axes (every right knuckle's offset from its hand is the left one's
# negated, in x, y and z): so the hand is H_left = D.H.S with D = -I on the axis rows and S = the mesh X mirror, and a
# finger keeps its rotation in the hand's frame with its offset negated. (The first try, D = diag(1,-1,1) with the
# fingers D.F.D, bent every finger the wrong way.) key, arms seq, gun AnimSet, gun seq, psk, [(kind, part bone, frame)]
MIRRORED = [
    ('Attachment_M1Garand', 'm1garand_reload', 'Garand_AnimSet', 'm1garand_gun_reload', 'US_Garand_Rigged', [('hold', 'clip', 21)]),
]
MIRROR_D = [[-1, 0, 0, 0], [0, -1, 0, 0], [0, 0, -1, 0], [0, 0, 0, 1]]
MIRROR_S = [[-1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def rows(m):
    return ', '.join('%.5ff' % v for v in (m[0][:3] + m[1][:3] + m[2][:3] + m[3][:3]))


def main(root):
    arms = read_psa(os.path.join(root, 'psa', 'MOHAGame', 'AnimSet', 'VM_AnimSet_NoBazooka.psa'))
    out = ['// Generated by tools/reload_grips.py from the game\'s reload animations -- do not edit. Per gun and grip:',
           '// the left hand\'s frame in the gripped bone\'s frame, and the fingers\' frames in the hand\'s (row-major',
           '// 4x3: the X, Y, Z rows, then the translation; engine units). Fingers in kGripFingers\' order.',
           'static const char* const kGripFingers[15] = {' + ', '.join('"%s"' % f for f in FINGERS) + '};',
           'struct GripData {', '    const char* gun;', '    const char* kind;    // "mag", "hold", "bolt"',
           '    const char* bone;    // the gripped bone', '    const char* seq;     // the arms\' sequence and the time used',
           '    float       time, dist;  // s; the hand bone\'s distance from the part then (units)', '    float       hand[12];',
           '    float       fingers[15][12];', '};', 'static const GripData kGrips[] = {']
    log = []
    for key, aseq, gset, gseq, psk, magb, grab, boltb, boff, (tout, tin, track) in GUNS:
        a = arms['seqs'][aseq]
        gp = read_psa(os.path.join(root, 'psa', 'Var_Flk_P', 'AnimSet', gset + '.psa'))
        sk = read_psk(os.path.join(root, 'psk', 'Var_Flk_P', 'SkeletalMesh3', psk + '.psk'))
        g = gp['seqs'][gseq]
        mag0 = gun_cs(gp, sk, g, magb, 0)
        grabInMag = xform(grab, inv(mag0))
        n = min(a['frames'], g['frames'])

        def hand_at(f):
            return mmul(key_local(arms, a, 'LeftHand', f), inv(key_local(arms, a, 'RightProp', f)))

        cands = {'mag': [], 'hold': [], 'bolt': []}
        for f in range(n):
            t = f / a['rate']
            h = hand_at(f)
            mag = gun_cs(gp, sk, g, magb, f)
            moved = math.dist(mag[3][:3], mag0[3][:3])
            dm = math.dist(h[3][:3], xform(grabInMag, mag))
            if moved < 1.5 and t <= tin:
                cands['mag'].append((dm, f, magb, mag))
            if moved > 8.0 and tout - 0.1 <= t <= tin + 0.1:
                cands['hold'].append((dm, f, magb, mag))
            if track - 0.5 <= t <= track + 0.3:
                bolt = gun_cs(gp, sk, g, boltb, f)
                db = math.dist(h[3][:3], (bolt[3][0] + boff[0], bolt[3][1] + boff[1], bolt[3][2] + boff[2]))
                cands['bolt'].append((db, f, boltb, bolt))
        for kind in ('mag', 'hold', 'bolt'):
            if not cands[kind]:
                log.append('%s %s: no frame' % (key, kind))
                continue
            d, f, bone, part = min(cands[kind], key=lambda c: c[0])
            if d > MAX_DIST:
                log.append('%s %s: best %.1f units at %.2f s -- left out' % (key, kind, d, f / a['rate']))
                continue
            hand = mmul(hand_at(f), inv(part))
            fingers = []
            for fn in FINGERS:
                m = key_local(arms, a, fn, f)
                k = int(fn[-1])
                for lower in range(k - 1, 0, -1):  # up the chain: X3 -> X2 -> X1 (-> the hand)
                    m = mmul(m, key_local(arms, a, fn[:-1] + str(lower), f))
                fingers.append(m)
            out.append('    {"%s", "%s", "%s", "%s", %.3ff, %.1ff,' % (key, kind, bone, aseq, f / a['rate'], d))
            out.append('     {%s},' % rows(hand))
            out.append('     {' + ',\n      '.join('{%s}' % rows(m) for m in fingers) + '}},')
            log.append('%s %s: %.2f s, %.1f units' % (key, kind, f / a['rate'], d))
    for key, aseq, gset, gseq, psk, grips in MIRRORED:
        a = arms['seqs'][aseq]
        gp = read_psa(os.path.join(root, 'psa', 'Var_Flk_P', 'AnimSet', gset + '.psa'))
        sk = read_psk(os.path.join(root, 'psk', 'Var_Flk_P', 'SkeletalMesh3', psk + '.psk'))
        g = gp['seqs'][gseq]
        for kind, bone, f in grips:
            part = gun_cs(gp, sk, g, bone, f)
            right = mmul(key_local(arms, a, 'RightHand', f), inv(key_local(arms, a, 'RightProp', f)))
            d = math.dist(right[3][:3], part[3][:3])
            hand = mmul(mmul(MIRROR_D, mmul(right, inv(part))), MIRROR_S)
            fingers = []
            for fn in FINGERS:
                rn = 'Right' + fn[len('Left'):]
                m = key_local(arms, a, rn, f)
                k = int(rn[-1])
                for lower in range(k - 1, 0, -1):
                    m = mmul(m, key_local(arms, a, rn[:-1] + str(lower), f))
                fingers.append([m[0], m[1], m[2], [-m[3][0], -m[3][1], -m[3][2], 1]])
            out.append('    {"%s", "%s", "%s", "%s", %.3ff, %.1ff,' % (key, kind, bone, aseq, f / a['rate'], d))
            out.append('     {%s},' % rows(hand))
            out.append('     {' + ',\n      '.join('{%s}' % rows(m) for m in fingers) + '}},')
            log.append('%s %s: %.2f s, the right hand mirrored (%.1f units from the part)' % (key, kind, f / a['rate'], d))
    out.append('};')
    print('\n'.join(out))
    sys.stderr.write('\n'.join(log) + '\n')


if __name__ == '__main__':
    main(sys.argv[1])
