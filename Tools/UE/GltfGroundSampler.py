"""Sample upper ground surfaces in a static glTF scene for offline decal placement.

The sampler uses glTF node transforms and triangle geometry. It intentionally
does not infer collision settings or replace runtime projected decals.
"""

import json
from itertools import product
from pathlib import Path


class GltfGroundSampler:
    def __init__(self, source):
        import numpy as np

        self.np = np
        self.source = Path(source)
        self.document = json.loads(self.source.read_text(encoding="utf-8"))
        if len(self.document.get("buffers", [])) != 1:
            raise ValueError("Ground sampler requires one external glTF buffer")
        uri = self.document["buffers"][0]["uri"]
        if ":" in uri or uri.startswith(("/", "\\")) or ".." in Path(uri).parts:
            raise ValueError("Ground buffer URI must be local to the glTF")
        self.binary = (self.source.parent / uri).read_bytes()
        self.mesh_cache = {}
        self.instances = []
        identity = np.eye(4, dtype=np.float64)
        for root in self.document["scenes"][self.document.get("scene", 0)]["nodes"]:
            self._visit(root, identity)
        self.bounds_min = np.asarray([item[2] for item in self.instances])
        self.bounds_max = np.asarray([item[3] for item in self.instances])

    def _accessor(self, index):
        np = self.np
        accessor = self.document["accessors"][index]
        if "sparse" in accessor:
            raise ValueError("Sparse ground accessors are not supported")
        view = self.document["bufferViews"][accessor["bufferView"]]
        dtype = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32,
                 5126: np.float32}[accessor["componentType"]]
        components = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[accessor["type"]]
        item_size = np.dtype(dtype).itemsize
        offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        stride = view.get("byteStride", components * item_size)
        return np.ndarray((accessor["count"], components), dtype=dtype,
                          buffer=self.binary, offset=offset,
                          strides=(stride, item_size)).copy()

    def _mesh(self, index):
        if index in self.mesh_cache:
            return self.mesh_cache[index]
        np = self.np
        primitives = []
        for primitive in self.document["meshes"][index]["primitives"]:
            if primitive.get("mode", 4) != 4 or "POSITION" not in primitive["attributes"]:
                continue
            vertices = self._accessor(primitive["attributes"]["POSITION"]).astype(np.float64)
            indices = self._accessor(primitive["indices"]).reshape((-1, 3)) \
                if "indices" in primitive else np.arange(len(vertices)).reshape((-1, 3))
            primitives.append((vertices, indices))
        self.mesh_cache[index] = primitives
        return primitives

    def _local_matrix(self, node):
        np = self.np
        if "matrix" in node:
            return np.asarray(node["matrix"], dtype=np.float64).reshape((4, 4)).T
        matrix = np.eye(4, dtype=np.float64)
        x, y, z, w = node.get("rotation", [0, 0, 0, 1])
        matrix[:3, :3] = np.asarray([
            [1 - 2 * (y*y + z*z), 2 * (x*y - z*w), 2 * (x*z + y*w)],
            [2 * (x*y + z*w), 1 - 2 * (x*x + z*z), 2 * (y*z - x*w)],
            [2 * (x*z - y*w), 2 * (y*z + x*w), 1 - 2 * (x*x + y*y)],
        ]) @ np.diag(node.get("scale", [1, 1, 1]))
        matrix[:3, 3] = node.get("translation", [0, 0, 0])
        return matrix

    def _visit(self, index, parent):
        np = self.np
        node = self.document["nodes"][index]
        world = parent @ self._local_matrix(node)
        if "mesh" in node:
            mesh = node["mesh"]
            corners = []
            for vertices, _ in self._mesh(mesh):
                if len(vertices):
                    minimum, maximum = vertices.min(axis=0), vertices.max(axis=0)
                    corners.extend(product(*zip(minimum, maximum)))
            if corners:
                bounds = (world[:3, :3] @ np.asarray(corners).T).T + world[:3, 3]
                self.instances.append((mesh, world, bounds.min(axis=0), bounds.max(axis=0)))
        for child in node.get("children", []):
            self._visit(child, world)

    def highest_surface(self, x, z, near_y, search_m=0.75):
        """Return uppermost near-horizontal surface at XZ near a decal's UE height."""
        np = self.np
        candidate = np.flatnonzero(
            (self.bounds_min[:, 0] <= x) & (self.bounds_max[:, 0] >= x) &
            (self.bounds_min[:, 2] <= z) & (self.bounds_max[:, 2] >= z) &
            (self.bounds_min[:, 1] <= near_y + search_m) &
            (self.bounds_max[:, 1] >= near_y - search_m))
        heights = []
        for instance_index in candidate:
            mesh, world, _, _ = self.instances[int(instance_index)]
            for vertices, indices in self._mesh(mesh):
                points = (world[:3, :3] @ vertices.T).T + world[:3, 3]
                triangles = points[indices]
                a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
                denominator = ((b[:, 2] - c[:, 2]) * (a[:, 0] - c[:, 0]) +
                               (c[:, 0] - b[:, 0]) * (a[:, 2] - c[:, 2]))
                valid = np.abs(denominator) > 1e-10
                safe = np.where(valid, denominator, 1.0)
                u = ((b[:, 2] - c[:, 2]) * (x - c[:, 0]) +
                     (c[:, 0] - b[:, 0]) * (z - c[:, 2])) / safe
                v = ((c[:, 2] - a[:, 2]) * (x - c[:, 0]) +
                     (a[:, 0] - c[:, 0]) * (z - c[:, 2])) / safe
                normal = np.cross(b - a, c - a)
                horizontal = np.abs(normal[:, 1]) / np.maximum(
                    np.linalg.norm(normal, axis=1), 1e-10) >= 0.5
                inside = valid & horizontal & (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6)
                if not inside.any():
                    continue
                height = u[inside] * a[inside, 1] + v[inside] * b[inside, 1] + \
                    (1 - u[inside] - v[inside]) * c[inside, 1]
                heights.extend(height[(height >= near_y - search_m) &
                                      (height <= near_y + search_m)].tolist())
        return max(heights) if heights else None
