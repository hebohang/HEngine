"""Actual Game shared scene-service transactions; not Editor UI acceptance."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import uuid

from preview import ROOT, Session, Storage, digest, write_json

SOURCE = ROOT/'Project/Assets/Effects/Authoring/CharacterDemo'
FILES = ['Actor.hprefab','ActorA.hprefab','ActorB.hprefab','Actor.propagation-v2.hprefab']


class Transactions:
    def __init__(self,session,report):
        self.session,self.report,self.state=session,report,None

    def check(self,name,condition,**evidence):
        self.report['checks'].append(dict(name=name,status='PASS' if condition else 'FAIL',evidence=evidence))
        write_json(self.session.output/'report.json',self.report)
        if not condition:
            raise RuntimeError('Assertion failed: '+name)

    def call(self,action,request):
        result=self.session.call('scene.'+action,request)
        if result.get('ok') is not True:
            raise RuntimeError('scene.'+action+' failed: '+json.dumps(result))
        return result

    def write(self,action):
        self.state=self.call(action,dict(sceneId=self.state['sceneId'],baseRevision=self.state['revision'],
            requestId=uuid.uuid4().hex))
        return self.state

    def apply(self,operations,label):
        request=dict(sceneId=self.state['sceneId'],baseRevision=self.state['revision'],
            requestId=uuid.uuid4().hex,operations=operations)
        planned=self.call('plan',request)
        self.check(label+'.plan_readonly',planned['revision']==self.state['revision'] and
            planned.get('completion')=='planned' and planned.get('sceneId')==self.state['sceneId'],receipt=planned)
        self.state=self.call('apply',request)
        retry=self.call('apply',request)
        self.check(label+'.idempotent',retry['transactionId']==self.state['transactionId'] and
            retry['revision']==self.state['revision'],receipt=self.state,retry=retry)

    def query(self):
        result=self.call('query',dict(sceneId=self.state['sceneId'],limit=128,
            fields=['uuid','name','parent','TransformComponent','AnimatorComponent']))
        if result.get('nextOffset') is not None:
            raise RuntimeError('Fixture query unexpectedly needs pagination')
        return {row['uuid']:row for row in result['entities']}

    def inspect(self,rows,synced,label,roots=None):
        def unique(name,root=None):
            found=[row for row in rows.values() if row['name']==name and
                (root is None or ancestor(row['uuid'],root,rows))]
            if len(found)!=1:
                raise RuntimeError('Expected one '+name+' under '+str(root))
            return found[0]
        a,b=unique('CharacterInstanceA'),unique('CharacterInstanceB')
        actual_roots=[a['uuid'],b['uuid']]
        self.check(label+'.distinct_roots',a['uuid']!=b['uuid'] and
            (roots is None or roots==actual_roots),roots=actual_roots)
        hand_ids=[]
        for index,(root,actor,speed) in enumerate([(a,'CharacterActorA',1),(b,'CharacterActorB',.65)]):
            root_id=root['uuid']
            unique(actor,root_id)
            hand=unique('CharacterHand',root_id); hand_ids.append(hand['uuid'])
            visual=unique('CharacterVisual',root_id)
            badges=[row for row in rows.values() if row['name']=='TemplateAddedBadge' and ancestor(row['uuid'],root_id,rows)]
            expected_y=1.4 if synced and index==1 else 1.2
            translation=hand['TransformComponent']['Translation']
            self.check(label+'.instance_'+str(index),math.isclose(translation[1],expected_y,rel_tol=1e-5)
                and math.isclose(visual['AnimatorComponent']['playSpeed'],speed,rel_tol=1e-5)
                and len(badges)==(1 if synced else 0),hand=hand,visual=visual,badges=badges)
        for ident,row in rows.items():
            if row['parent']:
                self.check(label+'.parent_'+ident,row['parent'] in rows and
                    sum(ancestor(ident,root,rows) for root in actual_roots)==1,
                    uuid=ident,parent=row['parent'])
        self.check(label+'.no_extra_roots',all(row['parent'] or row['uuid'] in actual_roots for row in rows.values()))
        return actual_roots,hand_ids


def ancestor(ident,root,rows):
    seen=set()
    while ident in rows and ident not in seen:
        if ident==root: return True
        seen.add(ident); ident=rows[ident]['parent']
    return False


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],default='DX12')
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    executable=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    pinned=digest(executable)
    if pinned.lower()!=args.existing_build_sha256.lower():
        parser.error('Candidate hash mismatch; no session started')
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    ident=uuid.uuid4().hex
    scratch=ROOT/'Project/Assets/PrefabAcceptance'/ident
    scratch.mkdir(parents=True,exist_ok=False)
    scene_relative='Saved/PrefabAcceptance-'+ident+'.he'
    scene=ROOT/'Project'/scene_relative; scene.parent.mkdir(parents=True,exist_ok=True)
    scene.write_text('{}',encoding='utf-8')
    source_hashes={name:digest(SOURCE/name) for name in FILES}
    copies={}
    for name in FILES:
        document=json.loads((SOURCE/name).read_text())
        for nested in document.get('nested',[]):
            original=Path(nested['path']).name
            if original not in FILES: raise ValueError('Unexpected nested template dependency')
            nested['path']=(scratch/original).relative_to(ROOT/'Project').as_posix()
        write_json(scratch/name,document); copies[name]=document
    write_json(output/'scratch-inputs.json',copies)
    session=Session(output,args.backend,pinned)
    report=dict(status='RUNNING',scope='Game Dev Agent, shared SceneEditingService; NOT Editor UI/runtime verification',
        executableSha256=pinned,backend=args.backend,sourceHashes=source_hashes,checks=[],
        scene=scene_relative,scratch=str(scratch),manualSceneUuidAssignments=False)
    transactions=Transactions(session,report)
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'prefab-transactions',[output]) as run:
        try:
            session.start()
            report['session']=session.call('session.status')
            transactions.state=transactions.call('open',dict(path=scene_relative))
            operations=[dict(op='prefab',path=(scratch/name).relative_to(ROOT/'Project').as_posix(),temporaryId=temp)
                for name,temp in [('ActorA.hprefab','$a'),('ActorB.hprefab','$b')]]
            transactions.apply(operations,'create')
            original=transactions.query()
            roots,hands=transactions.inspect(original,False,'created')
            transactions.apply([dict(op='prefab_override',entity=hands[0],
                pointer='/TransformComponent/Translation',mode='set')],'explicit_override')
            # Only the uniquely copied shared template changes. The service
            # allocates all UUIDs and maintains nested instance metadata.
            write_json(scratch/'Actor.hprefab',copies['Actor.propagation-v2.hprefab'])
            transactions.apply([dict(op='prefab_sync',entity=root) for root in roots],'sync')
            synced=transactions.query()
            transactions.inspect(synced,True,'synced',roots)
            transactions.check('existing_uuids_preserved',set(original)<=set(synced))
            transactions.write('undo')
            undone=transactions.query()
            transactions.inspect(undone,False,'undo',roots)
            transactions.check('undo_restores_entity_set',set(original)==set(undone))
            transactions.write('redo')
            redone=transactions.query()
            transactions.inspect(redone,True,'redo',roots)
            transactions.check('redo_uuids_and_fields',synced==redone)
            transactions.write('save')
            transactions.check('save_clean',transactions.state.get('dirty') is False)
            saved=json.loads(scene.read_text())
            write_json(output/'saved-scene.he',saved)
            metadata=saved[hands[0]]['PrefabInstanceComponent']
            overrides=json.loads(metadata['OverridePathsJson'])
            transactions.check('serialized_explicit_override','/TransformComponent/Translation' in overrides,
                metadata=metadata)
            for entity_id,components in saved.items():
                meta=components['PrefabInstanceComponent']
                transactions.check('serialized_parent_root_'+entity_id,
                    meta['InstanceRootUuid'] in roots and ancestor(entity_id,meta['InstanceRootUuid'],synced)
                    and components['TransformComponent']['ParentUuid']==synced[entity_id]['parent'],
                    uuid=entity_id,localId=meta['LocalId'],instanceRoot=meta['InstanceRootUuid'])
            transactions.state=transactions.call('open',dict(path=scene_relative))
            reopened=transactions.query()
            transactions.inspect(reopened,True,'reopened',roots)
            transactions.check('reopen_uuid_parent_fields_preserved',reopened==synced)
            transactions.write('save')
            reserialized=json.loads(scene.read_text())
            transactions.check('reopen_override_metadata_preserved',
                json.loads(reserialized[hands[0]]['PrefabInstanceComponent']['OverridePathsJson'])==overrides)
            transactions.call('validate',dict(sceneId=transactions.state['sceneId']))
            report['status']='PASS'; run['status']='success'
        except Exception as error:
            report['status']='FAIL'; report['error']=str(error); run['status']='failed'
        finally:
            try: session.stop()
            except Exception as error:
                report['status']='FAIL'; report['cleanupError']=str(error); run['status']='unknown'
            report['sourcesUnchanged']=all(digest(SOURCE/name)==value for name,value in source_hashes.items())
            report['binaryUnchanged']=digest(executable)==pinned
            if not report['sourcesUnchanged'] or not report['binaryUnchanged']:
                report['status']='FAIL'
                if run['status']!='unknown': run['status']='failed'
            if not report.get('cleanupError'):
                for name in FILES: (scratch/name).unlink(missing_ok=True)
                scratch.rmdir(); scene.unlink(missing_ok=True)
            write_json(output/'report.json',report)
    print(json.dumps(dict(status=report['status'],report=str(output/'report.json'))))
    return 0 if report['status']=='PASS' else 1


if __name__=='__main__':
    raise SystemExit(main())
