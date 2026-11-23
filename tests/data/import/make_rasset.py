# Writes skin.rasset (the test of the .rasset importer, #164): the exchange format as the Tlender's Export > Real Engine
# writes it (its io_rasset.h, version 1). A 2-bone rig ("Root" and "Tip", both along +Z) bending a strip ("Body": 7
# vertices, one split by a UV seam with the same source vertex: welded back to 6) skinned by weights, a morph target
# ("Bulge") on its middle row; a cube with hard edges ("Box": 24 vertices, no source: welded by position to 8, its 12
# edges sharp); a material with a 4x4 PNG carried in the file; a point light, a camera; the animation "Bend" (the tip
# turning 60 degrees about its X over 11 frames, the root constant; an object track that doesn't move; the weight of
# "Bulge" going 0 -> 1 with it: a morph curve).
# python make_rasset.py  ->  skin.rasset next to this script
import math, os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))

strs = bytearray(b"\0")
names = {}
def S(t):  # a name's offset in "STR " (each name once)
    if t not in names:
        names[t] = len(strs)
        strs.extend(t.encode() + b"\0")
    return names[t]

blob = bytearray(16)  # the arrays (none at 0: an offset 0 is "absent"); made absolute when the blob's place is known
def arr(data):
    while len(blob) % 16: blob.append(0)
    o = len(blob)
    blob.extend(data)
    return o  # relative to the blob; made absolute when written

def f32(*v): return struct.pack("<%df" % len(v), *v)

# --- the rig: both bones along +Z (Blender's bone Y): the rotation of +90 degrees about X
RX = [[1, 0, 0], [0, 0, 1], [0, -1, 0]]  # columns X, Y, Z of the rest rotation
def rest(head):  # column-major 4x4
    return [RX[0][0], RX[0][1], RX[0][2], 0, RX[1][0], RX[1][1], RX[1][2], 0, RX[2][0], RX[2][1], RX[2][2], 0, head[0], head[1], head[2], 1]
def inv_rigid(m):
    r = [[m[0], m[1], m[2]], [m[4], m[5], m[6]], [m[8], m[9], m[10]]]  # columns
    t = m[12:15]
    rt = [[r[0][0], r[1][0], r[2][0]], [r[0][1], r[1][1], r[2][1]], [r[0][2], r[1][2], r[2][2]]]  # columns of R^T
    tt = [-(rt[0][k] * t[0] + rt[1][k] * t[1] + rt[2][k] * t[2]) for k in range(3)]
    return [rt[0][0], rt[0][1], rt[0][2], 0, rt[1][0], rt[1][1], rt[1][2], 0, rt[2][0], rt[2][1], rt[2][2], 0, tt[0], tt[1], tt[2], 1]
# quaternion (x y z w) of the rest rotation: +90 degrees about X
QX90 = [math.sin(math.pi / 4), 0, 0, math.cos(math.pi / 4)]
bones = bytearray()
bones += struct.pack("<Ii", S("Root"), -1) + f32(0, 0, 0, *QX90, 1, 1, 1) + f32(*inv_rigid(rest((0, 0, 0)))) + f32(1.0) + struct.pack("<I", 1)
bones += struct.pack("<Ii", S("Tip"), 0) + f32(0, 1, 0, 0, 0, 0, 1, 1, 1, 1) + f32(*inv_rigid(rest((0, 0, 1)))) + f32(1.0) + struct.pack("<I", 1)
bones_at = arr(bones)

# --- Body: 3 rows (z 0, 1, 2) of 2 vertices, and vertex 6 = vertex 2 with another UV (the seam)
pos = [(-0.1, 0, 0), (0.1, 0, 0), (-0.1, 0, 1), (0.1, 0, 1), (-0.1, 0, 2), (0.1, 0, 2), (-0.1, 0, 1)]
uvs = [(0, 0), (1, 0), (0, 0.5), (1, 0.5), (0, 1), (1, 1), (0.5, 0.5)]
src = [0, 1, 2, 3, 4, 5, 2]
joints = [(0, 0, 0, 0), (0, 0, 0, 0), (0, 1, 0, 0), (0, 1, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0), (0, 1, 0, 0)]
weights = [(1, 0, 0, 0), (1, 0, 0, 0), (0.5, 0.5, 0, 0), (0.5, 0.5, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0), (0.5, 0.5, 0, 0)]
idx = [0, 1, 3, 0, 3, 6, 2, 3, 5, 2, 5, 4]
bulge = [(0, 0, 0), (0, 0, 0), (-0.1, 0, 0), (0.1, 0, 0), (0, 0, 0), (0, 0, 0), (-0.1, 0, 0)]
body = dict(
    positions=arr(b"".join(f32(*p) for p in pos)), normals=arr(b"".join(f32(0, -1, 0) for _ in pos)),
    uvs=arr(b"".join(f32(*u) for u in uvs)), joints=arr(b"".join(struct.pack("<4H", *j) for j in joints)),
    weights=arr(b"".join(f32(*w) for w in weights)), indices=arr(struct.pack("<%dI" % len(idx), *idx)),
    submeshes=arr(struct.pack("<IIiI", 0, len(idx), 0, 0)), source=arr(struct.pack("<%dI" % len(src), *src)))
bulge_at = arr(b"".join(f32(*d) for d in bulge))

# --- Box: a 1 m cube, 4 vertices per face with its normal (hard edges), no source
bp, bn, bu, bi = [], [], [], []
for axis in range(3):
    for sgn in (-1, 1):
        n = [0, 0, 0]; n[axis] = sgn
        u = [0, 0, 0]; u[(axis + 1) % 3] = 1
        w = [0, 0, 0]; w[(axis + 2) % 3] = 1
        base = len(bp)
        for (a, b) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            bp.append(tuple(0.5 * (n[k] + a * u[k] + b * w[k]) for k in range(3)))
            bn.append(tuple(n)); bu.append(((a + 1) / 2, (b + 1) / 2))
        q = [base, base + 1, base + 2, base, base + 2, base + 3]
        bi += q if sgn > 0 else [q[0], q[2], q[1], q[3], q[5], q[4]]
box = dict(positions=arr(b"".join(f32(*p) for p in bp)), normals=arr(b"".join(f32(*n) for n in bn)), uvs=arr(b"".join(f32(*u) for u in bu)),
           joints=0, weights=0, indices=arr(struct.pack("<%dI" % len(bi), *bi)), submeshes=arr(struct.pack("<IIiI", 0, len(bi), 0, 0)), source=0)

# --- a 4x4 checker PNG
def png(w, h, rgb):
    raw = b"".join(b"\0" + b"".join(bytes(rgb(x, y)) for x in range(w)) for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
img = png(4, 4, lambda x, y: (230, 230, 230) if (x + y) % 2 else (40, 40, 40))
img_at = arr(img)

# --- the animation: Tip about its X (0 -> 60 degrees), Root constant, Box's node constant
NF = 11
tip = b""
for i in range(NF):
    a = math.radians(60.0 * i / (NF - 1))
    tip += f32(0, 1, 0, math.sin(a / 2), 0, 0, math.cos(a / 2), 1, 1, 1)
tip_at = arr(tip)
root_at = arr(f32(0, 0, 0, *QX90, 1, 1, 1))
boxn_at = arr(f32(2, 0, 0.5, 0, 0, 0, 1, 1, 1, 1))
def tracks(A):  # (inside the blob: its sample offsets made absolute by build)
    return struct.pack("<IIIIQ", 1, 0, 1, NF, A(tip_at)) + struct.pack("<IIIIQ", 1, 0, 0, 1, A(root_at)) + struct.pack("<IIIIQ", 0, 2, 0, 1, A(boxn_at))
tracks_at = arr(tracks(lambda o: 0))
mw_at = arr(f32(*[i / (NF - 1) for i in range(NF)]))

# --- the records (array offsets made absolute below: blob offset + where the blob lands)
def node(name, parent, kind, ref, t=(0, 0, 0), r=(0, 0, 0, 1), s=(1, 1, 1)):
    return struct.pack("<IiIi", S(name), parent, kind, ref) + f32(*t, *r, *s) + struct.pack("<II", 0, 0)
nodes = node("Rig", -1, 2, 0) + node("Body", 0, 1, 0) + node("Box", -1, 1, 1, (2, 0, 0.5)) + node("Lamp", -1, 3, 0, (0, -2, 3)) + \
        node("Cam", -1, 4, 0, (0, -6, 1), (math.sin(math.pi / 4), 0, 0, math.cos(math.pi / 4)))

recs = {}
def build(blob_at):
    A = lambda o: o + blob_at if o else 0
    t = tracks(A)
    blob[tracks_at:tracks_at + len(t)] = t
    def mesh(name, m, nv, ni, skel, flags):
        return struct.pack("<IIIIiI", S(name), nv, ni, 1, skel, flags) + struct.pack("<8Q", A(m["positions"]), A(m["normals"]), A(m["uvs"]), A(m["joints"]),
               A(m["weights"]), A(m["indices"]), A(m["submeshes"]), A(m["source"])) + f32(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1) + f32(-1, -1, -1, 1, 1, 1)
    recs["NODE"] = (5, nodes)
    recs["MESH"] = (2, mesh("Body", body, len(pos), len(idx), 0, 7) + mesh("Box", box, len(bp), len(bi), -1, 3))
    recs["MATL"] = (1, struct.pack("<II", S("Paint"), 0) + f32(0.8, 0.2, 0.1, 1, 0, 0.5, 1.5, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0.5, 0, 0, 0) +
                    struct.pack("<7iI", 0, -1, -1, -1, -1, -1, -1, 0))
    recs["TEXR"] = (1, struct.pack("<IIIiII", S("checker.png"), S("C:/tex/checker.png"), S("png"), -1, 0, 0) + f32(0, 0, 1, 1, 0) + struct.pack("<IQQ", 0, A(img_at), len(img)))
    recs["SKEL"] = (1, struct.pack("<IIQ", S("Rig"), 2, A(bones_at)))
    recs["LGHT"] = (1, struct.pack("<II", S("Lamp"), 0) + f32(1, 0.9, 0.8, 100, 0.1, 0, 0) + struct.pack("<I", 0) + f32(0, 0) + struct.pack("<II", 0, 0))
    recs["CAMR"] = (1, struct.pack("<II", S("Cam"), 0) + f32(50, 36, 24, 0.7, 16 / 9, 1, 1, 0.1, 100, 0, 0, 10, 0) + struct.pack("<I", 0))
    recs["ANIM"] = (1, struct.pack("<IfiIiIQ", S("Bend"), 24.0, 1, NF, 1, 3, A(tracks_at)))
    recs["MRPH"] = (1, struct.pack("<IIIIQ", S("Bulge"), 0, 0, 0, A(bulge_at)) + f32(0.3, 0, 1) + struct.pack("<I", 0))
    recs["MWGT"] = (1, struct.pack("<IIIIQ", 0, 0, NF, 0, A(mw_at)))

TAGS = ["STR ", "NODE", "MESH", "MATL", "TEXR", "SKEL", "LGHT", "CAMR", "ANIM", "MRPH", "MWGT", "BLOB"]
def layout(blob_at):
    build(blob_at)
    at = 64 + 24 * len(TAGS)
    out = []
    for t in TAGS:
        at = (at + 15) & ~15
        if t == "STR ": data, n = bytes(strs), len(strs)
        elif t == "BLOB": data, n = bytes(blob), len(blob)
        else: n, data = recs[t]
        out.append((t, n, at, data))
        at += len(data)
    return out, at
# laid out once to learn where the blob lands (the names are all known by then), then with it
layout(0)
out, end = layout(0)
blob_at = [o for o in out if o[0] == "BLOB"][0][2]
out, end = layout(blob_at)
f = bytearray(struct.pack("<8sIIQIfII24s", b"RASSET\0\0", 1, len(TAGS), end, 0, 1.0, 1, 0, b""))
for (t, n, at, data) in out: f += struct.pack("<4sIQQ", t.encode(), n, at, len(data))
for (t, n, at, data) in out:
    while len(f) < at: f.append(0)
    f += data
open(os.path.join(HERE, "skin.rasset"), "wb").write(f)
print("skin.rasset: %d bytes" % len(f))
