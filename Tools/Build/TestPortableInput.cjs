const assert=require('node:assert/strict');
const path=require('node:path');
const {pathToFileURL}=require('node:url');
(async()=>{
    const {AnalogStick}=await import(pathToFileURL(path.resolve(__dirname,'../../Engine/Source/Runtime/Platform/Web/Input/AnalogStick.js')));
    const stick=new AnalogStick({radius:50,deadZone:.1});
    assert.equal(stick.begin(4,100,100),true);assert.equal(stick.begin(5,0,0),false);
    assert.equal(stick.move(5,150,150),false);assert.equal(stick.x,0);
    stick.move(4,102,100);assert.equal(stick.x,0);
    stick.move(4,200,200);assert.ok(Math.abs(Math.hypot(stick.x,stick.y)-1)<1e-10);
    assert.equal(stick.end(5),false);assert.equal(stick.end(4),true);assert.equal(stick.x,0);assert.equal(stick.id,null);
    assert.equal(stick.begin(5,10,10),true);stick.reset();assert.equal(stick.id,null);
    for(const radius of [0,-1,Infinity,NaN])assert.throws(()=>new AnalogStick({radius}));
    console.log('unit-test PASS: shared stick ownership, dead zone, normalization, release and cancellation');
})().catch(error=>{console.error(error);process.exitCode=1;});
