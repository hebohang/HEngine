import assert from 'node:assert/strict';
import {validateView, ViewHost} from '../../Engine/Source/Runtime/Platform/Web/Portable/ViewHost.js';
const valid=()=>({schema:1,worldWidth:24,worldHeight:14,objects:[{id:'player',x:0,y:0,width:1,height:1,color:'#55ccff'}],actions:[{id:'start',text:'Start'}]});
validateView(valid());
for(const mutate of [
    s=>s.worldWidth=0, s=>s.worldHeight=Infinity, s=>s.objects=null, s=>s.actions=null,
    s=>s.objects[0].x=NaN, s=>s.objects[0].width=-1, s=>s.objects[0].color='red',
    s=>s.objects.push({...s.objects[0]}), s=>s.actions.push({...s.actions[0]}),
    s=>s.actions=Array(5).fill({id:'x',text:'X'})
]) {const s=valid();mutate(s);assert.throws(()=>validateView(s));}
assert.throws(()=>new ViewHost({Initialize:()=>JSON.stringify({...valid(),objects:null})},{get:()=>''},'test',{}));
console.log('Portable view contract PASS: bounds, finite geometry, colors, duplicate IDs, budgets, first-frame validation.');
