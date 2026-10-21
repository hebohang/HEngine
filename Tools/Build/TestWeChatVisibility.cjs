'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),esbuild=require('../Scripting/node_modules/esbuild');
const root=path.resolve(__dirname,'../..'),scope={};
vm.runInNewContext(esbuild.buildSync({entryPoints:[path.join(root,'Engine/Source/Runtime/Platform/WeChat/Visibility.js')],bundle:true,write:false,format:'iife',globalName:'Visibility'}).outputFiles[0].text,scope);
const {meshBounds,geometryBounds,transformBounds,nearBounds,frustumPlanes,visibleBounds}=scope.Visibility;
const identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],planes=frustumPlanes(identity);
assert.ok(visibleBounds({min:[-.5,-.5,-.5],max:[.5,.5,.5]},planes));
assert.ok(!visibleBounds({min:[2,-.5,-.5],max:[3,.5,.5]},planes));
assert.ok(visibleBounds(null,planes),'unknown bounds remain visible');
assert.ok(nearBounds(null,100,100,1),'unknown bounds must not unload geometry');
assert.equal(meshBounds({primitives:[{attributes:{POSITION:0}}],accessorVec:[{min:[],max:[]}]}),null,'empty imported bounds are unknown');
assert.equal(JSON.stringify(geometryBounds({parts:[{position:new Float32Array([-2,0,-1,3,4,5])}]})),JSON.stringify({min:[-2,0,-1],max:[3,4,5]}),'imported mesh bounds recovered from real vertex positions');
const moved=identity.slice();moved[12]=70;
const bounds=transformBounds({min:[-20,0,-20],max:[20,2,20]},moved);
assert.ok(nearBounds(bounds,50,0,0),'point on extent is retained');
assert.ok(!nearBounds(bounds,0,0,49),'distance uses nearest point on bounds');
if(process.argv[2]){
    const project=path.resolve(process.argv[2]),catalog=JSON.parse(fs.readFileSync(path.join(project,'Config/GameplayCatalog.json')));
    const terrain={star:'NorthRidgeUpper',thunder:'WestShrine',water:'Foundation',wind:'EastBasin'};
    for(const [id,region] of Object.entries(catalog.regions)){
        const [x,,z]=region.authoredPosition;
        assert.ok(Math.hypot(x,z)>55,'reproduce old origin-only rejection at '+id);
        for(const name of ['Foundation',terrain[id]]){
            const mesh=JSON.parse(fs.readFileSync(path.join(project,`Assets/Models/VerdantRift/TerrainPlate_${name}.hmesh`)));
            const world=transformBounds(meshBounds(mesh),identity);
            assert.ok(nearBounds(world,x,z,0),'authored ground contains region '+id);
            assert.ok(nearBounds(world,x,z,55),'ground retained throughout '+id);
        }
    }
}
console.log('unit-test PASS: conservative bounds, frustum rejection, transformed extents and all four authored purification-region ground regressions');
