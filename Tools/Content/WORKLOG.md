# Blender content workflow implementation checkpoint

Scope: cgltf scene service + native output, Blender adapter, immutable publication,
three-way ownership merge, CLI, bounded Agent verification, static collision.

Architecture findings:
- Existing HAssetImporter / IMeshImporterService is a baked-model Assimp interface.
  New ISceneImporterService returns structured results; glTF is not converted to aiScene.
  Removed the unbuilt legacy GltfImporter JSON implementation. Positional baked-model
  CLI remains compatible, including animation; content sync always uses cgltf.
- Native MeshRes accessors + BasePbrMaterialResV2 and generated serialization reused.
  MeshLoader deduplicates material slots by path, requiring neutral per-slot placeholders.
- SceneDocument limit: 32 MiB, 65536 entities, 4096 unique mesh paths; scene.apply
  accepts at most 256 operations. Offline import uses whole-document preflight and
  immutable dependencies, not segmented scene.apply calls.
- RuntimeResourceRegistry pins are generation-specific; overwriting a path does not
  update held references. Content uses immutable paths and restarts validation sessions.
- SceneEditing preview strips gameplay; runtime startup materializes then runs lifecycle.
- Existing Box shape without MeshComponent ignored scale; fixed in PhysicSystem3D.
- Jolt supports concave MeshShape, but the component had no such path. Added cooked
  .hcollision v1 + CollisionAssetPath (reflection-driven), static-only restore, cook edge.
- Blender found at E:/Steam/steamapps/common/Blender/blender.exe, version 5.2.1 LTS.
- VS2026 v145 is available; isolated importer artifact is under
  out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe.

Final implementation evidence is recorded in VALIDATION.md, with exact commands,
report paths and explicit limits. The final fixture has 27 entities, a genuinely
concave cooked doorway, packed texture, dynamic drop and two character tests.
Game and author-preview captures pass; physical state assertions pass in Game.
The fixed-aspect camera load bug and swapped orthographic clips were repaired.
The tool-only Jolt cooker implementation lives under Platform and is compiled only
into Importers; cgltf types do not leave the scene importer implementation.
Editor/Game/Tests codegen no-op checks each report changed=0, deleted=0, and zero
hash/mtime changes for 237 manifest-owned outputs. See the validation report for
the complete test inventory, timing context and unverified shipping/backend cases.

Existing local untracked RenderingBenchmarks assets are user work: untouched.
Task fixtures are Project/SourceAssets/BlenderAcceptance; generated scene resources
are Project/Assets/Imported/<document UUID>. No automatic resource garbage collection.
