import csv
import json
from pathlib import Path
import re
import struct
import tempfile
import unittest
import tracemalloc

from verify_render_stats_stream import validate


class StreamEvidenceTests(unittest.TestCase):
    def fixture(self, folder):
        source=(Path(__file__).resolve().parents[2]/'Engine/Source/Runtime/Core/AppFramework/Application.cpp').read_text()
        start=source.index('csv << "frame,cpuWallFrameMs'); end=source.index(';',start)
        header=''.join(json.loads(v) for v in re.findall(r'"(?:[^"\\]|\\.)*"',source[start:end])).strip().split(',')
        path=folder/'frames.csv'
        values=[.123456123,17.456789321,35.123456789]
        with path.open('w',newline='') as stream:
            writer=csv.DictWriter(stream,fieldnames=header);writer.writeheader()
            for i,wall in enumerate(values):
                row={key:0 for key in header}
                row.update(frame=i+31,cpuWallFrameMs=f'{wall:.6f}',cpuTimingFrameId=i+41,
                           submissionTimingSampleId=i+51,cpuIntervalValid=int(i<2),gpuSample=0)
                writer.writerow(row)
        Path(str(path)+'.wall-ms.bin').write_bytes(struct.pack('<3d',*values))
        Path(str(path)+'.submissions.csv').write_text('submissionTimingSampleId,cpuTimingFrameId,status\n51,41,0\n52,42,0\n53,43,0\n54,44,0\n')
        Path(str(path)+'.passes.csv').write_text('gpuSample,passName,milliseconds,graphSubmissionId\n')
        Path(str(path)+'.memory.csv').write_text('frame,elapsedSeconds\n31,0\n')
        return path,values

    def test_full_rows_tail_retirement_and_exact_percentile(self):
        with tempfile.TemporaryDirectory() as folder:
            path,values=self.fixture(Path(folder))
            result=validate(path,3,True,True)
            self.assertEqual(result['frames'],3)
            self.assertEqual(result['submissions'],4) # Unmeasured retirement tail preserved.
            self.assertAlmostEqual(result['p95Ms'],values[1]+(values[2]-values[1])*.9,places=12)

    def test_missing_retirement_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path,_=self.fixture(Path(folder))
            Path(str(path)+'.submissions.csv').write_text('submissionTimingSampleId,cpuTimingFrameId,status\n51,41,0\n52,42,0\n')
            with self.assertRaisesRegex(ValueError,'Missing submission'):validate(path,3)

    def test_incomplete_spool_and_row_count_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path,_=self.fixture(Path(folder))
            with self.assertRaisesRegex(ValueError,'row count'):validate(path,4)
            Path(str(path)+'.wall-ms.bin').write_bytes(b'')
            with self.assertRaisesRegex(ValueError,'spool row count'):validate(path,3)

    def test_interior_cpu_sample_cannot_be_written_before_completion(self):
        with tempfile.TemporaryDirectory() as folder:
            path,_=self.fixture(Path(folder))
            with path.open(newline='') as stream:
                reader=csv.DictReader(stream);fields=reader.fieldnames;data=list(reader)
            data[0]['cpuIntervalValid']='0'
            with path.open('w',newline='') as stream:
                writer=csv.DictWriter(stream,fieldnames=fields);writer.writeheader();writer.writerows(data)
            with self.assertRaisesRegex(ValueError,'Non-tail CPU'):validate(path,3)

    def test_large_sidecars_stream_and_validate_the_final_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            path,_=self.fixture(Path(folder))
            with path.open(newline='') as stream:
                reader=csv.DictReader(stream);fields=reader.fieldnames;data=list(reader)
            data[0].update(gpuSample='7',graphSubmissionId='81')
            with path.open('w',newline='') as stream:
                writer=csv.DictWriter(stream,fieldnames=fields);writer.writeheader();writer.writerows(data)
            passes=Path(str(path)+'.passes.csv')
            count=20000
            with passes.open('w') as stream:
                stream.write('gpuSample,passName,milliseconds,graphSubmissionId\n')
                for _ in range(count): stream.write('7,pass,0.125,81\n')
            with Path(str(path)+'.memory.csv').open('w') as stream:
                stream.write('frame,elapsedSeconds\n')
                for i in range(count): stream.write(f'{i},0\n')
            tracemalloc.start()
            try:
                result=validate(path,3,True,True)
                peak=tracemalloc.get_traced_memory()[1]
            finally:
                tracemalloc.stop()
            self.assertEqual((result['passes'],result['memory']),(count,count))
            self.assertLess(peak,2_000_000, 'Raw sidecar rows must not accumulate')
            with passes.open('a') as stream: stream.write('8,bad,0.125,81\n')
            with self.assertRaisesRegex(ValueError,'GPU pass row'): validate(path,3)

    def test_duplicate_unmeasured_retirement_and_failed_measured_status(self):
        with tempfile.TemporaryDirectory() as folder:
            path,_=self.fixture(Path(folder))
            submissions=Path(str(path)+'.submissions.csv')
            original=submissions.read_text()
            submissions.write_text(original+'54,44,0\n')
            with self.assertRaisesRegex(ValueError,'Duplicate submission'): validate(path,3)
            submissions.write_text(original.replace('53,43,0','53,43,2'))
            with self.assertRaisesRegex(ValueError,'did not complete'): validate(path,3,True)
            self.assertEqual(validate(path,3,False)['status'],'PASS')


if __name__=='__main__':unittest.main()
