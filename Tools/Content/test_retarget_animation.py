"""Small contract checks for offline, bind-pose-aware animation retargeting."""

import math
import unittest

import retarget_animation as r


def joint(name, index, parent, rotation=r.IDENTITY):
    return {"name": name, "index": index, "parentIndex": parent,
            "localBindT": [0, 0, 0], "localBindR": rotation,
            "localBindS": [1, 1, 1]}


def node(name, rotation, children=()):
    return {"name": name, "localTransform": r.pose_matrix({
        "localBindR": rotation, "localBindT": [0, 0, 0],
        "localBindS": [1, 1, 1]}), "children": list(children)}


class RetargetAnimationTests(unittest.TestCase):
    def setUp(self):
        quarter = (math.cos(math.pi/4), 0, 0, math.sin(math.pi/4))
        self.source = {"name": "Aim", "rootNode": node("Root", r.IDENTITY, [
            node("mixamorig:Hips", r.IDENTITY, [node("mixamorig:Arm", quarter)])]),
            "clips": [{"name": "Aim", "duration": 1.0, "ticksPerSecond": 30,
                       "channels": [{"name": "mixamorig:Arm", "positionKeys": [],
                                     "scaleKeys": [], "rotationKeys": [
                                         {"qw": quarter[0], "qx": 0, "qy": 0,
                                          "qz": quarter[3], "timeStamp": 0},
                                         {"qw": 1, "qx": 0, "qy": 0, "qz": 0,
                                          "timeStamp": 1}]}]}]}
        self.skeleton = {"bones": [joint("root", 0, -1, quarter),
                                    joint("pelvis", 1, 0), joint("arm", 2, 1)],
                         "inverseBindMatrices": [[1, 0, 0, 0]*4 for _ in range(3)]}
        self.config = {"targetSkeletonPath": "Assets/target.hskel",
                       "boneMap": {"Hips": "pelvis", "Arm": "arm"}}

    def test_retarget_preserves_target_bind_and_applies_source_world_delta(self):
        output = r.retarget(self.source, self.skeleton, self.config)
        self.assertEqual(len(output["boneInfos"]), 3)
        self.assertEqual(output["rootNode"]["children"][0]["name"], "pelvis")
        channels = {c["name"]: c for c in output["clips"][0]["channels"]}
        self.assertEqual(set(channels), {"pelvis", "arm"})
        for target, expected in (("pelvis", r.IDENTITY), ("arm", r.IDENTITY)):
            key = channels[target]["rotationKeys"][0]
            actual = tuple(key[f] for f in ("qw", "qx", "qy", "qz"))
            self.assertAlmostEqual(abs(sum(a*b for a, b in zip(actual, expected))), 1, places=6)
        # The source arm rotates back to identity; the target arm's final world
        # rotation is identity despite the target root's 90-degree bind rotation.
        arm_end = channels["arm"]["rotationKeys"][-1]
        arm_local = tuple(arm_end[f] for f in ("qw", "qx", "qy", "qz"))
        world = r.mul(self.skeleton["bones"][0]["localBindR"], arm_local)
        self.assertAlmostEqual(abs(world[0]), 1, places=6)

    def test_rejects_missing_or_ambiguous_source_mapping(self):
        self.config["boneMap"]["Absent"] = "root"
        with self.assertRaisesRegex(ValueError, "missing or ambiguous"):
            r.retarget(self.source, self.skeleton, self.config)
        del self.config["boneMap"]["Absent"]
        self.source["rootNode"]["children"].append(node("other:Arm", r.IDENTITY))
        with self.assertRaisesRegex(ValueError, "missing or ambiguous"):
            r.retarget(self.source, self.skeleton, self.config)

    def test_absolute_rotation_normalizes_a_clip_already_in_target_space(self):
        self.config["rotationMode"] = "absolute"
        output = r.retarget(self.source, self.skeleton, self.config)
        pelvis = next(c for c in output["clips"][0]["channels"] if c["name"] == "pelvis")
        key = pelvis["rotationKeys"][0]
        local = tuple(key[f] for f in ("qw", "qx", "qy", "qz"))
        world = r.mul(self.skeleton["bones"][0]["localBindR"], local)
        self.assertAlmostEqual(abs(world[0]), 1, places=6)

    def test_hip_motion_includes_animated_fbx_parent_pivots(self):
        hip = self.source["rootNode"]["children"].pop()
        pivot = node("HipsPivot", r.IDENTITY, [hip])
        pivot["localTransform"][13] = 1
        self.source["rootNode"]["children"].append(pivot)
        self.source["clips"][0]["channels"].append({
            "name": "HipsPivot", "rotationKeys": [], "scaleKeys": [],
            "positionKeys": [{"px": 0, "py": 1, "pz": 0, "timeStamp": 0},
                             {"px": 0, "py": 2, "pz": 0, "timeStamp": 1}]})
        self.config.update(hipTarget="pelvis", hipMotion="vertical")
        output = r.retarget(self.source, self.skeleton, self.config)
        pelvis = next(c for c in output["clips"][0]["channels"] if c["name"] == "pelvis")
        self.assertAlmostEqual(pelvis["positionKeys"][0]["px"], 0, places=6)
        self.assertAlmostEqual(pelvis["positionKeys"][-1]["px"], 1, places=6)
        self.assertAlmostEqual(pelvis["positionKeys"][-1]["py"], 0, places=6)

    def test_pose_alignment_rotates_target_rest_chain_toward_source(self):
        source_arm = self.source["rootNode"]["children"][0]["children"][0]
        source_arm["localTransform"] = node("unused", r.IDENTITY)["localTransform"]
        self.source["clips"][0]["channels"] = []
        source_child = node("mixamorig:ForeArm", r.IDENTITY)
        source_child["localTransform"][12] = 1
        source_arm["children"].append(source_child)
        target_child = joint("forearm", 3, 2)
        target_child["localBindT"] = [1, 0, 0]
        self.skeleton["bones"].append(target_child)
        self.skeleton["inverseBindMatrices"].append([1, 0, 0, 0]*4)
        self.config["boneMap"]["ForeArm"] = "forearm"
        self.config["poseAlignment"] = {"arm": "forearm"}
        # The target's arm initially points up while the source rest arm points right.
        self.skeleton["bones"][2]["localBindR"] = (
            math.cos(math.pi/4), 0, 0, math.sin(math.pi/4))
        output = r.retarget(self.source, self.skeleton, self.config)
        arm = next(c for c in output["clips"][0]["channels"] if c["name"] == "arm")
        key = arm["rotationKeys"][0]
        local = tuple(key[f] for f in ("qw", "qx", "qy", "qz"))
        direction = r.rotate(r.mul(self.skeleton["bones"][0]["localBindR"], local),
                             target_child["localBindT"])
        self.assertAlmostEqual(direction[0], 1, places=6)
        self.assertAlmostEqual(direction[1], 0, places=6)


if __name__ == "__main__":
    unittest.main()
