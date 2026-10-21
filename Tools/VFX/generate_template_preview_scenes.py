"""Deterministic fixed-camera scenes for complete template envelopes."""
import json
import math
from pathlib import Path
import uuid

ROOT = Path(__file__).resolve().parents[2]


def build():
    outputs = []
    for source_name, target_name, position in (
        ('VFXAuthoringNeutral.he', 'VFXAuthoringTemplateNeutral.he', [0, 6, -10]),
        ('VFXAuthoringGameBackground.he', 'VFXAuthoringTemplateGame.he', [10, 6.02, 0]),
    ):
        directory = ROOT/'Project/Assets/Scenes'
        source = json.loads((directory/source_name).read_text(encoding='utf-8'))
        namespace = uuid.uuid5(uuid.NAMESPACE_URL, 'HEngine/VFX/template-preview/'+target_name)
        ids = {key: str(uuid.uuid5(namespace, key)) for key in source}
        def remap(value):
            if isinstance(value, dict):
                return {ids.get(key, key): remap(child) for key, child in value.items()}
            if isinstance(value, list):
                return [remap(child) for child in value]
            return ids.get(value, value) if isinstance(value, str) else value
        document = remap(source)
        cameras = [entity for entity in document.values() if entity.get('CameraComponent', {}).get('Primary')]
        if len(cameras) != 1:
            raise ValueError('Expected one fixed primary camera')
        camera = cameras[0]
        camera['TagComponent']['Tag'] = 'VFX Template Full Envelope Camera'
        camera['TransformComponent'].update(Translation=position, Rotation=[math.pi-math.atan(.55), 0, math.pi])
        # Keep the same FOV, exposure, environment and actual game geometry.
        # Aim at relative Y=.5 from distance sqrt(10^2+5.5^2), leaving room for
        # the broad environment recipe's spawn radius plus lifetime travel.
        data = (json.dumps(document, indent=4)+'\n').encode('utf-8')
        destination = directory/target_name
        if not destination.exists() or destination.read_bytes() != data:
            destination.write_bytes(data)
        outputs.append(str(destination.relative_to(ROOT)))
    return outputs


if __name__ == '__main__':
    print(json.dumps(build()))
