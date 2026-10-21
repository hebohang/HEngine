"""Normal VFX benchmark: plan by default; --run invokes bounded hectl checks."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
BUDGET = ROOT / "Tools/VFX/acceptance-budget.json"
EFFECTS = ["Assets/Effects/Authoring/" + template + "-" + variant + ".efkpkg"
           for variant in ("ember", "frost", "arcane")
           for template in ("hit", "explosion", "trail", "telegraph", "aura", "environment")][:16]
RETIREMENT = 32
ALLOCATION = ('gpu_allocated_emitters','gpu_reserved_particles','gpu_reserved_trails')
FAILURES = ('gpu_emitter_slot_failures','gpu_particle_block_failures','gpu_trail_block_failures')
NORMAL_ENVIRONMENT = {
    'HE_VFX_GPU_PARTICLES':'1',
    'HE_VFX_DISABLE_STATS':'0',
    'HE_VFX_STATS_LOG':'0',
    'HE_VFX_STRESS_MODE':'0',
    'HE_VFX_STRESS_EFFECT':'',
    'HE_VFX_VALIDATE_PACKAGES':'0',
}
NATIVE_ERROR = re.compile(r'VUID-|Vulkan validation: Validation Error:|NVRHI validation error:|'
    r'VK_ERROR_DEVICE_LOST|DXGI_ERROR_DEVICE_(REMOVED|HUNG|RESET)|NVRHI Vulkan: Device Removed|'
    r'DX12 Debug Layer:.*(?:ERROR|CORRUPTION)|\b(?:fatal error|Unhandled exception)\b',re.I)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def benchmark_environment(request_path, report_path):
    # Child-only overrides isolate the normal workload without mutating the
    # caller's environment or accidentally inheriting a stress/disabled-stats run.
    return dict(os.environ,**NORMAL_ENVIRONMENT,HE_VFX_BENCHMARK_REQUEST=str(request_path),
                HE_VFX_BENCHMARK_REPORT=str(report_path))


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def collect_native_evidence(directory, capture_path=None, capture_frame=None, log_root=None):
    directory=Path(directory)
    wrapper=(directory/'run.log').read_text(encoding='utf-8-sig',errors='replace')
    matches=re.findall(r'run HGame: STARTED pid=(\d+); stdout: ([^;\r\n]+); stderr: ([^\r\n]+)',wrapper)
    if len(matches)!=1: raise RuntimeError('Expected exactly one native HGame STARTED receipt')
    pid,stdout,stderr=matches[0]
    log_root=Path(log_root or ROOT/'out/agent/logs').resolve()
    evidence=dict(pid=int(pid),nativeCheckPassed='run HGame: PASS exit=0' in wrapper,streams={},
                  errors=[],errorCount=0,captures=[],validationScope='Retained native streams scanned for explicit errors; no claim that validation layers were enabled')
    for stream,path in [('stdout',stdout),('stderr',stderr)]:
        source=Path(path.strip()).resolve()
        if not source.is_relative_to(log_root) or not source.is_file():
            raise RuntimeError('Native log missing or outside Agent logs: '+str(source))
        destination=directory/('game.'+stream+'.log');shutil.copyfile(source,destination)
        evidence['streams'][stream]=dict(path=str(destination),sha256=sha(destination))
        with destination.open(encoding='utf-8-sig',errors='replace') as lines:
            for number,line in enumerate(lines,1):
                if NATIVE_ERROR.search(line):
                    evidence['errorCount']+=1
                    if len(evidence['errors'])<50:evidence['errors'].append(dict(stream=stream,line=number,text=line.strip()[:1800]))
                capture=re.search(r'\[Automation\] Captured frame (\d+): (.*?) \(target=([^)]*)\)',line)
                if capture:evidence['captures'].append(dict(frame=int(capture[1]),path=capture[2],target=capture[3],stream=stream,line=number))
    if capture_path is not None:
        expected=Path(capture_path).resolve()
        evidence['captureReceiptMatches']=len(evidence['captures'])==1 and all(
            c['frame']==capture_frame and Path(c['path']).resolve()==expected and c['target']=='presented-backbuffer'
            for c in evidence['captures'])
    write(directory/'native-evidence.json',evidence)
    if not evidence['nativeCheckPassed'] or evidence['errorCount'] or evidence.get('captureReceiptMatches') is False:
        raise RuntimeError('Native exit/error/capture evidence failed; inspect native-evidence.json')
    return evidence


def percentile(values, percent):
    values = sorted(values)
    return values[max(0, math.ceil(percent/100*len(values))-1)] if values else None


def exact(stats, key):
    metric = stats.get(key, {})
    value = metric.get("current")
    return value if metric.get("status") == 1 and valid_number(value) else None


def valid_number(value):
    return isinstance(value,(int,float)) and not isinstance(value,bool) and math.isfinite(value) and value>=0


def verify_candidate(configuration, pinned):
    if not isinstance(pinned,str) or not re.fullmatch(r'[0-9a-fA-F]{64}',pinned):
        raise RuntimeError('A preselected --existing-build-sha256 is required')
    if sha(expected_executable(configuration)).lower()!=pinned.lower():
        raise RuntimeError('Candidate hash changed or differs from the preselected executable')


def read_csv(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        return list(csv.DictReader(stream))


def analyze(directory, budget):
    directory = Path(directory)
    runtime = json.loads((directory / "runtime.json").read_text())
    warmup = budget["sampling"]["warmupFrames"]
    count = budget["sampling"]["sampleFramesPerRound"]
    end = warmup + count
    errors = []
    if runtime["status"] != "PASS":
        errors.append("Script failed: " + str(runtime.get("error")))
    baseline=runtime.get('baseline',{})
    retirement=[r for r in runtime['rows'] if r['script_frame']>=end+RETIREMENT]
    final=retirement[-1]['vfx'] if retirement else {}
    if not retirement or any(exact(final,k)!=0 for k in ('active_effects','active_cpu_instances')):
        errors.append('Final retirement missing or nonzero effect/CPU instance counts')
    for key in ALLOCATION+FAILURES:
        if not valid_number(baseline.get(key)) or baseline[key]!=0:
            errors.append('Missing/nonzero clean allocator baseline: '+key)
        if key in ALLOCATION and (not valid_number(final.get(key)) or final[key]!=baseline.get(key)):
            errors.append('Allocator did not return to baseline: '+key)
        if key in FAILURES and any(not valid_number(row['vfx'].get(key)) or
                row['vfx'][key]!=baseline.get(key) for row in runtime['rows']):
            errors.append('Allocator failure counter missing/changed: '+key)
    rows = read_csv(directory / "frames.csv")
    def unique_rows(records, identity, label):
        indexed = {}
        for record in records:
            key = identity(record)
            if key in indexed:
                raise ValueError('Duplicate '+label+' identity: '+str(key))
            indexed[key] = record
        return indexed

    # Reject ambiguous evidence instead of silently overwriting an earlier
    # (possibly expensive) sample. The original CSV stays intact for diagnosis.
    frames = unique_rows(rows, lambda row: int(row["frame"]), 'frame')
    cpu_frames = unique_rows(rows, lambda row: row["cpuTimingFrameId"], 'CPU frame')
    measured = [frames[f] for f in range(warmup+1,end+1) if f in frames]
    if len(measured) != count:
        errors.append("Incomplete CPU frame window")
    # Completed whole-graphics timings must match BOTH identifiers, not arrival order.
    submissions = read_csv(directory / "frames.csv.submissions.csv")
    submission_rows = unique_rows(submissions,
        lambda row: (row["submissionTimingSampleId"], row["cpuTimingFrameId"]), 'submission/CPU frame')
    complete = {key: row for key, row in submission_rows.items() if row["gpuFullGraphicsFrameSpanMs"]}
    values = {key:[] for key in ("totalCpuFrameP95Ms", "totalGpuFrameP95Ms",
        "cpuUpdateP95Ms", "cpuDrawSubmitP95Ms", "gpuSimulationP95Ms", "gpuVfxRenderP95Ms")}
    long_frames = []
    frame_by_cpu = {identity:int(row["frame"]) for identity,row in cpu_frames.items()}
    for row in measured:
        cpu = float(row["cpuWallFrameMs"])
        if valid_number(cpu): values["totalCpuFrameP95Ms"].append(cpu)
        else: errors.append('Invalid CPU frame time at '+row['frame'])
        timing = complete.get((row["submissionTimingSampleId"], row["cpuTimingFrameId"]))
        gpu = float(timing["gpuFullGraphicsFrameSpanMs"]) if timing else None
        if valid_number(gpu):
            values["totalGpuFrameP95Ms"].append(gpu)
        elif gpu is not None: errors.append('Invalid GPU frame time at '+row['frame'])
        if cpu > budget["normal"]["totalCpuFrameP95Ms"] or (gpu is not None and gpu > budget["normal"]["totalGpuFrameP95Ms"]):
            long_frames.append({"frame":int(row["frame"]), "cpu_ms":cpu, "gpu_ms":gpu,
                "submissionTimingSampleId":row["submissionTimingSampleId"], "cpuTimingFrameId":row["cpuTimingFrameId"]})
    cpu_serials = set()
    gpu_serials = {"gpu_simulation_ms":set(), "gpu_render_ms":set()}
    gpu_frames = {source:set() for source in gpu_serials}
    gpu_duplicate_frames = {source:[] for source in gpu_serials}
    peaks = {key:[] for key in ("active_cpu_instances", "particle_buffer_capacity", "draw_calls")}
    gpu_observations = []
    long_vfx_samples = []
    for row in runtime["rows"]:
        stats = row["vfx"]
        if warmup < row["script_frame"] <= end:
            if row["active_after_restart"] != 16:
                errors.append("16-effect continuity failed at script frame " + str(row["script_frame"]))
            if exact(stats,"active_effects") != 16:
                errors.append("Completed VFX update did not retain 16 effects at script frame " + str(row["script_frame"]))
            serial = stats.get("update_serial", 0)
            if not serial or serial in cpu_serials:
                errors.append("Missing/repeated CPU update identity")
            else:
                cpu_serials.add(serial)
                for source,target in (("cpu_update_ms","cpuUpdateP95Ms"),("cpu_draw_submit_ms","cpuDrawSubmitP95Ms")):
                    value = exact(stats,source)
                    if value is not None:
                        values[target].append(value)
                        if value > budget["normal"][target]:
                            long_vfx_samples.append({"script_frame":row["script_frame"],
                                "update_serial":serial,"metric":source,"milliseconds":value})
            for source in peaks:
                value = exact(stats,source)
                if value is not None:
                    peaks[source].append(value)
        for source,target,identity in (("gpu_simulation_ms","gpuSimulationP95Ms","last_gpu_simulation_submission"),
                                       ("gpu_render_ms","gpuVfxRenderP95Ms","last_gpu_render_submission")):
            submission = int(stats.get(identity,0))
            if not submission or submission in gpu_serials[source]:
                continue
            gpu_serials[source].add(submission)
            owners = [r for r in complete.values()
                      if int(r["beginGraphicsSubmissionId"]) <= submission <= int(r["completedGraphicsSubmissionId"])]
            if len(owners) != 1:
                continue
            frame = frame_by_cpu.get(owners[0]["cpuTimingFrameId"],0)
            if not warmup < frame <= end:
                continue
            if frame in gpu_frames[source]:
                gpu_duplicate_frames[source].append(frame)
                errors.append('Repeated GPU metric frame: '+source+' frame '+str(frame))
            gpu_frames[source].add(frame)
            value = exact(stats,source)
            gpu_observations.append({"frame":frame,"submission":submission,"metric":source,"milliseconds":value})
            if value is not None:
                values[target].append(value)
                if value > budget["normal"][target]:
                    long_vfx_samples.append({"frame":frame,"submission":submission,
                        "metric":source,"milliseconds":value})
    expected_gpu_frames = set(range(warmup+1,end+1))
    gpu_frame_coverage = {}
    for source,target in (("gpu_simulation_ms","gpuSimulationP95Ms"),
                          ("gpu_render_ms","gpuVfxRenderP95Ms")):
        missing = sorted(expected_gpu_frames - gpu_frames[source])
        gpu_frame_coverage[target] = dict(missingFrames=missing,
            duplicateFrames=gpu_duplicate_frames[source], observedFrames=sorted(gpu_frames[source]))
        if missing:
            errors.append('Missing GPU metric frames: '+source+' '+str(missing))
    results = {}
    for key, samples in values.items():
        p95 = percentile(samples,95)
        results[key] = {"samples":len(samples), "p50":percentile(samples,50), "p95":p95,
            "max":max(samples) if samples else None, "limit":budget["normal"][key],
            "status":"PASS" if len(samples)==count and p95 <= budget["normal"][key] and
                (key not in gpu_frame_coverage or (not gpu_frame_coverage[key]['missingFrames'] and
                    not gpu_frame_coverage[key]['duplicateFrames'])) else "FAIL"}
    for source,key in (("active_cpu_instances","cpuInstancesMax"),("particle_buffer_capacity","gpuParticleCapacityMax"),("draw_calls","drawCallsMax")):
        samples = peaks[source]
        results[key] = {"samples":len(samples), "max":max(samples) if samples else None,
            "limit":budget["normal"][key],
            "status":"PASS" if len(samples)==count and max(samples)<=budget["normal"][key] else "FAIL"}
    report = {"status":"PASS" if not errors and all(r["status"]=="PASS" for r in results.values()) else "FAIL",
        "retirement":{"baseline":baseline,"final":final,"requiredFrames":RETIREMENT,
            "scope":"Zero effect/CPU instance counts and CPU GPU-allocator ownership restored; not fence-retired GPU byte readback"},
        "errors":errors, "metrics":results, "long_frames":long_frames,
        "gpu_samples":gpu_observations, "gpu_frame_coverage":gpu_frame_coverage,
        "long_vfx_samples":long_vfx_samples,
        "excluded":"only declared warmup + 32 retirement frames",
        "cpu_vfx_world_frame_relation":"unavailable; separate script window with unique update_serial",
        "missing_gpu_samples":"FAIL, never substituted with zero"}
    write(directory / "analysis.json", report)
    return report


def expected_executable(configuration):
    if configuration not in ("Release", "RelWithDebInfo"):
        raise ValueError("Unsupported benchmark configuration")
    name = json.loads((ROOT / "Project/Project.heproject").read_text())["ExecutableName"]
    if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
        raise ValueError("Invalid project executable basename")
    return ROOT / ("out/agent/game-" + configuration.lower()) / "bin" / (name + ".exe")


def route_support(configuration):
    unsupported = []
    for name in ("HECtl.ps1", "Build.ps1"):
        source = (ROOT / "Tools/Agent" / name).read_text(encoding="utf-8-sig")
        match = re.search(r'\[ValidateSet\(([^)]*)\)\]\s*\[string\]\$Configuration',source,re.S)
        if match is None or configuration not in re.findall(r'"([^"]+)"',match.group(1)):
            unsupported.append(name)
    return {"supported":not unsupported,"unsupportedEntrypoints":unsupported}


def runtime_identity(runtime, configuration, backend):
    executable = Path(runtime["executable"]).resolve()
    expected = expected_executable(configuration).resolve()
    if executable != expected or not executable.is_file():
        raise RuntimeError("Embedded executable differs from selected configuration: " + str(executable))
    cache = expected.parent.parent / "CMakeCache.txt"
    match = re.search(r'^CMAKE_BUILD_TYPE:STRING=(.*)$',cache.read_text(),re.M)
    if match is None or match.group(1).strip() != configuration:
        raise RuntimeError("Candidate CMake build type differs from selected configuration")
    return {"executable":str(executable),"exeSha256":sha(executable),"backendRequested":backend,
        "backendEvidence":"StrictGraphicsApi and retained startup log","configuration":configuration,
        "cmakeBuildType":match.group(1).strip()}


def plan(backend, configuration="Release"):
    budget = json.loads(BUDGET.read_text())
    return {"schemaVersion":1,"backend":backend,"configuration":configuration,
        "executionStatus":"not_executed", "route":route_support(configuration),
        "expectedExecutable":str(expected_executable(configuration)),
        "developmentDifferences":"HE_BUILD_DEV_GAME=ON, PACK_GAME=OFF, HE_ENABLE_GM=ON; loose Project assets, Python observer, RenderStats and GPU development probe enabled; not a shipping FPS claim",
        "environmentOverrides":dict(NORMAL_ENVIRONMENT),
        "statisticsOverhead":"Synchronous streaming CSV/submission/memory serialization and periodic flush remain in wall/start-to-start/busy costs; not subtracted. Update/render/window subintervals exclude those write sites; GPU supply can be affected indirectly. Compare only candidates with the same measurement implementation.",
        "budgetSha256":sha(BUDGET),"warmupFrames":budget["sampling"]["warmupFrames"],
        "sampleFrames":budget["sampling"]["sampleFramesPerRound"],"rounds":budget["sampling"]["rounds"],
        "retirementFrames":RETIREMENT,"effects":EFFECTS,
        "coverageProxy":{"phase":"after all timing rounds in separate bounded processes",
            "captureFrames":60,"captures":["empty","active","empty-repeat"],
            "dependency":"Pillow","scope":"Final RGB difference coverage, not actual fragment overdraw or a performance-budget test"},
        "resourceHashes":{p:sha(ROOT / "Project" / p) for p in EFFECTS},
        "sceneSha256":sha(ROOT / "Project/Assets/Scenes/VFXAuthoringBenchmark.he"),
        "scriptSha256":sha(ROOT / "Project/Scripts/VFXAuthoringBenchmark.py")}


def coverage_proxy(empty_path, active_path, repeat_path):
    from PIL import Image, ImageChops
    with Image.open(empty_path) as a, Image.open(active_path) as b, Image.open(repeat_path) as c:
        if a.size != b.size or a.size != c.size:
            raise ValueError('Coverage captures must have identical dimensions')
        images=[im.convert('RGB') for im in (a,b,c)]
    def difference(first,second):
        channels=ImageChops.difference(first,second).split()
        maximum=ImageChops.lighter(ImageChops.lighter(channels[0],channels[1]),channels[2])
        histogram=maximum.histogram();pixels=maximum.width*maximum.height
        return dict(pixels=pixels,thresholds={str(t):dict(changedPixels=sum(histogram[t+1:]),
            coverageFraction=sum(histogram[t+1:])/pixels) for t in (1,4,16)},
            boundsAbove16=maximum.point(lambda p:255 if p>16 else 0).getbbox())
    effect=difference(images[0],images[1]);control=difference(images[0],images[2])
    valid=control['thresholds']['16']['changedPixels']==0 and effect['thresholds']['16']['changedPixels']>0
    return dict(status='PASS' if valid else 'FAIL',effectDifference=effect,controlDrift=control,
        scope='Final RGB changed-pixel coverage at fixed view/time, including compositing and postprocessing; not transparent-fragment count, overdraw layers, alpha coverage, fill-rate cost or a performance threshold',
        criterion='Measurement integrity only: repeated empty control has no >16 RGB drift and active capture has a nonzero >16 difference',
        visualStatus='NOT_REVIEWED')


def collect_coverage(directory, request, backend, configuration, pinned):
    # Each hectl capture owns bounded execution and cleanup. These independent
    # processes run only after all timing rounds; no readback enters 600 samples.
    directory.mkdir()
    captures={}
    for label,mode in [('empty','empty'),('active','active'),('empty-repeat','empty')]:
        verify_candidate(configuration,pinned)
        folder=directory/label;folder.mkdir()
        capture_request=dict(request,coverageMode=mode,coverageFrames=60)
        write(folder/'request.json',capture_request)
        image_path=folder/'frame.png'
        command=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'Tools/Agent/HECtl.ps1'),
            'game','capture','-Configuration',configuration,'-GraphicsApi',backend,'-StrictGraphicsApi',
            '-StartupScene','Assets/Scenes/VFXAuthoringBenchmark.he','-Frames','60',
            '-FixedDeltaSeconds','0.016666666666666666','-Resolution','1280x720',
            '-CapturePath',str(image_path),'-TimeoutSeconds','300']
        write(folder/'command.json',command)
        env=benchmark_environment(folder/'request.json',folder/'runtime.json')
        with (folder/'run.log').open('w',encoding='utf-8') as log:
            process=subprocess.Popen(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
            try:
                code=process.wait()
            finally:
                while process.poll() is None:
                    try: process.wait()
                    except KeyboardInterrupt: continue
        verify_candidate(configuration,pinned)
        native=collect_native_evidence(folder,image_path,60)
        if code: raise RuntimeError('Coverage capture failed: '+label)
        runtime=json.loads((folder/'runtime.json').read_text())
        identity=runtime_identity(runtime,configuration,backend)
        if runtime.get('status')!='PASS' or runtime.get('script_frames',0)<60:
            raise RuntimeError('Coverage script failed or incomplete: '+label)
        selected=[r for r in runtime.get('rows',[]) if r.get('script_frame')==60]
        if len(selected)!=1 or selected[0].get('active_after_restart')!=(16 if mode=='active' else 0):
            raise RuntimeError('Coverage workload differs at captured frame: '+label)
        if plan(backend,configuration)!=request:
            raise RuntimeError('Benchmark inputs changed during coverage capture')
        captures[label]=dict(path=str(image_path),sha256=sha(image_path),identity=identity,scriptFrame=60,
                             workload=selected[0],native=native,
                             simulationRelation='Native receipt independently verifies automation capture frame 60 and presented-backbuffer target. Script row 60 verifies workload, but exact script-to-automation frame identity is not exposed; alignment is a startup-sequence assumption, not measured proof')
    result=coverage_proxy(*(captures[label]['path'] for label in ('empty','active','empty-repeat')))
    result.update(captures=captures,phase='Separate capture processes after all performance rounds')
    write(directory/'coverage.json',result)
    return result


def run(backend, configuration="Release", pinned=None):
    request = plan(backend, configuration)
    budget = json.loads(BUDGET.read_text())
    directory = ROOT / "out/agent/rendering-evidence/vfx-benchmark" / uuid.uuid4().hex
    directory.mkdir(parents=True)
    write(directory / "request.json", request)
    hardware = {"status":"unavailable", "actualAdapterEvidence":"retained strict-backend startup logs"}
    query = shutil.which("nvidia-smi")
    if query:
        try:
            result = subprocess.run([query,"--query-gpu=name,uuid,driver_version","--format=csv,noheader"],
                capture_output=True,text=True,timeout=10)
            hardware.update(status="observed" if result.returncode == 0 else "unavailable",
                adapters=result.stdout.strip(),error=result.stderr.strip())
        except (OSError,subprocess.TimeoutExpired) as error:
            hardware["error"] = str(error)
    write(directory / "hardware.json",hardware)
    sys.path.insert(0,str(ROOT / "Tools/Agent"))
    from storage import Storage
    previous_parent = os.environ.get("HE_STORAGE_PARENT")
    report = {"status":"FAIL", "configuration":configuration,"backend":backend,
        "executionStatus":"not_executed", "request":request,"rounds":[],"pinnedExecutableSha256":pinned}
    try:
        verify_candidate(configuration,pinned)
        storage = Storage(ROOT)
        ident = storage.start(project=str(ROOT / "Project"), workflow="vfx-benchmark", paths=[str(directory)],
            outer=True, parent=previous_parent or "", owner_pid=os.getpid())
    except Exception as error:
        # No runtime is started without the outer retention barrier. Preserve
        # a reviewable failure even when the collector lock is busy.
        report["error"] = "Candidate validation or Storage registration failed: " + str(error)
        write(directory / "result.json",report)
        print(json.dumps({"status":"FAIL","executionStatus":"not_executed","evidence":str(directory)}))
        return 1
    state = "failed"
    try:
        os.environ["HE_STORAGE_PARENT"] = ident
        # hectl owns timeout, graceful exit and process reclamation. The wrapper
        # waits in finally even on Ctrl+C; it never orphans a running child.
        driver = directory / "invoke.ps1"
        driver.write_text("param([string]$Repo,[string]$Round,[string]$Backend,[string]$Configuration,[int]$Frames)\n"
            "$ErrorActionPreference='Stop'\n"
            "& (Join-Path $Repo 'hectl.bat') game check -Configuration $Configuration -GraphicsApi $Backend -StrictGraphicsApi -StartupScene Assets/Scenes/VFXAuthoringBenchmark.he -Frames $Frames -FixedDeltaSeconds 0.016666666666666666 -Resolution 1280x720 -TimeoutSeconds 300 -PerformanceWarmupFrames 0 -RenderStats -RenderStatsPath (Join-Path $Round 'frames.csv')\n"
            "exit $LASTEXITCODE\n",encoding="utf-8")
        from PIL import Image  # Required coverage dependency; fail before any runtime starts.
        if not request["route"]["supported"]:
            raise RuntimeError("Selected configuration is unsupported by hectl entrypoints: " +
                ", ".join(request["route"]["unsupportedEntrypoints"]))
        for index in range(request["rounds"]):
            verify_candidate(configuration,pinned)
            round_dir = directory / ("round-"+str(index+1))
            round_dir.mkdir()
            env = benchmark_environment(directory / "request.json",round_dir / "runtime.json")
            command = ["powershell.exe","-NoProfile","-ExecutionPolicy","Bypass","-File",str(driver),
                "-Repo",str(ROOT),"-Round",str(round_dir),"-Backend",backend,"-Configuration",configuration,
                "-Frames",str(request["warmupFrames"]+request["sampleFrames"]+RETIREMENT)]
            write(round_dir / "command.json",command)
            with (round_dir / "run.log").open("w",encoding="utf-8") as log:
                process = subprocess.Popen(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
                report["executionStatus"] = "executed"
                try:
                    code = process.wait()
                finally:
                    while process.poll() is None:
                        try:
                            process.wait()
                        except KeyboardInterrupt:
                            continue
            native=collect_native_evidence(round_dir)
            if code:
                raise RuntimeError("hectl round failed with exit " + str(code))
            verify_candidate(configuration,pinned)
            runtime = json.loads((round_dir / "runtime.json").read_text())
            identity = runtime_identity(runtime,configuration,backend)
            write(round_dir / "identity.json",identity)
            if plan(backend,configuration) != request:
                raise RuntimeError("Benchmark inputs changed during execution")
            result = analyze(round_dir,budget)
            report["rounds"].append({"round":index+1,"identity":identity,"native":native,"status":result["status"]})
        hashes = {r["identity"]["exeSha256"] for r in report["rounds"]}
        report['coverageProxy']=collect_coverage(directory/'coverage',request,backend,configuration,pinned)
        report["status"] = "PASS" if len(hashes)==1 and all(r["status"]=="PASS" for r in report["rounds"]) and report['coverageProxy']['status']=='PASS' else "FAIL"
        state = "success" if report["status"]=="PASS" else "failed"
    except Exception as error:
        report["error"] = type(error).__name__ + ": " + str(error)
    finally:
        if report['executionStatus']=='executed':
            try: verify_candidate(configuration,pinned)
            except Exception as error:
                report['status']='FAIL'; state='failed'; report['candidateError']=str(error)
        try:
            write(directory / "result.json",report)
        except Exception:
            # A run without its final report cannot be retained as successful.
            state = "failed"
            raise
        finally:
            try:
                storage.finish(ident,state)
            except Exception as error:
                write(directory / "storage-error.json",{"error":str(error),"state":state})
            finally:
                if previous_parent is None:
                    os.environ.pop("HE_STORAGE_PARENT",None)
                else:
                    os.environ["HE_STORAGE_PARENT"] = previous_parent
        if not previous_parent:
            try:
                storage.auto()
            except Exception as error:
                write(directory / "storage-auto-error.json",{"error":str(error)})
    print(json.dumps({"status":report["status"],"evidence":str(directory)}))
    return 0 if report["status"]=="PASS" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=["DX12","Vulkan"],default="DX12")
    parser.add_argument("--configuration",choices=["Release","RelWithDebInfo"],default="Release")
    parser.add_argument("--run",action="store_true")
    parser.add_argument("--existing-build-sha256",help="Required for --run; checked before and after every hectl round")
    parser.add_argument("--analyze",type=Path)
    args = parser.parse_args()
    if args.analyze:
        print(json.dumps(analyze(args.analyze,json.loads(BUDGET.read_text())),indent=2))
    elif args.run:
        sys.exit(run(args.backend,args.configuration,args.existing_build_sha256))
    else:
        print(json.dumps(plan(args.backend,args.configuration),indent=2))
