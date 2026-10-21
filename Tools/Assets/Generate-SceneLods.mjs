// Reproducible index-only LOD derivatives; never overwrite imported assets.
// Original vertex streams (including lightmap UV1), material slots and LOD0
// remain byte-for-byte unchanged. Scene authoring consumes manifest.mapping.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath, pathToFileURL} from 'node:url';

const options = {};
for (let i = 2; i < process.argv.length; i += 2) {
    if (!process.argv[i].startsWith('--') || !process.argv[i + 1]) throw Error('Expected --option value');
    options[process.argv[i].slice(2)] = process.argv[i + 1];
}
if (!options.project || !options.scene || !options.output) throw Error('Required: --project --scene --output');
const project = fs.realpathSync(options.project);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const resolveAsset = value => {
    const result = path.resolve(project, value);
    if (!result.startsWith(project + path.sep)) throw Error('Asset escapes project: ' + value);
    return result;
};
const output = resolveAsset(options.output);
if (!output.startsWith(path.join(project, 'Assets', 'Generated') + path.sep)) throw Error('Output must be in Assets/Generated');
const read = filename => JSON.parse(fs.readFileSync(filename, 'utf8').replace(/^\uFEFF/, ''));
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const modulePath = path.join(root, 'Examples/RenderingTests/SourceAssets/RenderingBenchmarks/Tools/Meshoptimizer025/meshopt_simplifier.mjs');
const moduleHash = hash(fs.readFileSync(modulePath));
if (moduleHash !== 'c1d23d1a1ead1251def1f98fa427b6e61d9e6f53ff32e69aaf7159d502cd77ca') throw Error('Unrecognized meshoptimizer module');
const {MeshoptSimplifier} = await import(pathToFileURL(modulePath));
await MeshoptSimplifier.ready;
const scenePath = resolveAsset(options.scene);
const scene = read(scenePath);
const meshes = [...new Set(Object.values(scene).map(e => e.MeshComponent?.Path).filter(Boolean))].sort();
const manifest = {version: 1, sourceScene: options.scene, sourceSceneHash: hash(fs.readFileSync(scenePath)),
    moduleHash, policy: 'Index-only; lock borders and normal/UV0/UV1 seams; preserve LOD0 and original buffers',
    ratios: [0.5, 0.25, 0.125], relativeErrorLimit: 0.005, mapping: {}, meshes: []};
fs.mkdirSync(output, {recursive: true});
function accessor(mesh, buffers, index) {
    const a = mesh.accessorVec[index], v = mesh.bufferViewVec[a.bufferViewIndex];
    const count = {SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4}[a.type];
    const bytes = {5126: 4, 5125: 4, 5123: 2, 5121: 1}[a.componentType];
    if (!count || !bytes || a.normalized) throw Error('Unsupported accessor');
    const buffer = buffers[v.bufferIndex], start = (v.byteOffset || 0) + (a.byteOffset || 0);
    const stride = v.byteStride || count * bytes;
    if (start < 0 || start + (a.count - 1) * stride + count * bytes > buffer.length) throw Error('Accessor bounds');
    const data = a.componentType === 5126 ? new Float32Array(a.count * count) : new Uint32Array(a.count * count);
    for (let i = 0; i < a.count; ++i) for (let c = 0; c < count; ++c) {
        const offset = start + i * stride + c * bytes;
        data[i * count + c] = a.componentType === 5126 ? buffer.readFloatLE(offset) :
            bytes === 4 ? buffer.readUInt32LE(offset) : bytes === 2 ? buffer.readUInt16LE(offset) : buffer.readUInt8(offset);
        if (!Number.isFinite(data[i * count + c])) throw Error('Non-finite accessor');
    }
    return data;
}
for (const source of meshes) {
    const filename = resolveAsset(source);
    if (filename.startsWith(output + path.sep)) throw Error('Cannot use derivative as source');
    const mesh = read(filename);
    if (mesh.skeletonPath || mesh.primitives.some(p => p.targets?.length || p.attributes.JOINTS_0 !== undefined)) continue;
    const buffers = mesh.bufferVec.map(b => fs.readFileSync(resolveAsset(b.uri)));
    const chunks = [], report = {source, sourceHash: hash(fs.readFileSync(filename)),
        buffers: mesh.bufferVec.map((b, i) => ({uri: b.uri, hash: hash(buffers[i])})), primitives: []};
    const bufferIndex = mesh.bufferVec.length;
    let offset = 0;
    for (const p of mesh.primitives) {
        if (p.mode !== 4 || p.lods?.length) throw Error('Expected static triangle LOD0');
        const material = p.materialResPath ? read(resolveAsset(p.materialResPath)) : {};
        if (material.BlendMode && material.BlendMode !== 'Opaque') continue;
        const positions = accessor(mesh, buffers, p.attributes.POSITION);
        const indices = accessor(mesh, buffers, p.indices);
        if (indices.length < 300) continue;
        const streams = ['NORMAL', 'TANGENT', 'TEXCOORD_0', 'TEXCOORD_1', 'COLOR_0'].filter(k => p.attributes[k] !== undefined)
            .map(k => accessor(mesh, buffers, p.attributes[k]));
        const vertices = positions.length / 3;
        const widths = streams.map(s => s.length / vertices), stride = widths.reduce((a, b) => a + b, 0);
        const attributes = new Float32Array(vertices * stride), locks = new Uint8Array(vertices), groups = new Map();
        for (let i = 0; i < vertices; ++i) {
            let cursor = i * stride;
            streams.forEach((s, j) => {attributes.set(s.subarray(i * widths[j], (i + 1) * widths[j]), cursor); cursor += widths[j];});
            const key = positions.subarray(i * 3, i * 3 + 3).join(',');
            if (!groups.has(key)) groups.set(key, []);
            groups.get(key).push(i);
        }
        for (const group of groups.values()) {
            const first = group[0];
            if (group.some(i => attributes.subarray(i * stride, (i + 1) * stride)
                .some((v, j) => v !== attributes[first * stride + j]))) group.forEach(i => {locks[i] = 1;});
        }
        const scale = MeshoptSimplifier.getScale(positions, 3);
        let previousCount = indices.length, previousError = 0;
        p.lods = [];
        for (const ratio of manifest.ratios) {
            const target = Math.max(3, Math.floor(indices.length * ratio / 3) * 3);
            const [simplified, error] = MeshoptSimplifier.simplifyWithAttributes(indices, positions, 3,
                attributes, stride, Array(stride).fill(1), locks, target, manifest.relativeErrorLimit, ['LockBorder']);
            if (!Number.isFinite(error) || error < 0 || error > manifest.relativeErrorLimit + 1e-6) throw Error('Invalid error');
            if (!simplified.length || simplified.length >= previousCount) continue;
            if (simplified.some(i => i >= vertices)) throw Error('Invalid LOD index');
            const bytes = Buffer.alloc(simplified.length * 4);
            simplified.forEach((value, i) => bytes.writeUInt32LE(value, i * 4));
            const viewIndex = mesh.bufferViewVec.length, index = mesh.accessorVec.length;
            mesh.bufferViewVec.push({bufferIndex, byteOffset: offset, byteLength: bytes.length, byteStride: 0, name: 'LOD indices', target: 0});
            mesh.accessorVec.push({bufferViewIndex: viewIndex, byteOffset: 0, componentType: 5125,
                count: simplified.length, max: [], min: [], name: 'LOD indices', normalized: false, type: 'SCALAR'});
            const objectSpaceError = Math.max(previousError, error * scale);
            p.lods.push({indices: index, objectSpaceError});
            chunks.push(bytes); offset += bytes.length;
            previousCount = simplified.length; previousError = objectSpaceError;
        }
        report.primitives.push({baseIndices: indices.length, lods: p.lods.map(l => ({count: mesh.accessorVec[l.indices].count, error: l.objectSpaceError}))});
    }
    if (!chunks.length) continue;
    const name = hash(Buffer.from(source)).slice(0, 24);
    const uri = path.relative(project, output).split(path.sep).join('/') + '/' + name;
    fs.writeFileSync(path.join(output, name + '.bin'), Buffer.concat(chunks));
    mesh.bufferVec.push({byteLength: offset, data: [], name: 'LOD indices', uri: uri + '.bin'});
    fs.writeFileSync(path.join(output, name + '.hmesh'), JSON.stringify(mesh, null, 2) + '\n');
    manifest.mapping[source] = uri + '.hmesh';
    report.addedIndexBytes = offset;
    manifest.meshes.push(report);
}
fs.writeFileSync(path.join(output, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
console.log(JSON.stringify({meshes: manifest.meshes.length, inputMeshes: meshes.length,
    addedIndexBytes: manifest.meshes.reduce((n, m) => n + m.addedIndexBytes, 0), manifest: path.join(output, 'manifest.json')}));
