import copy
import importlib.util
import json
from pathlib import Path
import sys
import types
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'Project'))


class Vector:
    def __init__(self,*values):
        self.x,self.y,self.z = values[:3]
        self.w = values[3] if len(values) > 3 else 0


class Entity:
    serial = 0
    def __init__(self):
        Entity.serial += 1
        self.ident = str(Entity.serial)
        self.alive = True
        self.transform = types.SimpleNamespace(Translation=Vector(0,0,0))
        self.light = types.SimpleNamespace()
        self.sprite = types.SimpleNamespace(Color=Vector(1,1,1,1))
    def GetUUID(self):
        if not self.alive: raise ValueError("destroyed")
        return self.ident
    def GetWorldPosition(self): return self.transform.Translation
    def HasComponent(self,name): return self.alive
    def GetComponent(self,name):
        if not self.alive: raise ValueError("destroyed")
        return {"Transform":self.transform,"PointLight":self.light,"SpriteRenderer":self.sprite}[name]


class Handles:
    def __init__(self): self.live = set(); self.serial = 0
    def play(self,*args,**kwargs):
        self.serial += 1; self.live.add(self.serial); return self.serial
    def stop(self,value): self.live.discard(value)


class PresentationTests(unittest.TestCase):
    def setUp(self):
        self.vfx,self.audio = Handles(),Handles()
        self.pause_calls = []
        self.lights = []
        def instantiate(*args):
            entity=Entity(); self.lights.append(entity); return entity
        engine=types.ModuleType("HEngine")
        engine.vec3=engine.vec4=Vector
        engine.VFX=types.SimpleNamespace(LoadEffect=lambda path:True, PlaySeeded=self.vfx.play,
            IsEffectPlaying=lambda handle:handle in self.vfx.live, StopEffect=self.vfx.stop,
            SetColor=lambda *args:True,SetEffectTransform=lambda *args:True,
            SetPaused=lambda handle,value:self.pause_calls.append(('vfx',value)))
        engine.Audio=types.SimpleNamespace(Play3D=self.audio.play,Stop=self.audio.stop,
            SetPosition=lambda *args:True,GetLastError=lambda:'audio unavailable',
            Pause=lambda handle:self.pause_calls.append(('audio',True)),
            Resume=lambda handle:self.pause_calls.append(('audio',False)))
        engine.Instantiate=instantiate
        engine.Destroy=lambda entity:setattr(entity,'alive',False)
        engine.GetFullPath=lambda path:str(ROOT/'Project'/path)
        sys.modules['HEngine']=engine
        spec=importlib.util.spec_from_file_location('presentation_tested',ROOT/'Project/Scripts/PresentationPlayer.py')
        self.module=importlib.util.module_from_spec(spec); spec.loader.exec_module(self.module)
        self.service=self.module.PresentationService()
        self.resource=json.loads((ROOT/'Project/Assets/Effects/Authoring/Compositions/impact.json').read_text())
        self.path='Assets/Effects/Authoring/Compositions/impact.json'
        self.target,self.camera=Entity(),Entity()

    def test_timeline_ends_and_restores_shake(self):
        self.assertEqual(self.service.play(self.path,'one',self.target,self.camera),'playing')
        self.service.update(.05)
        self.assertNotEqual(self.camera.transform.Translation.x,0)
        self.service.update(.8)
        self.assertEqual(self.service.status('one')['status'],'completed')
        self.assertFalse(self.vfx.live or self.audio.live or any(e.alive for e in self.lights))
        self.assertAlmostEqual(self.camera.transform.Translation.x,0)
        self.assertAlmostEqual(self.camera.transform.Translation.y,0)

    def test_idempotency_cancel_and_target_destruction(self):
        self.service.play(self.path,'one',self.target,self.camera)
        self.service.play(self.path,'one',self.target,self.camera)
        self.assertEqual(self.vfx.serial,1)
        with self.assertRaises(self.module.PresentationError):
            self.service.play(self.path,'one',self.target,self.camera,{'scale':2})
        self.target.alive=False
        self.service.update(.1)
        self.assertEqual(self.service.status('one')['status'],'cancelled')
        self.assertFalse(self.vfx.live or self.audio.live or any(e.alive for e in self.lights))
        self.assertEqual(self.service.cancel('one'),'cancelled')

    def test_audio_failure_rolls_back_started_particles(self):
        sys.modules['HEngine'].Audio.Play3D=lambda *args,**kwargs:0
        self.assertEqual(self.service.play(self.path,'one',self.target,self.camera),'failed')
        self.assertFalse(self.vfx.live)

    def test_bad_reload_preserves_previous_resource(self):
        old=self.service.load(self.path)
        with self.assertRaises((OSError,self.module.PresentationError)):
            self.service.load('Assets/missing.json')
        self.assertIs(self.service.resources[self.path],old)
        invalid=copy.deepcopy(old); invalid['tracks'][0]['parameters']['radius']=2
        with self.assertRaises(self.module.PresentationError): self.module.validate(invalid)

    def test_sprite_tint_adapter_restores_original(self):
        resource=copy.deepcopy(self.resource)
        resource['tracks']=[{'id':'color','kind':'sprite_color','at':0,'duration':1,'parameters':{'color':[1,0,0,1]}}]
        self.service.resources[self.path]=resource
        self.service.play(self.path,'one',self.target)
        self.assertEqual(self.target.sprite.Color.y,0)
        self.service.cancel('one')
        self.assertEqual(self.target.sprite.Color.y,1)

    def test_pause_propagates_to_vfx_audio_and_timeline(self):
        self.service.play(self.path,'one',self.target,self.camera)
        self.service.paused = True
        self.service.update(.5)
        self.assertEqual(self.service.status('one')['time'],0)
        self.assertIn(('vfx',True),self.pause_calls)
        self.assertIn(('audio',True),self.pause_calls)
        self.service.paused = False
        self.assertIn(('audio',False),self.pause_calls)
        self.service.stop()


if __name__ == '__main__': unittest.main()
