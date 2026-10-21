# Runtime loading screen

`Game.LoadingScreen` is an optional object in the project's game config. Projects
without it retain the compact classic loading panel.

```json
{
  "Game": {
    "LoadingScreen": {
      "Style": "CopperNoir",
      "DocumentPath": "Assets/UI/Loading/CopperNoir.rml",
      "Title": "MY GAME",
      "Language": "zh-CN",
      "LanguagePreferencePath": "Config/Frontend.json"
    }
  }
}
```

- `Style`: `CopperNoir` selects the opaque copper/ink RmlUi presentation;
  `Classic` keeps the legacy compact ImGui panel. No external image assets are needed.
- `DocumentPath`: optional RmlUi document path. The built-in document and RCSS
  reside in Engine Content; the existing project-first/engine-fallback resolver
  also permits project-authored replacements. Retain the `loading-*` element IDs
  used by the built-in document to bind localized text and real progress.
- `Title`: optional project branding, rendered as text.
- `Language`: `zh-CN` (default when configured) or `en`.
- `LanguagePreferencePath`: optional `Config/`-relative JSON file containing a
  string `Language`. The project file is read first, then the corresponding
  `ConfigManager::GetSavedPath` file, matching the public `settings.read` service.
  Preferences are refreshed at startup and each scene transition, so changing
  language in a project's menu applies to the next loading screen.

The screen uses the existing scene-document, materialization and texture-upload
progress. Percentages are weighted stage completion, not elapsed-time estimates.
The rotating accent is decorative and never changes the progress or readiness
gates. CopperNoir covers the previous scene while the existing startup/scene
transition remains active, including rendering resource warmup. Project scene
ImGui callbacks are suppressed during loading. The RmlUi overlay has independent
ownership and visibility: activating a menu/HUD does not hide the loading overlay,
and hiding it does not deactivate or steal focus from the underlying menu/HUD.
The layer updates RmlUi even before the first scene exists, so the same document
serves startup and scene transitions. Chinese uses the already-loaded Noto Sans SC
RmlUi font fallback; it does not depend on Game's Latin-only ImGui font.

Validation: Game build and the 1080p LoadingRml42 startup/menu-to-game capture passed; Chinese glyphs and overlay coverage were visually checked. Full transition lifecycle results are recorded in engine-upgrade-validation.md.

2026-09-28 lifecycle verification (`runtime-manual`): PASS with
`powershell -NoProfile -ExecutionPolicy Bypass -File out/agent/frontend-stage-qa.ps1`.
The owned, artifact-pinned DX12 Deferred session exercised Chinese main menu,
wardrobe changes, Chinese/English settings at 720p and 1080p, Play, courtyard HUD,
Escape, return to the lightweight main menu, and preference persistence. The
script's assertions passed and its `finally` stopped the owned process. This
run intentionally makes no performance claim because Editor compilation ran
concurrently.

Evidence in `out/agent/frontend-stage-qa/`: `runtime.log` records three matching
overlay show/hide pairs (startup, menu-to-courtyard, courtyard-to-menu). Hiding
preserved respectively the active main menu, shooter HUD, and restored main menu;
each scene's resource warmup completed with zero pending texture uploads before
the overlay closed. `main-zh720.preview.jpg`, `garden-playing1080.preview.jpg`, and
`restored-main-en1080.preview.jpg` were inspected: no lingering loading overlay,
Chinese and English menu text render correctly, and the selected copper outfit
survives scene changes. `preferences-after-reload.json` confirms the saved language,
outfit and UI scale; `stop.log` confirms session STOPPED.

Scene startup also warms every already-loaded MeshComponent in the current level,
including zero-scale and off-camera instances, through resource-only snapshots.
RuntimeGameLayer retains RenderSystem3D::PrepareLevelMeshResources requests only
while the loading gate is active and releases them on readiness/failure/stop. This
avoids first-shot or first-reveal uploads without drawing hidden meshes or loading
other levels. Procedurally spawned assets still need explicit PrepareMeshResources
before first use; this is not a streaming system for future unknown resources.

Final resident-mesh warmup lifecycle (`runtime-manual`, 2026-09-29): **PASS**:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/ValidateFrontendLifecycle.ps1 -ArtifactPath C:/WorkSpace/HEngineDev/out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe
```

The artifact SHA-256 was
`d1712c946ccc8500237400f82efdd585374e407773042daf84f6dd7aca1fa1c9`.
Fresh project startup created nonexclusive borderless **3840x2160** output.
Actual mouse clicks on Play and Return, with Escape opening the game menu,
produced world counts **11 → 3676 → 11**. The log records **4 → 3103 → 4**
resident mesh requests, three warmup-ready events with `pendingTextures=0`, and
three matching overlay show/hide pairs. Each overlay closes after its scene's
warmup-ready event and preserves the menu/HUD document. Captured courtyard HUD
and returned Chinese main menu were visually checked at bounded preview size;
neither retained the loading overlay. No WARN/ERROR entries were present.

Session `20260929-092405-226-72ea12b196e2` stopped in `finally`
(`STOPPED pid=115064`). Evidence is under the external project's
`Saved/Diagnostics/FrontendLifecycle/20260929-092404/`.
This is lifecycle validation, not a performance measurement. `post-play.png`
captures the menu before transition presentation begins, so it is not claimed as
a loading-screen image; loading presentation has separate LoadingRml42 evidence.

The first script attempt failed an invalid test assumption: `ui.element.rect`
queries the active gameplay document, not the independently owned system overlay.
Its `loading-screen` query therefore cannot establish overlay visibility. That
owned session also stopped. The corrected script validates loading ownership and
readiness from actual lifecycle logs and final scene screenshots; no runtime
logic was weakened to satisfy the test.
