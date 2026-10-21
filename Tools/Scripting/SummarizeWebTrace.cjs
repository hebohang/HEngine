'use strict';
// Chrome trace ts/dur are microseconds. Correlate only the page's main thread.
const fs = require('node:fs'), path = require('node:path'), crypto = require('node:crypto'), assert = require('node:assert/strict');
const root = path.resolve(process.argv[2] || '');
const result = JSON.parse(fs.readFileSync(path.join(root,'result.json'),'utf8'));
assert.equal(result.status,'PASS');
assert.equal(result.trace?.dataLossOccurred,false);
const file = path.join(root,'browser-trace.json');
assert(fs.statSync(file).size <= 256*1024*1024,'Trace exceeds bounded analysis size.');
const bytes = fs.readFileSync(file), events = JSON.parse(bytes).traceEvents;
assert(Array.isArray(events));
const phases = ['enabled','disabled','forcedGc'];
const markers = phases.map(name => {
    const matches = events.filter(e => e.name === 'HEngine.Phase.'+name);
    assert.equal(matches.length,1,'Ambiguous phase clock marker: '+name);
    assert(Number.isFinite(matches[0].args?.data?.startTime));
    return matches[0];
});
const offsets = markers.map(e => e.ts-e.args.data.startTime*1000);
const thread = markers[0];
assert(markers.every(e => e.pid === thread.pid && e.tid === thread.tid));
const main = events.filter(e => e.pid === thread.pid && e.tid === thread.tid);
const complete = main.filter(e => e.ph === 'X' && Number.isFinite(e.dur));
const browserGc = complete.filter(e => e.name === 'MajorGC' || e.name === 'MinorGC');
const managedGc = [], starts = new Map();
for (const event of main.filter(e => e.name === 'HEngine.ForcedManagedGC').sort((a,b) => a.ts-b.ts)) {
    const id = JSON.stringify(event.id2 ?? event.id);
    assert.notEqual(id,undefined,'Missing user timing event ID.');
    if (event.ph === 'b') { assert(!starts.has(id)); starts.set(id,event); }
    else if (event.ph === 'e') {
        const start = starts.get(id); assert(start,'Unpaired managed GC end marker.');
        assert(event.ts >= start.ts);
        managedGc.push({ts:start.ts,dur:event.ts-start.ts}); starts.delete(id);
    }
}
assert.equal(starts.size,0);
assert.equal(managedGc.length,result.forcedGc.collections.length,'GC marker coverage differs from actual collections.');
function overlap(start,end,slices) {
    const ranges = slices.map(e => [Math.max(start,e.ts),Math.min(end,e.ts+e.dur)])
        .filter(([a,b]) => b>a).sort((a,b) => a[0]-b[0]);
    let sum = 0, last = -Infinity;
    for (const [a,b] of ranges) { sum += Math.max(0,b-Math.max(a,last)); last = Math.max(last,b); }
    return sum;
}
const summary = {identity:result.identity,browserVersion:result.browserVersion,eventCount:events.length,
    clockOffsetSpreadMicroseconds:Math.max(...offsets)-Math.min(...offsets),
    traceSha256:crypto.createHash('sha256').update(bytes).digest('hex'),mainThread:{pid:thread.pid,tid:thread.tid},
    browserGc:{count:browserGc.length,major:browserGc.filter(e => e.name==='MajorGC').length,
        minor:browserGc.filter(e => e.name==='MinorGC').length,maxMicroseconds:Math.max(0,...browserGc.map(e=>e.dur))},
    forcedManagedGcMarkers:{count:managedGc.length,maxMicroseconds:Math.max(0,...managedGc.map(e=>e.dur))},
    completeMainThreadEventsOver50ms:complete.filter(e=>e.dur>50000).length,phases:{},
    limitations:'Diagnostic tracing adds overhead. MajorGC/MinorGC are browser V8 events, not Mono events. Explicit managed-GC markers include the host button handler and reporting. This does not attribute natural Mono GC, scheduling stalls or GPU waits. Frame timestamps use the browser performance clock; marker offset spread reports alignment variation. Sub-millisecond overlaps are approximate. Overlap is temporal association, not causal proof; uncovered time is not assigned to a runtime.'};
for (let i=0;i<phases.length;i++) {
    const name = phases[i], marker = markers[i];
    const offset = marker.ts-marker.args.data.startTime*1000;
    const frames = JSON.parse(fs.readFileSync(path.join(root,name+'-frames.json'),'utf8'));
    assert.equal(frames.length,result[name].engineCallCpuMs.count);
    const annotated = frames.map(frame => {
        const start = offset+frame.start*1000, end = start+frame.cpu*1000;
        return {...frame,browserGcInsideEngineCallMicroseconds:overlap(start,end,browserGc),
            browserGcInsideFrameIntervalMicroseconds:overlap(start-frame.interval*1000,start,browserGc),
            forcedManagedGcInsideFrameIntervalMicroseconds:overlap(start-frame.interval*1000,start,managedGc)};
    });
    summary.phases[name] = {count:frames.length,clockOffsetMicroseconds:offset,
        engineCallsOver20ms:frames.filter(f=>f.cpu>20).length,frameIntervalsOver33ms:frames.filter(f=>f.interval>33).length,
        largestEngineCalls:annotated.sort((a,b)=>b.cpu-a.cpu).slice(0,5)};
}
fs.writeFileSync(path.join(root,'trace-summary.json'),JSON.stringify(summary,null,2));
console.log(JSON.stringify({traceSha256:summary.traceSha256,browserGc:summary.browserGc,
    forcedManagedGcMarkers:summary.forcedManagedGcMarkers,completeMainThreadEventsOver50ms:summary.completeMainThreadEventsOver50ms}));
