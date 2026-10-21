// Offline, API-independent LOD authoring using the pinned official meshoptimizer
// WebAssembly distribution. Original geometry and vertices remain immutable.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath, pathToFileURL} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const project = path.join(root, 'Examples', 'RenderingTests');
const options = {};
for (let index = 2; index < process.argv.length; index += 2) {
    if (!process.argv[index].startsWith('--') || index + 1 === process.argv.length) throw Error('Expected --option value pairs');
    options[process.argv[index].slice(2)] = process.argv[index + 1];
}
const source = path.resolve(options.source || path.join(project, 'Assets/RenderingBenchmarks/IntelSponza2048BCGPU'));
const output = path.resolve(options.output || path.join(project, 'Assets/RenderingBenchmarks/IntelSponza2048BCGPULOD'));
const modulePath = path.join(project, 'SourceAssets/RenderingBenchmarks/Tools/Meshoptimizer025/meshopt_simplifier.mjs');
const pinnedModuleHash = 'c1d23d1a1ead1251def1f98fa427b6e61d9e6f53ff32e69aaf7159d502cd77ca';
const hash = buffer => crypto.createHash('sha256').update(buffer).digest('hex');
if (hash(fs.readFileSync(modulePath)) !== pinnedModuleHash) throw Error('Pinned meshoptimizer module checksum mismatch');
if (output === source || !output.startsWith(path.join(project,'Assets/RenderingBenchmarks') + path.sep)) throw Error('Expected a separate benchmark output');
const {MeshoptSimplifier} = await import(pathToFileURL(modulePath));
await MeshoptSimplifier.ready;
if (!MeshoptSimplifier.supported) throw Error('meshoptimizer WebAssembly unsupported');
const read = p => JSON.parse(fs.readFileSync(p,'utf8'));
const write = (p,v) => {fs.mkdirSync(path.dirname(p),{recursive:true});fs.writeFileSync(p,JSON.stringify(v,null,2)+'\n');};
const asset = p => path.relative(project,p).split(path.sep).join('/');
const resolveAsset = p => {
    const resolved = path.resolve(project,p);
    if (!resolved.startsWith(project+path.sep)) throw Error('Asset path escapes the project');
    return resolved;
};
function replacePaths(value,mapping) {
    if (typeof value==='string') return mapping.get(value) || value;
    if (Array.isArray(value)) return value.map(v=>replacePaths(v,mapping));
    if (value && typeof value==='object') return Object.fromEntries(Object.entries(value).map(([k,v])=>[k,replacePaths(v,mapping)]));
    return value;
}
function accessorData(mesh,buffers,index,expectedType) {
    const a=mesh.accessorVec[index], view=mesh.bufferViewVec[a.bufferViewIndex], buffer=buffers[view.bufferIndex];
    const components={SCALAR:1,VEC2:2,VEC3:3,VEC4:4}[a.type];
    const size={5126:4,5125:4,5123:2,5121:1}[a.componentType];
    if (!components || !size || a.normalized || (expectedType && a.type!==expectedType)) throw Error('Unsupported accessor layout');
    const offset=view.byteOffset+a.byteOffset, stride=view.byteStride || components*size;
    if (offset<0 || offset+(a.count-1)*stride+components*size>buffer.length) throw Error('Accessor bounds invalid');
    const result=a.componentType===5126 ? new Float32Array(a.count*components) : new Uint32Array(a.count*components);
    for (let i=0;i<a.count;++i) for (let c=0;c<components;++c) {
        const byte=offset+i*stride+c*size;
        result[i*components+c]=a.componentType===5126 ? buffer.readFloatLE(byte) : size===4 ? buffer.readUInt32LE(byte) : size===2 ? buffer.readUInt16LE(byte) : buffer.readUInt8(byte);
        if (!Number.isFinite(result[i*components+c])) throw Error('Non-finite vertex data');
    }
    return result;
}
function seamLocks(positions,attributes) {
    const locks=new Uint8Array(positions.length/3), groups=new Map();
    for (let i=0;i<locks.length;++i) {
        const key=positions.slice(i*3,i*3+3).join(',');
        if (!groups.has(key)) groups.set(key,[]);
        groups.get(key).push(i);
    }
    for (const group of groups.values()) {
        if (group.length<2) continue;
        const first=group[0];
        const seam=group.some(i=>Array.from({length:5},(_,a)=>a).some(a=>attributes[i*5+a]!==attributes[first*5+a]));
        if (seam) for (const i of group) locks[i]=1;
    }
    return locks;
}
const settings={tool:'meshoptimizer v0.25 official JS/WASM',moduleSha256:pinnedModuleHash,
    ratios:[0.5,0.25,0.125],relativeErrorLimit:0.005,flags:['LockBorder'],
    attributeWeights:{normal:[1,1,1],uv0:[1,1]},seamPolicy:'Lock coincident positions with differing normal or UV0; preserve all original vertices',
    errorPolicy:'Independent simplification from LOD0; returned relative appearance error times getScale; conservative monotonic maximum',
    vertexPolicy:'Original vertex/accessor/buffer content remains unchanged; append only index accessors/views and one external index buffer per mesh'};
const report={version:1,source:asset(source),output:asset(output),settings,status:'GENERATING',meshes:[]};
// Explicit scenes allow a benchmark trajectory to retain its full environment
// while reusing meshes from a separate source variant. Validate before writing.
const sceneInputs=fs.readdirSync(source).filter(n=>n.endsWith('.he')).sort().map(n=>path.join(source,n));
if (options.scene) {
    const explicitScene=path.resolve(options.scene);
    if (!explicitScene.startsWith(project+path.sep) || path.extname(explicitScene).toLowerCase()!=='.he' || !fs.statSync(explicitScene).isFile()) throw Error('Explicit scene must be a project .he file');
    if (!sceneInputs.includes(explicitScene)) sceneInputs.push(explicitScene);
}
const sceneNames=new Set();
for (const scene of sceneInputs) {
    const name=path.basename(scene);
    if (sceneNames.has(name)) throw Error('Scene output filename collision: '+name);
    sceneNames.add(name);
}
const configPath=path.join(source,'benchmark_config.json');
if (!fs.existsSync(configPath) && !options.scene) throw Error('Missing benchmark_config.json; supply an explicit benchmark scene');
const benchmark=fs.existsSync(configPath) ? read(configPath) : {
    sourceMeshVariant:asset(source),sourceScene:asset(path.resolve(options.scene)),
    scene:asset(path.join(output,path.basename(options.scene)))
};
report.scenes=sceneInputs.map(scene=>({source:asset(scene),output:path.basename(scene),sourceSha256:hash(fs.readFileSync(scene))}));
const previousMarker=path.join(output,'lod_generation.json');
if (fs.existsSync(previousMarker)) {
    const previous=read(previousMarker);
    if (JSON.stringify(previous.settings)!==JSON.stringify(settings) || previous.source!==report.source) throw Error('Existing LOD directory has another configuration');
    if ((previous.scenes && JSON.stringify(previous.scenes)!==JSON.stringify(report.scenes)) || (!previous.scenes && options.scene)) throw Error('Existing LOD directory has another scene provenance');
}
fs.mkdirSync(path.join(output,'Meshes'),{recursive:true});
const meshMap=new Map();
for (const filename of fs.readdirSync(path.join(source,'Meshes')).filter(n=>n.endsWith('.hmesh')).sort()) {
    const sourcePath=path.join(source,'Meshes',filename), mesh=read(sourcePath);
    const buffers=mesh.bufferVec.map(b=>fs.readFileSync(resolveAsset(b.uri)));
    const lodChunks=[];
    let byteOffset=0;
    const newBufferIndex=mesh.bufferVec.length;
    const meshReport={name:filename,sourceMetadataSha256:hash(fs.readFileSync(sourcePath)),sourceBuffers:mesh.bufferVec.map((b,i)=>({uri:b.uri,sha256:hash(buffers[i])})),primitives:[]};
    for (let primitiveIndex=0;primitiveIndex<mesh.primitives.length;++primitiveIndex) {
        const primitive=mesh.primitives[primitiveIndex];
        if (primitive.mode!==4 || primitive.targets.length || primitive.lods?.length) throw Error('Expected unmodified static triangle LOD0');
        const positions=accessorData(mesh,buffers,primitive.attributes.POSITION,'VEC3');
        const normals=accessorData(mesh,buffers,primitive.attributes.NORMAL,'VEC3');
        const uv=accessorData(mesh,buffers,primitive.attributes.TEXCOORD_0,'VEC2');
        const indices=accessorData(mesh,buffers,primitive.indices,'SCALAR');
        if (normals.length!==positions.length || uv.length/2!==positions.length/3 || indices.length%3) throw Error('Inconsistent primitive attributes');
        const attributes=new Float32Array(positions.length/3*5);
        for (let i=0;i<positions.length/3;++i) attributes.set([normals[i*3],normals[i*3+1],normals[i*3+2],uv[i*2],uv[i*2+1]],i*5);
        const locked=seamLocks(positions,attributes);
        const scale=MeshoptSimplifier.getScale(positions,3);
        if (!Number.isFinite(scale) || scale<0) throw Error('Invalid mesh simplification scale');
        let previousCount=indices.length, previousError=0;
        primitive.lods=[];
        const primitiveReport={primitive:primitiveIndex,lod0Indices:indices.length,scale,lockedSeamVertices:locked.reduce((a,b)=>a+b,0),levels:[],skipped:[]};
        for (const ratio of settings.ratios) {
            const target=Math.max(3,Math.floor(indices.length*ratio/3)*3);
            if (target>=previousCount) continue;
            const [simplified,relativeError]=MeshoptSimplifier.simplifyWithAttributes(indices,positions,3,attributes,5,[1,1,1,1,1],locked,target,settings.relativeErrorLimit,settings.flags);
            if (!Number.isFinite(relativeError) || relativeError<0 || relativeError>settings.relativeErrorLimit+1e-6) throw Error('Invalid simplification error');
            if (simplified.length===0 || simplified.length>=previousCount) {
                primitiveReport.skipped.push({ratio,reason:'Topology/seam/border/error limit prevents further index reduction',indices:simplified.length});
                continue;
            }
            for (const index of simplified) if (index>=positions.length/3) throw Error('LOD index is outside shared vertex data');
            const objectSpaceError=Math.max(previousError,relativeError*scale);
            const buffer=Buffer.alloc(simplified.length*4);
            simplified.forEach((value,index)=>buffer.writeUInt32LE(value,index*4));
            const viewIndex=mesh.bufferViewVec.length, accessorIndex=mesh.accessorVec.length;
            mesh.bufferViewVec.push({bufferIndex:newBufferIndex,byteLength:buffer.length,byteOffset,byteStride:0,name:'LOD indices',target:0});
            mesh.accessorVec.push({bufferViewIndex:viewIndex,byteOffset:0,componentType:5125,count:simplified.length,max:[],min:[],name:'LOD indices',normalized:false,type:'SCALAR'});
            primitive.lods.push({indices:accessorIndex,objectSpaceError});
            primitiveReport.levels.push({ratio,indices:simplified.length,relativeError,objectSpaceError,sha256:hash(buffer)});
            lodChunks.push(buffer);byteOffset+=buffer.length;
            previousCount=simplified.length;previousError=objectSpaceError;
        }
        meshReport.primitives.push(primitiveReport);
    }
    const outputPath=path.join(output,'Meshes',filename);
    if (lodChunks.length) {
        const binaryPath=path.join(output,'Meshes',filename.replace('.hmesh','.lods.bin'));
        fs.writeFileSync(binaryPath,Buffer.concat(lodChunks));
        mesh.bufferVec.push({byteLength:byteOffset,data:[],name:mesh.name+' LOD indices',uri:asset(binaryPath)});
    }
    write(outputPath,mesh);
    meshMap.set(asset(sourcePath),asset(outputPath));
    meshReport.additionalIndexBytes=byteOffset;
    report.meshes.push(meshReport);
    if (report.meshes.length%10===0) process.stdout.write('LOD meshes '+report.meshes.length+'\n');
}
for (const scene of report.scenes) write(path.join(output,scene.output),replacePaths(read(resolveAsset(scene.source)),meshMap));
benchmark.meshLod=settings;
benchmark.meshLodRuntime='Authored index assets only; runtime selection, image error and GPU savings require HGame validation';
write(path.join(output,'benchmark_config.json'),benchmark);
for (const name of ['credits_license.txt','LICENSE.txt']) if (fs.existsSync(path.join(source,name))) fs.copyFileSync(path.join(source,name),path.join(output,name));
report.status='PASS_OFFLINE_GENERATION';
report.additionalIndexBytes=report.meshes.reduce((sum,m)=>sum+m.additionalIndexBytes,0);
report.primitiveCount=report.meshes.reduce((sum,m)=>sum+m.primitives.length,0);
report.primitivesWithLods=report.meshes.reduce((sum,m)=>sum+m.primitives.filter(p=>p.levels.length).length,0);
write(previousMarker,report);
process.stdout.write(JSON.stringify({status:report.status,meshes:report.meshes.length,primitives:report.primitiveCount,primitivesWithLods:report.primitivesWithLods,additionalIndexBytes:report.additionalIndexBytes})+'\n');
