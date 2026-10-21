import csv
import json
from pathlib import Path
import tempfile
import unittest
import importlib.util
import os
import sys
import types
from unittest.mock import patch
from benchmark import analyze, plan, BUDGET, expected_executable, runtime_identity, route_support, run, ALLOCATION, FAILURES, verify_candidate, sha, benchmark_environment, NORMAL_ENVIRONMENT


class BenchmarkTests(unittest.TestCase):
    def test_normal_child_environment_isolated_from_previous_stress(self):
        inherited=dict(HE_VFX_DISABLE_STATS='1',HE_VFX_STRESS_MODE='1',HE_VFX_STRESS_EFFECT='unexpected.efkpkg',
                       HE_VFX_STATS_LOG='1',HE_VFX_VALIDATE_PACKAGES='1',HE_VFX_GPU_PARTICLES='0')
        with patch.dict(os.environ,inherited):
            before=dict(os.environ)
            child=benchmark_environment(Path('request.json'),Path('runtime.json'))
            self.assertEqual({key:child[key] for key in NORMAL_ENVIRONMENT},NORMAL_ENVIRONMENT)
            self.assertEqual(child['HE_VFX_BENCHMARK_REQUEST'],'request.json')
            self.assertEqual(child['HE_VFX_BENCHMARK_REPORT'],'runtime.json')
            self.assertEqual(dict(os.environ),before)

    def test_storage_start_failure_reports_without_starting_runtime(self):
        class BusyStorage:
            def __init__(self,root): pass
            def start(self,**kwargs): raise RuntimeError("collector lock busy")
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            with patch("benchmark.ROOT",root), patch("benchmark.plan",return_value={"configuration":"Release"}), \
                    patch("benchmark.shutil.which",return_value=None), patch("benchmark.verify_candidate"), \
                    patch.dict(sys.modules,{"storage":types.SimpleNamespace(Storage=BusyStorage)}), \
                    patch("benchmark.subprocess.Popen") as process, patch("builtins.print"):
                self.assertEqual(run("DX12"),1)
                process.assert_not_called()
            results=list(root.glob("out/agent/rendering-evidence/vfx-benchmark/*/result.json"))
            self.assertEqual(len(results),1)
            report=json.loads(results[0].read_text())
            self.assertEqual(report["status"],"FAIL")
            self.assertEqual(report["executionStatus"],"not_executed")
            self.assertIn("collector lock busy",report["error"])

    def fixture(self, folder, stale_sim=False, missing_gpu=False, long_frame=False):
        budget = json.loads(BUDGET.read_text())
        budget["sampling"].update(warmupFrames=1,sampleFramesPerRound=3)
        def csvfile(name,rows):
            with (folder/name).open("w",newline="") as stream:
                writer = csv.DictWriter(stream,fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
        frames = [{"frame":i,"cpuWallFrameMs":30 if long_frame and i==3 else 2,
            "submissionTimingSampleId":i,"cpuTimingFrameId":100+i} for i in range(1,6)]
        csvfile("frames.csv",frames)
        submissions = [{"submissionTimingSampleId":i,"cpuTimingFrameId":100+i,
            "beginGraphicsSubmissionId":i*10,"completedGraphicsSubmissionId":i*10+9,
            "gpuFullGraphicsFrameSpanMs":"" if missing_gpu and i==3 else 3} for i in range(1,6)]
        # Out-of-order completions must still match their originating frame.
        csvfile("frames.csv.submissions.csv",list(reversed(submissions)))
        rows = []
        for i in range(1,6):
            stats = {key:{"status":1,"current":value} for key,value in (
                ("cpu_update_ms",.1),("cpu_draw_submit_ms",.2),("gpu_simulation_ms",.3),
                ("gpu_render_ms",.4),("active_effects",16),("active_cpu_instances",300),("particle_buffer_capacity",50176),("draw_calls",16))}
            stats.update({key:0 for key in ALLOCATION+FAILURES})
            stats.update(update_serial=i,last_gpu_render_submission=i*10+1,
                last_gpu_simulation_submission=11 if stale_sim else i*10+2)
            rows.append({"script_frame":i,"vfx":stats,"active_after_restart":16})
        final=json.loads(json.dumps(rows[-1]))
        final["script_frame"]=4+32
        for key in ("active_effects","active_cpu_instances"):final["vfx"][key]["current"]=0
        rows.append(final)
        (folder/"runtime.json").write_text(json.dumps({"status":"PASS","rows":rows,"baseline":{key:0 for key in ALLOCATION+FAILURES}}))
        return budget

    def test_duplicate_long_frame_cannot_be_hidden_by_short_replacement(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            budget = self.fixture(folder, long_frame=True)
            original = (folder/'frames.csv').read_bytes()
            with (folder/'frames.csv').open(newline='') as stream:
                records = list(csv.DictReader(stream))
            replacement = dict(next(row for row in records if row['frame']=='3'), cpuWallFrameMs='0.1')
            with (folder/'frames.csv').open('a', newline='') as stream:
                csv.DictWriter(stream, fieldnames=list(replacement)).writerow(replacement)
            evidence = (folder/'frames.csv').read_bytes()
            self.assertTrue(evidence.startswith(original))
            with self.assertRaisesRegex(ValueError, 'Duplicate frame identity'):
                analyze(folder, budget)
            self.assertEqual((folder/'frames.csv').read_bytes(), evidence)

    def test_duplicate_gpu_timing_cannot_replace_expensive_completion(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            budget = self.fixture(folder)
            path = folder/'frames.csv.submissions.csv'
            with path.open(newline='') as stream:
                records = list(csv.DictReader(stream))
            expensive = next(row for row in records if row['cpuTimingFrameId']=='103')
            expensive['gpuFullGraphicsFrameSpanMs']='90'
            cheap = dict(expensive, gpuFullGraphicsFrameSpanMs='0.1')
            with path.open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(expensive))
                writer.writeheader(); writer.writerows(records+[cheap])
            evidence = path.read_bytes()
            with self.assertRaisesRegex(ValueError, 'Duplicate submission/CPU frame identity'):
                analyze(folder, budget)
            self.assertEqual(path.read_bytes(), evidence)

    def test_distinct_frames_cannot_share_cpu_timing_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            budget = self.fixture(folder)
            path = folder/'frames.csv'
            with path.open(newline='') as stream:
                records = list(csv.DictReader(stream))
            records[2]['cpuTimingFrameId']=records[1]['cpuTimingFrameId']
            with path.open('w', newline='') as stream:
                writer=csv.DictWriter(stream,fieldnames=list(records[0]))
                writer.writeheader(); writer.writerows(records)
            with self.assertRaisesRegex(ValueError, 'Duplicate CPU frame identity'):
                analyze(folder,budget)

    def test_invoke_write_failure_finishes_storage_and_restores_environment(self):
        for previous in (None, 'existing-parent'):
            with self.subTest(previous=previous), tempfile.TemporaryDirectory() as temporary:
                root=Path(temporary); calls=[]
                class RegisteredStorage:
                    def __init__(self, root): pass
                    def start(self, **kwargs):
                        calls.append(('start', kwargs['parent']))
                        return 'new-owned-record'
                    def finish(self, ident, state): calls.append(('finish', ident, state))
                    def auto(self): calls.append(('auto',))
                original_write = Path.write_text
                def fail_invoke(path, *args, **kwargs):
                    if path.name=='invoke.ps1':
                        self.assertEqual(os.environ.get('HE_STORAGE_PARENT'),'new-owned-record')
                        raise OSError('injected invoke write failure')
                    return original_write(path, *args, **kwargs)
                with patch.dict(os.environ):
                    if previous is None: os.environ.pop('HE_STORAGE_PARENT',None)
                    else: os.environ['HE_STORAGE_PARENT']=previous
                    with patch('benchmark.ROOT',root), patch('benchmark.plan',return_value={'configuration':'Release'}), \
                            patch('benchmark.shutil.which',return_value=None), patch('benchmark.verify_candidate'), \
                            patch.dict(sys.modules,{'storage':types.SimpleNamespace(Storage=RegisteredStorage)}), \
                            patch.object(Path,'write_text',fail_invoke), patch('benchmark.subprocess.Popen') as process, \
                            patch('builtins.print'):
                        self.assertEqual(run('DX12'),1)
                        process.assert_not_called()
                    self.assertEqual(os.environ.get('HE_STORAGE_PARENT'),previous)
                self.assertIn(('start',previous or ''),calls)
                self.assertIn(('finish','new-owned-record','failed'),calls)
                results=list(root.glob('out/agent/rendering-evidence/vfx-benchmark/*/result.json'))
                self.assertEqual(len(results),1)
                report=json.loads(results[0].read_text())
                self.assertEqual(report['status'],'FAIL')
                self.assertEqual(report['executionStatus'],'not_executed')
                self.assertIn('injected invoke write failure',report['error'])

    def test_fresh_simulation_submissions_cannot_replace_missing_frame(self):
        self._assert_fresh_gpu_duplicate_frame_rejected('gpu_simulation_ms',
            'last_gpu_simulation_submission','gpuSimulationP95Ms')

    def test_fresh_render_submissions_cannot_replace_missing_frame(self):
        self._assert_fresh_gpu_duplicate_frame_rejected('gpu_render_ms',
            'last_gpu_render_submission','gpuVfxRenderP95Ms')

    def _assert_fresh_gpu_duplicate_frame_rejected(self, source, identity, metric):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary); budget=self.fixture(folder)
            path=folder/'runtime.json'; runtime=json.loads(path.read_text())
            # 21/22 and 23 are distinct completed submissions, but all belong
            # to frame 2. No sample remains for frame 3. Counts stay at three.
            replacement=runtime['rows'][2]['vfx']
            replacement[identity]=23
            replacement[source]['current']=budget['normal'][metric]*0.9
            path.write_text(json.dumps(runtime))
            result=analyze(folder,budget)
            self.assertEqual(result['status'],'FAIL')
            self.assertEqual(result['metrics'][metric]['samples'],3)
            self.assertEqual(result['metrics'][metric]['status'],'FAIL')
            coverage=result['gpu_frame_coverage'][metric]
            self.assertEqual(coverage['missingFrames'],[3])
            self.assertEqual(coverage['duplicateFrames'],[2])
            observed=[r for r in result['gpu_samples'] if r['metric']==source]
            self.assertEqual(len(observed),3)
            self.assertEqual(sum(r['frame']==2 for r in observed),2)
            self.assertTrue(any(r['submission']==23 and
                r['milliseconds']==budget['normal'][metric]*0.9 for r in observed))

    def test_invoke_and_result_write_failure_still_cleans_up_without_fake_report(self):
        for previous in (None,'existing-parent'):
            with self.subTest(previous=previous), tempfile.TemporaryDirectory() as temporary:
                root=Path(temporary);calls=[]
                class RegisteredStorage:
                    def __init__(self,root): pass
                    def start(self,**kwargs): return 'owned-record'
                    def finish(self,ident,state): calls.append((ident,state))
                    def auto(self): pass
                original_write=Path.write_text
                def fail_writes(path,*args,**kwargs):
                    if path.name in ('invoke.ps1','result.json'):
                        raise OSError('injected '+path.name+' write failure')
                    return original_write(path,*args,**kwargs)
                with patch.dict(os.environ):
                    if previous is None: os.environ.pop('HE_STORAGE_PARENT',None)
                    else: os.environ['HE_STORAGE_PARENT']=previous
                    with patch('benchmark.ROOT',root), patch('benchmark.plan',return_value={'configuration':'Release'}), \
                            patch('benchmark.shutil.which',return_value=None), patch('benchmark.verify_candidate'), \
                            patch.dict(sys.modules,{'storage':types.SimpleNamespace(Storage=RegisteredStorage)}), \
                            patch.object(Path,'write_text',fail_writes), patch('benchmark.subprocess.Popen') as process, \
                            patch('builtins.print'):
                        with self.assertRaisesRegex(OSError,'result.json write failure'):
                            run('DX12')
                        process.assert_not_called()
                    self.assertEqual(os.environ.get('HE_STORAGE_PARENT'),previous)
                self.assertEqual(calls,[('owned-record','failed')])
                self.assertEqual(list(root.glob('out/agent/rendering-evidence/vfx-benchmark/*/result.json')),[])

    def test_completed_submission_join(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            result = analyze(folder,self.fixture(folder))
            self.assertEqual(result["status"],"PASS")
            self.assertEqual(result["metrics"]["totalGpuFrameP95Ms"]["samples"],3)

    def test_stale_simulation_not_counted_with_new_render(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            result = analyze(folder,self.fixture(folder,stale_sim=True))
            self.assertEqual(result["status"],"FAIL")
            self.assertEqual(result["metrics"]["gpuSimulationP95Ms"]["samples"],0)
            self.assertEqual(result["metrics"]["gpuVfxRenderP95Ms"]["samples"],3)

    def test_missing_gpu_and_long_frame_remain(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            result = analyze(folder,self.fixture(folder,missing_gpu=True,long_frame=True))
            self.assertEqual(result["status"],"FAIL")
            self.assertEqual(result["metrics"]["totalGpuFrameP95Ms"]["samples"],2)
            self.assertEqual(result["long_frames"][0]["frame"],3)
            self.assertIsNone(result["long_frames"][0]["gpu_ms"])

    def test_bad_numbers_and_retirement_fail(self):
        for corruption in ('negative','infinite','retirement','failure','missing'):
            with self.subTest(corruption=corruption), tempfile.TemporaryDirectory() as temp:
                folder=Path(temp);budget=self.fixture(folder)
                path=folder/'runtime.json';data=json.loads(path.read_text())
                if corruption=='negative':data['rows'][1]['vfx']['cpu_update_ms']['current']=-1
                if corruption=='infinite':data['rows'][1]['vfx']['gpu_render_ms']['current']=float('inf')
                if corruption=='retirement':data['rows'][-1]['vfx']['gpu_reserved_particles']=256
                if corruption=='failure':data['rows'][1]['vfx']['gpu_particle_block_failures']=1
                if corruption=='missing':data.pop('baseline')
                path.write_text(json.dumps(data))
                self.assertEqual(analyze(folder,budget)['status'],'FAIL')

    def test_invalid_csv_times_fail(self):
        for filename,column,value in [('frames.csv','cpuWallFrameMs','-1'),
                                      ('frames.csv.submissions.csv','gpuFullGraphicsFrameSpanMs','inf')]:
            with self.subTest(filename=filename), tempfile.TemporaryDirectory() as temp:
                folder=Path(temp);budget=self.fixture(folder)
                path=folder/filename
                with path.open(newline='') as stream:rows=list(csv.DictReader(stream))
                for row in rows:row[column]=value
                with path.open('w',newline='') as stream:
                    writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
                self.assertEqual(analyze(folder,budget)['status'],'FAIL')

    def test_candidate_preselected_and_changed(self):
        with tempfile.TemporaryDirectory() as temp:
            target=Path(temp)/'candidate.exe';target.write_bytes(b'first');pinned=sha(target)
            with patch('benchmark.expected_executable',return_value=target):
                verify_candidate('Release',pinned)
                with self.assertRaises(RuntimeError):verify_candidate('Release',None)
                target.write_bytes(b'changed')
                with self.assertRaises(RuntimeError):verify_candidate('Release',pinned)

    def test_locked_plan(self):
        request = plan("DX12")
        self.assertEqual((len(request["effects"]),request["warmupFrames"],request["sampleFrames"],request["rounds"]),(16,600,600,3))
        self.assertEqual(len(request["resourceHashes"]),16)
        self.assertEqual(request["configuration"],"Release")
        self.assertEqual(request["executionStatus"],"not_executed")
        self.assertIn("game-release",request["expectedExecutable"])
        diagnostic=plan("DX12","RelWithDebInfo")
        self.assertEqual(diagnostic["configuration"],"RelWithDebInfo")
        self.assertIn("game-relwithdebinfo",diagnostic["expectedExecutable"])
        self.assertEqual(request["budgetSha256"],diagnostic["budgetSha256"])

    def test_actual_identity_rejects_wrong_configuration_and_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/"Project").mkdir()
            (root/"Project/Project.heproject").write_text(json.dumps({"ExecutableName":"Fixture"}))
            with patch("benchmark.ROOT",root):
                target=expected_executable("Release")
                target.parent.mkdir(parents=True)
                target.write_bytes(b"fixture candidate")
                cache=target.parent.parent/"CMakeCache.txt"
                cache.write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
                identity=runtime_identity({"executable":str(target)},"Release","DX12")
                self.assertEqual(identity["configuration"],"Release")
                self.assertEqual(len(identity["exeSha256"]),64)
                with self.assertRaises(RuntimeError):
                    runtime_identity({"executable":str(target)},"RelWithDebInfo","DX12")
                cache.write_text("CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n")
                with self.assertRaises(RuntimeError):
                    runtime_identity({"executable":str(target)},"Release","DX12")

    def test_route_does_not_silently_downgrade(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            folder=root/"Tools/Agent"; folder.mkdir(parents=True)
            for name in ["HECtl.ps1","Build.ps1"]:
                (folder/name).write_text('[ValidateSet("Debug", "RelWithDebInfo")]\n[string]$Configuration')
            with patch("benchmark.ROOT",root):
                self.assertFalse(route_support("Release")["supported"])
                self.assertTrue(route_support("RelWithDebInfo")["supported"])

    def test_one_shots_replay_and_finally_stop(self):
        live = set()
        events = []
        def play(*args):
            handle = len(events)
            events.append(handle)
            live.add(handle)
            return handle
        fake = types.SimpleNamespace(VFX=types.SimpleNamespace(
            SetQualityTier=lambda tier:None, LoadEffect=lambda path:True,
            PlaySeeded=play, IsEffectPlaying=lambda handle:handle in live,
            StopEffect=lambda handle:live.discard(handle),
            GetStats=lambda:{"update_serial":1,"last_gpu_simulation_submission":1,"last_gpu_render_submission":1}))
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder/"request.json").write_text(json.dumps({"warmupFrames":1,"sampleFrames":3,"effects":["effect"]*16}))
            with patch.dict(sys.modules,{"HEngine":fake}), patch.dict(os.environ,{
                "HE_VFX_BENCHMARK_REQUEST":str(folder/"request.json"),"HE_VFX_BENCHMARK_REPORT":str(folder/"runtime.json")}):
                spec = importlib.util.spec_from_file_location("benchmark_scene",Path(__file__).resolve().parents[2]/"Project/Scripts/VFXAuthoringBenchmark.py")
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                scene = module.VFXAuthoringBenchmark()
                try:
                    scene.Start(None)
                    for _ in range(4):
                        live.clear()  # Every one-shot ends before the next update.
                        scene.Update(None)
                        self.assertEqual(len(live),16)
                    scene.Update(None)
                finally:
                    scene.Stop()
                self.assertFalse(live)
                report = json.loads((folder/"runtime.json").read_text())
                self.assertEqual(report["status"],"PASS")
                self.assertEqual(len([e for e in report["events"] if e["kind"]=="play"]),80)


if __name__ == "__main__":
    unittest.main()
