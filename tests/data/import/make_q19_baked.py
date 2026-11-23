# (Q19, notas-dev/importar-tlender.md) Makes q19_baked.tlend and its images: what the Tlender's export writes for the
# Engine (the format defined on the Engine's side): an object whose stack the Engine can't apply (Boolean) with its
# evaluated mesh ("evalmeshes"), and a material whose nodes the Tlender baked on its GPU ("bake <output> <color space>
# <file>": a procedural Lava, an Ambient Occlusion that needs the mesh).
#   python tests/data/import/make_q19_baked.py
import os, struct, zlib

here = os.path.dirname(os.path.abspath(__file__))

def png(path, w, h, px):  # px(x, y) -> (r, g, b)
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

png(os.path.join(here, 'q19_lava_baked.png'), 64, 64, lambda x, y: (255, 80 + (x * 3) % 100, 0) if (x // 8 + y // 8) % 2 else (40, 10, 5))
png(os.path.join(here, 'q19_ao_baked.png'), 64, 64, lambda x, y: (200 - abs(x - 32) * 3,) * 3)

cube = ['v -0.5 -0.5 -0.5', 'v 0.5 -0.5 -0.5', 'v 0.5 0.5 -0.5', 'v -0.5 0.5 -0.5',
        'v -0.5 -0.5 0.5', 'v 0.5 -0.5 0.5', 'v 0.5 0.5 0.5', 'v -0.5 0.5 0.5']
faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
uv = 't 0 0 0 1 0 1 1 0 1'
L = ['TLENDER 98', 'anim 1 30 1 24', 'meshes 1', 'mesh 8 6'] + cube
for f in faces:
    L += ['f 4 %d %d %d %d' % f, uv]
L += ['materials 1', 'material Lava', 'tree 4 3 -1 0 0 1',
      'node TlShaderNodeLava -400 0 0 0 0 0 0 2', 'in 1 0.3 0 1', 'in 5 0 0 0', 'value 0 0 0 0', 'ramp 0', 'label ',
      'bake 0 0 q19_lava_baked.png',
      'node ShaderNodeAmbientOcclusion -400 -300 0 0 0 0 0 3', 'in 1 1 1 1', 'in 1 0 0 0', 'in 0 0 0 0', 'value 0 0 0 0', 'ramp 0', 'label ',
      'bake 1 1 q19_ao_baked.png',
      'node ShaderNodeBsdfPrincipled -100 0 0 0 0 0 0 3', 'in 0.8 0.8 0.8 1', 'in 0 0 0 0', 'in 0.5 0 0 0', 'value 0 0 0 0', 'ramp 0', 'label ',
      'node ShaderNodeOutputMaterial 200 0 0 0 0 0 0 1', 'in 0 0 0 0', 'value 0 0 0 0', 'ramp 0', 'label ',
      'link 0 0 2 0', 'link 1 1 2 2', 'link 2 0 3 0',
      'objects 1 active 0',
      'o 0 0 0 0 1 0 0 0 1 1 1 0.8 0.8 0.8 1 0 50 1000 1 1 1 0.1 Carved',
      'slots 0 0 1 0', 'mods 2', 'md BOOLEAN 3 Boolean', 'mp operation=DIFFERENCE;object=-1', 'md SUBSURF 3 Subdivision', 'mp levels=1',
      # the Tlender's evaluated mesh of object 0: a pyramid (5 vertices, 5 faces)
      'evalmeshes 1', 'evalmesh 0 5 5',
      'v -0.5 -0.5 -0.5', 'v 0.5 -0.5 -0.5', 'v 0.5 0.5 -0.5', 'v -0.5 0.5 -0.5', 'v 0 0 0.7',
      'f 4 0 3 2 1', uv, 'f 3 0 1 4', 't 0 0 0 1 0 0.5 1', 'f 3 1 2 4', 't 0 0 0 1 0 0.5 1', 'f 3 2 3 4', 't 0 0 0 1 0 0.5 1',
      'f 3 3 0 4', 't 0 0 0 1 0 0.5 1',
      'end', '']
open(os.path.join(here, 'q19_baked.tlend'), 'w', newline='\n').write('\n'.join(L))
print('q19_baked.tlend, q19_lava_baked.png, q19_ao_baked.png')
