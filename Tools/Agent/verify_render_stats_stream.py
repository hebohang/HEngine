"""Validate complete streamed render-statistics evidence after an owned run."""
import argparse
import csv
from contextlib import contextmanager
import hashlib
import json
import math
from pathlib import Path
import struct

FRAME_HEADER_SHA256 = 'ea41650d6cab964f40daf538a56f2f885a4d8ec565d7e48d09bd0bf78c3c06cb'


@contextmanager
def rows(path):
    with path.open(encoding='utf-8-sig',newline='') as stream:
        reader=csv.DictReader(stream)
        yield reader.fieldnames,reader


def validate(path, expected_frames=None, require_complete=False, expect_final_unavailable=False):
    path=Path(path)
    with rows(path) as (header, reader):
        frames=list(reader)
    if hashlib.sha256(','.join(header).encode()).hexdigest()!=FRAME_HEADER_SHA256:
        raise ValueError('Frame CSV columns changed or are missing')
    if expected_frames is not None and len(frames)!=expected_frames:
        raise ValueError('Frame row count differs from owned run receipt')
    data=Path(str(path)+'.wall-ms.bin').read_bytes()
    if len(data)!=8*len(frames): raise ValueError('Exact wall spool row count differs')
    walls=[x[0] for x in struct.iter_unpack('<d',data)]
    ids=[]; previous_frame=-1
    for i,(row,wall) in enumerate(zip(frames,walls)):
        if None in row or any(v is None for v in row.values()): raise ValueError('Incomplete frame CSV row')
        if not math.isfinite(wall) or abs(float(row['cpuWallFrameMs'])-wall)>0.00000051:
            raise ValueError('Wall spool and CSV differ')
        frame=int(row['frame'])
        if frame<=previous_frame: raise ValueError('Frame IDs are not increasing')
        previous_frame=frame
        if i<len(frames)-1 and row['cpuIntervalValid']!='1':
            raise ValueError('Non-tail CPU sample lacks its next-frame interval')
        ids.append((row['submissionTimingSampleId'],row['cpuTimingFrameId']))
    set_ids=set(ids)
    if len(set_ids)!=len(ids): raise ValueError('Duplicate CPU/sample join identity')
    if expect_final_unavailable and frames and frames[-1]['cpuIntervalValid']!='0':
        raise ValueError('Fixed-frame final CPU sample must remain unavailable')
    # Keep only retirement identities/status, never raw CSV rows. Identity keys
    # include unmeasured frames so duplicate detection retains its original scope.
    by_id={}
    submission_count=0
    with rows(Path(str(path)+'.submissions.csv')) as (_, submissions):
        for row in submissions:
            key=(row['submissionTimingSampleId'],row['cpuTimingFrameId'])
            if key in by_id: raise ValueError('Duplicate submission retirement sample')
            by_id[key]=row['status'] if key in set_ids else None
            submission_count+=1
    missing=[key for key in ids if key not in by_id]
    if missing: raise ValueError('Missing submission retirement for measured CPU frame')
    if require_complete and any(by_id[key]!='0' for key in ids):
        raise ValueError('Measured GPU submission did not complete')
    expected={(r['gpuSample'],r['graphSubmissionId']) for r in frames if r['gpuSample']!='0'}
    pass_count=0
    with rows(Path(str(path)+'.passes.csv')) as (_, passes):
        for row in passes:
            if (row['gpuSample'],row['graphSubmissionId']) not in expected:
                raise ValueError('GPU pass row is not joined to a measured frame sample')
            pass_count+=1
    with rows(Path(str(path)+'.memory.csv')) as (_, memory):
        memory_count=sum(1 for _ in memory)
    ordered=sorted(walls)
    def percentile(p):
        if not ordered: return 0.
        position=p*(len(ordered)-1); lower=int(position); upper=min(lower+1,len(ordered)-1)
        return ordered[lower]+(ordered[upper]-ordered[lower])*(position-lower)
    return dict(status='PASS',frames=len(frames),submissions=submission_count,passes=pass_count,memory=memory_count,
                p95Ms=percentile(.95),p99Ms=percentile(.99),exactWallSpool=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames-csv',type=Path,required=True)
    parser.add_argument('--expected-frames',type=int,required=True)
    parser.add_argument('--require-complete',action='store_true')
    parser.add_argument('--expect-final-unavailable',action='store_true')
    args=parser.parse_args()
    print(json.dumps(validate(args.frames_csv,args.expected_frames,args.require_complete,args.expect_final_unavailable)))
