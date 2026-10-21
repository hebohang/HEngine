'use strict';
const fs=require('node:fs'), path=require('node:path');
const file=path.resolve(process.argv[2]), result=JSON.parse(fs.readFileSync(file,'utf8').replace(/^\uFEFF/,''));
if(result.status!=='PASS' || result.phases.length!==3) throw new Error('A complete successful real-frame capture is required.');
const stats=values=>{
    if(!values.length || !values.every(Number.isFinite)) throw new Error('Missing/non-finite timing data.');
    const sorted=[...values].sort((a,b)=>a-b);
    return {count:values.length,mean:values.reduce((sum,v)=>sum+v,0)/values.length,p95:sorted[Math.ceil(values.length*.95)-1],p99:sorted[Math.ceil(values.length*.99)-1],max:sorted.at(-1)};
};
const summary={measurement:result.measurement,identity:result.identity,
    firstRenderedFrameMs:result.startup.firstRenderedFrameMs,
    hostInitialFrameCpuMs:stats(result.final.cpuFrameMs),phases:result.phases.map(p=>({name:p.name,
        realEngineFrames:p.engineFrames,observerIntervalMs:stats(p.intervalsMs),
        managedToEngineBridge:{calls:p.bridgeCalls,milliseconds:p.bridgeMilliseconds,meanMs:p.bridgeCalls?p.bridgeMilliseconds/p.bridgeCalls:null},
        memory:p.memory,...(p.gc.length?{collections:stats(p.gc.map(g=>g.milliseconds)),
            intervalAfterCollectionMs:stats(p.gc.map(g=>p.intervalsMs[g.afterObserverFrame-1]).filter(Number.isFinite)),
            followingObservationCoverage:'The final collection can end the capture before the next observer callback; count reports only intervals actually observed.'}: {})}))};
fs.writeFileSync(path.join(path.dirname(file),'summary.json'),JSON.stringify(summary,null,2)+'\n');
console.log(JSON.stringify(summary,null,2));
