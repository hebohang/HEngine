'use strict';
const assert=require('node:assert/strict'),vm=require('node:vm'),path=require('node:path');
const esbuild=require('../Scripting/node_modules/esbuild');
const root=path.resolve(__dirname,'../..'),scope={};
const code=esbuild.buildSync({stdin:{contents:"export * from './Engine/Source/Runtime/Platform/Web/CanvasUI.js'; export * from './Engine/Source/Runtime/Platform/Web/ViewportLayout.js';",resolveDir:root},bundle:true,write:false,format:'iife',globalName:'UI'}).outputFiles[0].text;
vm.runInNewContext(code,scope);const {CanvasUI,hitTest,viewportLayout}=scope.UI;
for(const viewport of [{width:960,height:540,left:12,top:44,right:12,bottom:10},{width:890,height:400,left:48,top:44},{width:390,height:844,top:40,bottom:20}]){
    const layout=viewportLayout(viewport,{landscapeHeight:720,portraitWidth:480});
    const near=(a,b)=>assert.ok(Math.abs(a-b)<1e-8);
    near(layout.fullRect.x*layout.scale+layout.left,0);near(layout.fullRect.y*layout.scale+layout.top,0);
    near(layout.fullRect.width*layout.scale,viewport.width);near(layout.fullRect.height*layout.scale,viewport.height);
    const point=layout.point(layout.left+123*layout.scale,layout.top+45*layout.scale);near(point.x,123);near(point.y,45);
    let depth=0;assert.throws(()=>layout.paint({save(){depth++;},restore(){depth--;},translate(){},scale(){}},()=>{throw Error('paint failure');}));assert.equal(depth,0);
}
const draws=[],context={save(){},restore(){},beginPath(){},rect(){},clip(){},moveTo(){},arcTo(){},fill(){},fillText(){},drawImage:(...args)=>draws.push(args)};
const ui=new CanvasUI(context),card={x:20,y:30,w:300,h:240};
ui.button('Open document',{x:30,y:210,w:280,h:40},{action:'open',value:'document',hitBounds:card});
assert.equal(ui.targets.length,1);assert.equal(hitTest(ui.targets,40,50).value,'document');assert.equal(hitTest(ui.targets,40,220).value,'document');assert.equal(hitTest(ui.targets,0,0),undefined);
ui.button('Overlay',{x:20,y:30,w:50,h:50},{action:'overlay'});assert.equal(hitTest(ui.targets,40,50).action,'overlay');
ui.imageCover({width:100,height:200},{x:-20,y:-30,width:400,height:200});
const [,x,y,w,h]=draws[0];assert.ok(x<=-20&&y<=-30&&x+w>=380&&y+h>=170);
console.log('unit-test PASS: project-independent design sizes, safe/full viewport, input transform, context restoration, cover image, whole-card and topmost hit testing');
