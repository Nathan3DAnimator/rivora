// Writes the glTF import test's files (node make_gltf.js): test.glb (an embedded image, a node hierarchy, a mesh used
// by two nodes of the same name, a primitive without indices, one without a material, a spot light pointing down, a
// skinned body over two joints with an animation and a mesh under a joint) and test.gltf (a data URI
// buffer, an external image whose URI has %20, a triangle strip with a morph target at half weight, a camera). The expected counts are in CMakeLists.txt.
const fs = require("fs");
const path = require("path");
const dir = __dirname;
const png = fs.readFileSync(path.join(dir, "tex logo.png"));

function f32(a) { return Buffer.from(new Float32Array(a).buffer); }
function u16(a) { const b = Buffer.from(new Uint16Array(a).buffer); return b.length % 4 ? Buffer.concat([b, Buffer.alloc(4 - b.length % 4)]) : b; }
function pad4(b) { return b.length % 4 ? Buffer.concat([b, Buffer.alloc(4 - b.length % 4)]) : b; }

// A buffer from parts: their bufferViews
function pack(parts) {
    const views = [];
    let off = 0;
    for (const p of parts) {
        views.push({ buffer: 0, byteOffset: off, byteLength: p.data.length, ...(p.target ? { target: p.target } : {}) });
        off += pad4(p.data).length;
    }
    return { bin: Buffer.concat(parts.map(p => pad4(p.data))), views };
}

// --- test.glb ---
{
    const quad = f32([-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1]);
    const quad_uv = f32([0, 0, 1, 0, 1, 1, 0, 1]);
    const quad_idx = u16([0, 2, 1, 0, 3, 2]);
    const tri = f32([-1, 0, 0, 1, 0, 0, 0, 2, 0]); // (no indices)
    const { bin, views } = pack([
        { data: quad, target: 34962 }, { data: quad_uv, target: 34962 }, { data: Buffer.from(new Uint16Array([0, 2, 1, 0, 3, 2]).buffer), target: 34963 },
        { data: tri, target: 34962 }, { data: png },
        // the skinned body: a column of two quads over two joints, the joint 1 bending in "Bend"
        { data: f32([-0.2, 0, 0, 0.2, 0, 0, -0.2, 1, 0, 0.2, 1, 0, -0.2, 2, 0, 0.2, 2, 0]) },                 // 5 positions
        { data: Buffer.from(new Uint8Array([0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0])) }, // 6 joints
        { data: f32([1, 0, 0, 0, 1, 0, 0, 0, 0.6, 0.4, 0, 0, 0.6, 0.4, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) },          // 7 weights
        { data: Buffer.from(new Uint16Array([0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4]).buffer) },                     // 8 indices
        { data: f32([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1]) }, // 9 inverse binds
        { data: f32([0, 1]) },                                                                               // 10 times
        { data: f32([0, 0, 0, 1, 0, 0, 0.3826834, 0.9238795]) },                                             // 11 rotations
        { data: Buffer.from(new Uint8Array([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255])) },             // 12 the Tri's vertex colors (M5)
    ]);
    const j = {
        asset: { version: "2.0", generator: "Real Engine test" },
        scene: 0,
        scenes: [{ nodes: [0, 3, 4, 5, 9] }],
        extensionsUsed: ["KHR_lights_punctual"],
        extensions: { KHR_lights_punctual: { lights: [{ type: "spot", color: [1, 0.9, 0.8], intensity: 500, spot: { outerConeAngle: 0.6 } }] } },
        nodes: [
            { name: "Root", translation: [0, 0, -2], children: [1, 2] },
            { name: "Box", mesh: 0, rotation: [0, 0.7071068, 0, 0.7071068] },
            { name: "Box", mesh: 0, scale: [2, 2, 2], translation: [3, 0, 0] },
            { name: "Tri", mesh: 1, matrix: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -3, 0, 0, 1] },
            { name: "Spot", translation: [0, 4, 0], rotation: [-0.7071068, 0, 0, 0.7071068], extensions: { KHR_lights_punctual: { light: 0 } } }, // (down)
            { name: "Skel", translation: [5, 0, 0], children: [6] },
            { name: "J0", children: [7] },
            { name: "J1", translation: [0, 1, 0], children: [8] },
            { name: "Cap", mesh: 1, translation: [0, 1, 0] }, // (under a joint: a part of J1)
            { name: "Body", mesh: 2, skin: 0 },
        ],
        skins: [{ name: "Body", joints: [6, 7], inverseBindMatrices: 8 }],
        animations: [{ name: "Bend", channels: [{ sampler: 0, target: { node: 7, path: "rotation" } }], samplers: [{ input: 9, output: 10 }] }],
        meshes: [
            { name: "Box", primitives: [
                { attributes: { POSITION: 0, TEXCOORD_0: 1 }, indices: 2, material: 0 },
                { attributes: { POSITION: 3 }, material: 1 },
            ] },
            { name: "Tri", primitives: [{ attributes: { POSITION: 3, COLOR_0: 11 } }] }, // (vertex colors: normalized bytes)
            { name: "Body", primitives: [{ attributes: { POSITION: 4, JOINTS_0: 5, WEIGHTS_0: 6 }, indices: 7, material: 0 }] },
        ],
        materials: [
            { name: "Painted", pbrMetallicRoughness: { baseColorFactor: [1, 0.5, 0.5, 1], baseColorTexture: { index: 0 }, metallicFactor: 0 } },
            { name: "Metal", pbrMetallicRoughness: { metallicRoughnessTexture: { index: 0 }, roughnessFactor: 0.5 }, normalTexture: { index: 0, scale: 0.8 },
              emissiveFactor: [1, 0.2, 0], extensions: { KHR_materials_emissive_strength: { emissiveStrength: 3 } } },
        ],
        textures: [{ source: 0, sampler: 0 }],
        samplers: [{ magFilter: 9729, wrapS: 33071 }],
        images: [{ name: "Logo", bufferView: 4, mimeType: "image/png" }],
        accessors: [
            { bufferView: 0, componentType: 5126, count: 4, type: "VEC3", min: [-1, 0, -1], max: [1, 0, 1] },
            { bufferView: 1, componentType: 5126, count: 4, type: "VEC2" },
            { bufferView: 2, componentType: 5123, count: 6, type: "SCALAR" },
            { bufferView: 3, componentType: 5126, count: 3, type: "VEC3", min: [-1, 0, 0], max: [1, 2, 0] },
            { bufferView: 5, componentType: 5126, count: 6, type: "VEC3", min: [-0.2, 0, 0], max: [0.2, 2, 0] },
            { bufferView: 6, componentType: 5121, count: 6, type: "VEC4" },
            { bufferView: 7, componentType: 5126, count: 6, type: "VEC4" },
            { bufferView: 8, componentType: 5123, count: 12, type: "SCALAR" },
            { bufferView: 9, componentType: 5126, count: 2, type: "MAT4" },
            { bufferView: 10, componentType: 5126, count: 2, type: "SCALAR", min: [0], max: [1] },
            { bufferView: 11, componentType: 5126, count: 2, type: "VEC4" },
            { bufferView: 12, componentType: 5121, normalized: true, count: 3, type: "VEC4" },
        ],
        bufferViews: views,
        buffers: [{ byteLength: bin.length }],
    };
    let js = Buffer.from(JSON.stringify(j));
    js = Buffer.concat([js, Buffer.alloc((4 - js.length % 4) % 4, 0x20)]);
    const head = Buffer.alloc(12), jh = Buffer.alloc(8), bh = Buffer.alloc(8);
    head.write("glTF", 0), head.writeUInt32LE(2, 4), head.writeUInt32LE(12 + 8 + js.length + 8 + bin.length, 8);
    jh.writeUInt32LE(js.length, 0), jh.writeUInt32LE(0x4E4F534A, 4);
    bh.writeUInt32LE(bin.length, 0), bh.writeUInt32LE(0x004E4942, 4);
    fs.writeFileSync(path.join(dir, "test.glb"), Buffer.concat([head, jh, js, bh, bin]));
}

// --- test.gltf ---
{
    const strip = f32([0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 2, 0, 1, 2, 0]);
    const strip_uv = f32([0, 0, 1, 0, 0, 0.5, 1, 0.5, 0, 1, 1, 1]);
    const lift = f32([0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 2, 0, 0, 2]); // the morph: bends the ribbon forward
    const { bin, views } = pack([{ data: strip }, { data: strip_uv }, { data: lift }]);
    const j = {
        asset: { version: "2.0" },
        scenes: [{ nodes: [0, 1] }],
        // the camera 5 m in front looking back at the ribbon (glTF's cameras look down -Z), 40° tall at 16:9
        nodes: [{ name: "Ribbon", mesh: 0 }, { name: "Shot", camera: 0, translation: [0.5, 1, 5] }],
        cameras: [{ type: "perspective", perspective: { yfov: 0.6981317, aspectRatio: 1.7777778, znear: 0.1 } }],
        meshes: [{ primitives: [{ attributes: { POSITION: 0, TEXCOORD_0: 1 }, targets: [{ POSITION: 2 }], mode: 5, material: 0 }], weights: [0.5] }],
        materials: [{ name: "Ribbon", pbrMetallicRoughness: { baseColorTexture: { index: 0 } }, alphaMode: "MASK" }],
        textures: [{ source: 0 }],
        images: [{ uri: "tex%20logo.png" }],
        accessors: [
            { bufferView: 0, componentType: 5126, count: 6, type: "VEC3", min: [0, 0, 0], max: [1, 2, 0] },
            { bufferView: 1, componentType: 5126, count: 6, type: "VEC2" },
            { bufferView: 2, componentType: 5126, count: 6, type: "VEC3", min: [0, 0, 0], max: [0, 0, 2] },
        ],
        bufferViews: views,
        buffers: [{ byteLength: bin.length, uri: "data:application/octet-stream;base64," + bin.toString("base64") }],
    };
    fs.writeFileSync(path.join(dir, "test.gltf"), JSON.stringify(j, null, 1));
}
console.log("written");
