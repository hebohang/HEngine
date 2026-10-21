function () {
    const w=globalThis.__gameContextWindow, s=w.HEWeChatSession;
    if (!s?.scripts || !s.isReady()) throw new Error('Real C# game must be running.');
    if (w.__hePerformanceProbe && !w.__hePerformanceProbe.done) throw new Error('Measurement already running.');
    const result={done:false,status:'RUNNING',identity:s.scripts.identity(),startup:s.getMetrics(),phases:[]};
    w.__hePerformanceProbe=result;
    let phase, previous=null, observerFrames=0;
    const begin=name=>{
        const metrics=s.getMetrics(), bridge=s.scripts.metrics().bridge;
        phase={name,intervalsMs:[],gc:[],startFrames:metrics.frames,startCalls:bridge.calls,startBridgeMs:bridge.milliseconds};
        result.phases.push(phase); previous=null; observerFrames=0;
    };
    begin('enabled');
    function tick() {
        try {
            const now=Date.now();
            if (previous!==null) phase.intervalsMs.push(now-previous);
            previous=now; observerFrames++;
            const live=s.getMetrics(), realFrames=live.frames-phase.startFrames;
            if (!s.isReady() || live.errors.length) throw new Error('Engine stopped or reported an error during measurement.');
            if (phase.name==='forcedGc' && realFrames>=(phase.gc.length+1)*15 && phase.gc.length<20) {
                phase.gc.push({...s.scripts.collect(),afterObserverFrame:observerFrames});
            }
            if (realFrames>=300) {
                const metrics=s.getMetrics(), bridge=s.scripts.metrics().bridge;
                phase.engineFrames=metrics.frames-phase.startFrames;
                phase.bridgeCalls=bridge.calls-phase.startCalls;
                phase.bridgeMilliseconds=bridge.milliseconds-phase.startBridgeMs;
                phase.memory=s.scripts.memory();
                if (phase.name==='enabled') { s.scripts.enabled(false); begin('disabled'); }
                else if (phase.name==='disabled') { s.scripts.enabled(true); begin('forcedGc'); }
                else {
                    result.final=s.getMetrics(); result.snapshot=s.scripts.snapshot();
                    if (phase.gc.length!==20) throw new Error('Expected 20 real managed collections.');
                    result.status='PASS'; result.done=true; return;
                }
            }
            w.requestAnimationFrame(tick);
        } catch(error) {
            try { s.scripts.enabled(true); } catch {}
            result.error=String(error.stack || error); result.status='FAIL'; result.done=true;
        }
    }
    w.requestAnimationFrame(tick);
    return true;
}
