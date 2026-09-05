# Material preview shader ball provenance

- Upstream project: Academy Software Foundation MaterialX
- Upstream commit: `39e4c9e04580bb690235667f6b28686e60344773`
- Source URL: `https://raw.githubusercontent.com/AcademySoftwareFoundation/MaterialX/39e4c9e04580bb690235667f6b28686e60344773/resources/Geometry/shaderball.glb`
- Source SHA-256: `A47A52C37F6BE60C963C766A6279D302D9940456B9D02972CC8A1BF9026A859A`
- License: Apache License 2.0. A verbatim copy is stored at
  `../../../SourceAssets/MaterialPreview/MaterialX-LICENSE.txt`; upstream
  third-party notices are stored alongside it in `MaterialX-THIRD-PARTY.md`.
- Attribution: MaterialX Viewer documentation identifies this geometry as the
  Arnold Shader Ball contributed by the Solid Angle team at Autodesk.

`MaterialBall.hmesh` and `MaterialBall.bin` were generated offline with the
repository `HAssetImporter`; runtime and packaged builds do not load the GLB.
The original pinned GLB remains in `Engine/Content/SourceAssets/MaterialPreview`
for reproducibility.

Derived artifact SHA-256 values:

- `MaterialBall.hmesh`: `9B016F97D3A8111D682B7DF12B3A760822B8B891C74B474ABFCAA776B2CAD12F`
- `MaterialBall.bin`: `1861841756E37082129400984A71C8CCB05CDB150F970E5F9F73B6C8DDDD565A`

Stable material-slot mapping:

- Primitive 0 (`indices` accessor 5, 212424 indices) is the editable preview
  surface and uses slot `DefaultMaterial_0.hmat` before the Editor replaces it
  with the draft material instance.
- Primitive 1 (`indices` accessor 11, 52368 indices) is the neutral calibration
  / base geometry and keeps `shaderball.hmat`.

