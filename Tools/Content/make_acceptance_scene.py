"""Blender --background --factory-startup --python-exit-code 7 --python this.py -- output.blend"""
from pathlib import Path
import sys
import math
import bpy
from mathutils import Vector


def make(path):
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1
    scene.render.resolution_x, scene.render.resolution_y = 1280, 720
    gray = bpy.data.materials.new("Concrete")
    gray.diffuse_color = (0.45, 0.5, 0.55, 1)
    gray.use_nodes = True
    gray.node_tree.nodes.get("Principled BSDF").inputs["Base Color"].default_value = (0.45, 0.5, 0.55, 1)
    red = gray.copy()
    red.name = "Terracotta"
    red.node_tree.nodes.get("Principled BSDF").inputs["Base Color"].default_value = (0.65, 0.07, 0.03, 1)
    image = bpy.data.images.new("Packed acceptance checker", width=2, height=2, alpha=True)
    image.pixels = [0.65, .07, .03, 1, .18, .025, .01, 1,
                    .18, .025, .01, 1, .65, .07, .03, 1]
    image.pack()
    texture = red.node_tree.nodes.new("ShaderNodeTexImage")
    texture.image = image
    red.node_tree.links.new(texture.outputs["Color"], red.node_tree.nodes.get("Principled BSDF").inputs["Base Color"])
    bpy.ops.mesh.primitive_cube_add(size=1)
    template = bpy.context.object
    geometry = template.data
    geometry.name = "SharedUnitCube"
    geometry.materials.append(gray)
    bpy.data.objects.remove(template, do_unlink=True)

    def empty(name, location, scale=(1, 1, 1), collision=None, dynamic=False):
        obj = bpy.data.objects.new(name, None)
        scene.collection.objects.link(obj)
        obj.location, obj.scale = location, scale
        if collision:
            obj["he_proxy"], obj["he_collision"] = True, collision
            obj["he_motion"] = "dynamic" if dynamic else "static"
        return obj

    def cube(name, location, scale, material=None, collide=False):
        obj = bpy.data.objects.new(name, geometry)
        scene.collection.objects.link(obj)
        obj.location, obj.scale = location, scale
        if material:
            obj.material_slots[0].link = "OBJECT"
            obj.material_slots[0].material = material
        if collide:
            empty(name + " Collision", location, scale, "box")
        return obj

    cube("Floor", (0, 0, -0.25), (14, 14, 0.5), collide=True)
    cube("Door Wall Left", (-3.5, -4, 1.5), (5, 0.3, 3), collide=True)
    cube("Door Wall Right", (3.5, -4, 1.5), (5, 0.3, 3), collide=True)
    cube("Door Lintel", (0, -4, 2.6), (2, 0.3, 0.8), collide=True)
    cube("Back Wall", (0, 5, 1.5), (12, 0.3, 3), collide=True)
    cube("Side Wall", (-6, 0.5, 1.5), (0.3, 9, 3), collide=True)
    for i in range(4):
        height = (i + 1) * 0.25
        cube("Step " + str(i), (3, 0.8 + i * 0.6, height / 2), (2, 0.6, height), collide=True)
    group = empty("Crates", (-2, 1, 0))
    for i in range(3):
        obj = cube("Shared Crate " + str(i), (i * 1.25, 0, 0.5), (0.8, 0.8, 1), red if i == 1 else None)
        obj.parent = group
    cube("Mirrored Display", (-3.5, 2.5, 1), (-1, 0.6, 2), red)
    drop = empty("Test Drop", (0, 0, 4), (0.6, 0.6, 0.6), "box", True)
    visual = cube("Drop Visual", (0, 0, 0), (1, 1, 1), red)
    visual.parent = drop
    empty("Test Door Walker", (0, -5.5, 0.91))["he_character"] = True
    empty("Test Stair Walker", (3, -0.5, 0.91))["he_character"] = True
    # Join exactly the three wall pieces into a genuinely concave doorway mesh.
    # Static triangle cooking must retain the opening instead of making a hull.
    bpy.ops.object.select_all(action="DESELECT")
    for name in ["Door Wall Left", "Door Wall Right", "Door Lintel"]:
        bpy.data.objects[name].data = bpy.data.objects[name].data.copy()
        bpy.data.objects[name].select_set(True)
        bpy.data.objects.remove(bpy.data.objects[name + " Collision"], do_unlink=True)
    bpy.context.view_layer.objects.active = bpy.data.objects["Door Lintel"]
    bpy.ops.object.join()
    frame = bpy.context.object
    frame.name = "Concave Door Frame"
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    frame["he_collision"] = "mesh"
    camera_data = bpy.data.cameras.new("Acceptance Camera")
    camera = bpy.data.objects.new("Acceptance Camera", camera_data)
    scene.collection.objects.link(camera)
    camera.location = (11, -15, 11)
    camera.rotation_euler = (Vector((0, 0, 1)) - camera.location).to_track_quat("-Z", "Y").to_euler()
    camera.data.lens = 42
    camera["he_primary"] = True
    camera["he_exposure_ev100"] = 10.0
    scene.camera = camera
    sun_data = bpy.data.lights.new("Sun", "SUN")
    sun_data.energy = 3
    sun = bpy.data.objects.new("Sun", sun_data)
    scene.collection.objects.link(sun)
    sun.rotation_euler = (math.radians(25), math.radians(-20), math.radians(-35))
    light_data = bpy.data.lights.new("Point", "POINT")
    light_data.energy = 800
    light = bpy.data.objects.new("Point", light_data)
    scene.collection.objects.link(light)
    light.location = (-3, -1, 4)
    light["he_light_range"] = 12
    path = Path(path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(path))


if __name__ == "__main__":
    make(sys.argv[sys.argv.index("--") + 1])
