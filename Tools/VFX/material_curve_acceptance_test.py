"""AST-extract pure package builder; no image/runtime dependencies or GPU."""
import ast,io,zipfile,tempfile,unittest
from pathlib import Path
source=Path(__file__).with_name('material_curve_acceptance.py')
tree=ast.parse(source.read_text());node=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='compiled_package')
namespace=dict(io=io,zipfile=zipfile);exec(compile(ast.Module(body=[node],type_ignores=[]),str(source),'exec'),namespace)
class CachePackage(unittest.TestCase):
 def test_compiled_only_preserves_effect_and_replaces_exact_material_path(self):
  with tempfile.TemporaryDirectory() as temp:
   path=Path(temp)/'source.zip'
   with zipfile.ZipFile(path,'w') as archive:
    archive.writestr('effect.efkefc',b'original runtime');archive.writestr('Material/Cyan.efkmat',b'source');archive.writestr('Texture/a.png',b'texture')
   result=namespace['compiled_package'](path,b'official cache')
   with zipfile.ZipFile(io.BytesIO(result)) as archive:
    self.assertNotIn('Material/Cyan.efkmat',archive.namelist())
    self.assertEqual(archive.read('Material/Cyan.efkmatd'),b'official cache')
    self.assertEqual(archive.read('effect.efkefc'),b'original runtime')
    self.assertEqual(archive.read('Texture/a.png'),b'texture')
 def test_missing_or_ambiguous_material_rejected(self):
  with tempfile.TemporaryDirectory() as temp:
   for count in [0,2]:
    path=Path(temp)/str(count)
    with zipfile.ZipFile(path,'w') as archive:
     for i in range(count):archive.writestr(str(i)+'.efkmat',b'source')
    with self.assertRaises(ValueError):namespace['compiled_package'](path,b'cache')
 def test_probe_backend_routes_and_cleanup(self):
  node=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='compiled_probe')
  ns=dict(namespace);ns.update(pixels=lambda p:p,difference=lambda a,b:dict(changedPixels=0))
  exec(compile(ast.Module(body=[node],type_ignores=[]),str(source),'exec'),ns)
  for backend in ['DX12','Vulkan']:
   with tempfile.TemporaryDirectory() as temp:
    root=Path(temp);project=root/'Project';project.mkdir();fixture=project/'source.zip'
    with zipfile.ZipFile(fixture,'w') as archive:archive.writestr('Cyan.efkmat',b'source');archive.writestr('effect.efkefc',b'original')
    ns['ROOT']=root
    class Checks:
     session=type('Session',(),dict(output=project))()
     def __init__(self):self.handles=set();self.counter=0;self.rejections=[];self.report=dict(checks=[],visualEvidence=[],compiledCoverage={})
     def play(self,path):self.counter+=1;self.handles.add(self.counter);return self.counter
     def stop(self,h):self.handles.remove(h)
     def state(self,h):return dict(playing=h in self.handles,paused=True)
     def call(self,*a,**kw):return dict(drawCalls=dict(current=1))
     def control(self,*a,**kw):return {}
     def capture(self,label):return dict(path=project/(label+'.png'))
     def record(self,name,condition,**kw):self.report['checks'].append(dict(name=name,status='PASS' if condition else 'FAIL'))
     def rejected(self,name,*a,**kw):self.rejections.append(name);self.record(name,True)
    checks=Checks();ns['compiled_probe'](checks,checks.report,fixture,project,[],b'cache',backend,project/'source.png')
    self.assertFalse(checks.handles)
    self.assertEqual(checks.report['compiledCoverage']['status'],'PASS')
    self.assertEqual(len(checks.rejections),2 if backend=='Vulkan' else 0)

if __name__=='__main__':unittest.main()
