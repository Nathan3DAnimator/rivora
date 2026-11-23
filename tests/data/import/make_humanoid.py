#!/usr/bin/env python3
"""tests/data/import/humanoid.glb for the rig tests (notas-dev/ikrig.md "Detecção de rig"): a humanoid with Mixamo's
names (mixamorig:Hips, LeftUpLeg, LeftLeg, LeftFoot...), a tail of three bones and two ears of two, in a T pose facing
+Z (glTF's front; its left at +X), meters. Each bone's part of the skin is a closed 8-sided tube around the bone of a
known radius (the thigh 8 cm, the shin 6 cm, the upper arm 5 cm...), weighted 1 to it: Generate Bodies must find those
thicknesses, and nothing else.
    python tests/data/import/make_humanoid.py
"""
import json
import math
import os
import struct

# name, parent, head (m, relative to the parent's head), radius of its tube (m; 0: none)
BONES = [
    ("mixamorig:Hips", None, (0, 1.00, 0), 0.14),
    ("mixamorig:Spine", "mixamorig:Hips", (0, 0.12, 0), 0.13),
    ("mixamorig:Spine1", "mixamorig:Spine", (0, 0.14, 0), 0.13),
    ("mixamorig:Spine2", "mixamorig:Spine1", (0, 0.14, 0), 0.14),
    ("mixamorig:Neck", "mixamorig:Spine2", (0, 0.15, 0), 0.05),
    ("mixamorig:Head", "mixamorig:Neck", (0, 0.10, 0), 0.10),
    ("mixamorig:HeadTop_End", "mixamorig:Head", (0, 0.20, 0), 0.0),
    ("mixamorig:LeftShoulder", "mixamorig:Spine2", (0.03, 0.10, 0), 0.04),
    ("mixamorig:LeftArm", "mixamorig:LeftShoulder", (0.15, 0, 0), 0.05),
    ("mixamorig:LeftForeArm", "mixamorig:LeftArm", (0.27, 0, 0), 0.04),
    ("mixamorig:LeftHand", "mixamorig:LeftForeArm", (0.25, 0, 0), 0.035),
    ("mixamorig:LeftHandIndex1", "mixamorig:LeftHand", (0.10, 0, 0), 0.0),
    ("mixamorig:RightShoulder", "mixamorig:Spine2", (-0.03, 0.10, 0), 0.04),
    ("mixamorig:RightArm", "mixamorig:RightShoulder", (-0.15, 0, 0), 0.05),
    ("mixamorig:RightForeArm", "mixamorig:RightArm", (-0.27, 0, 0), 0.04),
    ("mixamorig:RightHand", "mixamorig:RightForeArm", (-0.25, 0, 0), 0.035),
    ("mixamorig:RightHandIndex1", "mixamorig:RightHand", (-0.10, 0, 0), 0.0),
    ("mixamorig:LeftUpLeg", "mixamorig:Hips", (0.10, -0.05, 0), 0.08),
    ("mixamorig:LeftLeg", "mixamorig:LeftUpLeg", (0, -0.43, 0), 0.06),
    ("mixamorig:LeftFoot", "mixamorig:LeftLeg", (0, -0.44, 0), 0.045),
    ("mixamorig:LeftToeBase", "mixamorig:LeftFoot", (0, -0.06, 0.12), 0.03),
    ("mixamorig:LeftToe_End", "mixamorig:LeftToeBase", (0, 0, 0.08), 0.0),
    ("mixamorig:RightUpLeg", "mixamorig:Hips", (-0.10, -0.05, 0), 0.08),
    ("mixamorig:RightLeg", "mixamorig:RightUpLeg", (0, -0.43, 0), 0.06),
    ("mixamorig:RightFoot", "mixamorig:RightLeg", (0, -0.44, 0), 0.045),
    ("mixamorig:RightToeBase", "mixamorig:RightFoot", (0, -0.06, 0.12), 0.03),
    ("mixamorig:RightToe_End", "mixamorig:RightToeBase", (0, 0, 0.08), 0.0),
    ("Tail", "mixamorig:Hips", (0, -0.02, -0.12), 0.04),
    ("Tail1", "Tail", (0, -0.05, -0.18), 0.03),
    ("Tail2", "Tail1", (0, -0.02, -0.18), 0.02),
    ("Tail3", "Tail2", (0, 0, -0.15), 0.0),
    ("LeftEar", "mixamorig:Head", (0.05, 0.15, 0), 0.02),
    ("LeftEar1", "LeftEar", (0.02, 0.12, 0), 0.015),
    ("LeftEar2", "LeftEar1", (0.01, 0.10, 0), 0.0),
    ("RightEar", "mixamorig:Head", (-0.05, 0.15, 0), 0.02),
    ("RightEar1", "RightEar", (-0.02, 0.12, 0), 0.015),
    ("RightEar2", "RightEar1", (-0.01, 0.10, 0), 0.0),
]


def main():
    idx = {b[0]: i for i, b in enumerate(BONES)}
    world = []
    for name, parent, head, _ in BONES:
        p = world[idx[parent]] if parent else (0, 0, 0)
        world.append(tuple(p[k] + head[k] for k in range(3)))
    # a tube per bone with a radius: from its head to its main child's head (the first child)
    pos, joints, weights, tris = [], [], [], []
    for i, (name, parent, head, r) in enumerate(BONES):
        if r <= 0:
            continue
        kids = [k for k, b in enumerate(BONES) if b[1] == name]
        a = world[i]
        b = world[kids[0]] if kids else tuple(a[k] + (a[k] - world[idx[parent]][k]) * 0.5 for k in range(3))
        d = [b[k] - a[k] for k in range(3)]
        L = math.sqrt(sum(x * x for x in d))
        d = [x / L for x in d]
        up = (0, 0, 1) if abs(d[2]) < 0.9 else (1, 0, 0)
        u = [d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]]
        ul = math.sqrt(sum(x * x for x in u))
        u = [x / ul for x in u]
        v = [d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]]
        base = len(pos)
        rings = 4
        for ring in range(rings + 1):
            t = ring / rings
            c = [a[k] + (b[k] - a[k]) * t for k in range(3)]
            for s in range(8):
                ang = s * math.pi / 4
                pos.append([c[k] + (u[k] * math.cos(ang) + v[k] * math.sin(ang)) * r for k in range(3)])
                joints.append([i, 0, 0, 0])
                weights.append([1.0, 0, 0, 0])
        for ring in range(rings):
            for s in range(8):
                p0 = base + ring * 8 + s
                p1 = base + ring * 8 + (s + 1) % 8
                q0, q1 = p0 + 8, p1 + 8
                tris += [p0, p1, q1, p0, q1, q0]
    nodes = []
    for i, (name, parent, head, _) in enumerate(BONES):
        n = {"name": name, "translation": list(head)}
        kids = [k for k, b in enumerate(BONES) if b[1] == name]
        if kids:
            n["children"] = kids
        nodes.append(n)
    mesh_node = len(nodes)
    nodes.append({"name": "Body", "mesh": 0, "skin": 0})
    root = len(nodes)
    nodes.append({"name": "Humanoid", "children": [0, mesh_node]})
    # buffers
    blob = bytearray()

    def add(data, fmt):
        off = len(blob)
        blob.extend(struct.pack("<%d%s" % (len(data), fmt), *data))
        while len(blob) % 4:
            blob.append(0)
        return off, len(blob) - off

    flat = [x for p in pos for x in p]
    o_pos = add(flat, "f")
    o_j = add([x for j in joints for x in j], "H")
    o_w = add([x for w in weights for x in w], "f")
    o_i = add(tris, "I")
    ibm = []
    for w in world:
        ibm += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -w[0], -w[1], -w[2], 1]
    o_ibm = add(ibm, "f")
    mn = [min(p[k] for p in pos) for k in range(3)]
    mx = [max(p[k] for p in pos) for k in range(3)]
    views = [{"buffer": 0, "byteOffset": o[0], "byteLength": o[1]} for o in (o_pos, o_j, o_w, o_i, o_ibm)]
    acc = [
        {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5123, "count": len(pos), "type": "VEC4"},
        {"bufferView": 2, "componentType": 5126, "count": len(pos), "type": "VEC4"},
        {"bufferView": 3, "componentType": 5125, "count": len(tris), "type": "SCALAR"},
        {"bufferView": 4, "componentType": 5126, "count": len(BONES), "type": "MAT4"},
    ]
    gl = {
        "asset": {"version": "2.0", "generator": "make_humanoid.py"},
        "scene": 0, "scenes": [{"nodes": [root]}], "nodes": nodes,
        "meshes": [{"name": "Body", "primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}, "indices": 3}]}],
        "skins": [{"name": "Armature", "joints": list(range(len(BONES))), "inverseBindMatrices": 4, "skeleton": 0}],
        "accessors": acc, "bufferViews": views, "buffers": [{"byteLength": len(blob)}],
    }
    js = json.dumps(gl, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
    out += struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(blob), 0x004E4942) + bytes(blob)
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "humanoid.glb")
    open(path, "wb").write(out)
    print("humanoid.glb: %d bones, %d vertices, %d triangles" % (len(BONES), len(pos), len(tris) // 3))


if __name__ == "__main__":
    main()
