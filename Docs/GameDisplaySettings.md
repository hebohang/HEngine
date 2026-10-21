# Game startup window mode

Projects opt in through `Config/Game.json` (or their configured game config):

```json
{
  "Game": {
    "Display": {
      "DefaultMode": "BorderlessFullscreen",
      "PreferencesPath": "Config/Frontend.json"
    }
  }
}
```

`DefaultMode` accepts `Windowed` or `BorderlessFullscreen`. Projects without this
object retain their previous ordinary-window startup. `PreferencesPath` is an
optional project-selected `Config/` JSON path, not an engine-specific frontend
file. Its boolean `Fullscreen` preference is resolved from the project file and
then the corresponding Saved file, so an explicit saved `false` remains windowed.
The result is applied before creating the native window and renderer, including
the first loading screen. No user preference files are rewritten by startup.

`--windowed` and `--borderless-fullscreen` override and lock the startup mode.
`--resolution=WIDTHxHEIGHT` has final precedence and retains the exact windowed
automation surface, including when a frontend reapplies its saved preferences.
Normal launches do not lock the mode: `settings.display` with `Fullscreen` switches
between windowed and borderless at runtime. Existing window position, normal size,
maximized state and resizability are restored on leaving borderless mode.

The GLFW implementation keeps the native window detached from a monitor in both
modes. Borderless uses the current desktop extent and does not request a display
mode or refresh-rate switch. The primary monitor is used at startup; subsequent
toggles use the monitor with the greatest overlap with the current window.
Platform/API types remain in `Runtime/Platform/GLFW`.

`[Display]` log entries record creation and mode changes, including client size,
actual framebuffer size, window position, content scale, monitor name, desktop
extent/refresh rate and whether GLFW has an exclusive monitor association.
The read-only `settings.display` result also reports `WindowWidth/WindowHeight`,
`OutputWidth/OutputHeight`, and `InternalWidth/InternalHeight`. Rendering values
come from the most recently recorded main-scene statistics (zero before a frame
is recorded), not from configured resolution percentages. A resize is deferred to
the frame boundary, so callers should refresh statistics on a subsequent frame.

Validation: static diff review passed. `GameDisplaySettings` is registered as a
headless test covering opt-in defaults, saved explicit false, project/Saved/CLI
precedence, fixed-resolution override, unsafe preference paths and unsupported
exclusive mode rejection. Build/test results are owned by the coordinated
integration record; the runtime checks below do not claim 4K performance.

2026-09-29 `runtime-manual` PASS:

- `powershell -NoProfile -ExecutionPolicy Bypass -File out/agent/fullscreen-roundtrip-qa.ps1`
  used a fresh isolated Saved root and the native startup policy. Creation logged
  borderless client/framebuffer **3840x2160**, desktop **3840x2160 at 60 Hz**,
  content scale **1.5**, and `exclusive=false`. The actual settings dropdown
  switched to windowed **3840x2054** and back to borderless **3840x2160**. Saved
  `Fullscreen` changed false/true accordingly; the main menu remained functional.
  Evidence: `out/agent/fullscreen-roundtrip-qa/display-evidence.log`, screenshot
  previews and saved preference snapshots. Session:
  `20260929-090007-621-78d6c6985ab3` in the MechanicGirlLab game sessions directory.
- `powershell -NoProfile -ExecutionPolicy Bypass -File out/agent/display-dropdown-qa.ps1`
  verified that opening the quality dropdown did not change requested settings,
  and moving the highlight then pressing Escape canceled without applying.
  Resolution choices changed the actual compiled RenderGraph extent:
  **1920x1080 → 3840x2160 → 1280x720 → 1920x1080**. Captures remained **3840x2160**,
  demonstrating separate native UI/output and internal scene extents. Evidence:
  `out/agent/display-dropdown-qa/*-graph.json` and screenshot previews. Session:
  `20260929-090316-427-84a9cde0c17b`.

Both owned sessions stopped in `finally` (confirmed by their `stop.log`). Native
window modes, saved preferences and dropdown navigation were exercised with
synthetic keyboard input; mouse selection was not tested by these scripts.
Chinese text and the fullscreen/dropdown layouts were visually inspected. The
small dropdown triangle glyph was missing in this tested artifact and was
reported separately to the frontend owner; it did not block option selection.

Final mouse and spatial-AA validation (`runtime-manual`, 2026-09-29): **PASS**.
The reusable project script was run with the final integrated artifact:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/ValidateDisplayDropdowns.ps1 -ArtifactPath C:/WorkSpace/HEngineDev/out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe
```

- Artifact SHA-256: `70856909597a563b949ad4ce1371a7397e8261f89201d146527a1356f60a90e3`.
  Owned session: `20260929-090916-692-c2317f159d13`; `finally` confirmed
  `STOPPED pid=110360`. The captured runtime log contains no WARN/ERROR entries.
- Virtual mouse clicks targeted the real Rml element bounds. Opening quality and
  canceling with Escape preserved requested settings. Selecting Performance then
  Balanced disabled then enabled effective AO and skin SSS as expected.
- Fresh startup was borderless **3840x2160**, scale **1.5**, `exclusive=false`.
  Mouse choices switched windowed **3840x2054** then borderless **3840x2160**;
  isolated Saved preferences recorded false then true. This checks persistence
  writes, not a second process restart with the same Saved directory.
- Native, 720p and 1080p choices produced actual graph extents **3840x2160**,
  **1280x720** and **1920x1080**, while fullscreen captures stayed **3840x2160**.
  Revision/hash-checked Session transactions selected **FXAA → SMAA → TAA**;
  effective settings and graph snapshots confirmed each mode at internal 1080p.
- Sequential visual inspection of Balanced, all three AA captures and the restored
  main menu found complete scene/UI output without blank areas or layout clipping.
  The replacement ASCII `v` dropdown markers rendered correctly; the earlier
  missing-triangle observation is resolved in this artifact. This short UI run
  does not establish temporal AA motion quality or performance.

Evidence and preview images:
`C:/WorkSpace/HEngineProjects/MechanicGirlLab/Saved/Diagnostics/DisplayDropdowns/20260929-090916/`.
The directory includes `result.json`, `runtime.log`, `stop.log`, settings/preference
snapshots, graph snapshots, native captures and bounded JPEG previews.

For an owned display-mode QA session, `Tools/Agent/Session.ps1` accepts
`-UseProjectDisplaySettings`. This opt-in switch omits `--resolution` without
forcing a fullscreen CLI override, so the normal project/Saved startup policy
and in-game window-mode choices can be tested. The session still creates its own
isolated Saved root and must be stopped in `finally`. Example direct invocation:

```powershell
& Tools/Agent/Session.ps1 -Action Start -ProjectRoot C:/WorkSpace/HEngineProjects/MechanicGirlLab `
    -Configuration RelWithDebInfo -UseProjectDisplaySettings -GraphicsApi DX12 -StrictGraphicsApi
try {
    # Use the session's input, capture and status commands to exercise the UI.
} finally {
    & Tools/Agent/Session.ps1 -Action Stop -ProjectRoot C:/WorkSpace/HEngineProjects/MechanicGirlLab `
        -Configuration RelWithDebInfo
}
```

For a previously validated binary, supply `-ArtifactPath`, `-SessionRoot`,
`-ExistingBuildSha256` and the same unique `-OwnerTag` on all session calls.
The default remains the existing 1280x720 fixed window. Combining an explicit
`-Resolution` with `-UseProjectDisplaySettings` is rejected instead of silently
ignoring either intent. The hectl dispatcher is unchanged.
