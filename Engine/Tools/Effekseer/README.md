# Effekseer Tool

This directory contains the official Windows Effekseer `1.80.6` editor used by
HEngineEditor to preview `.efkpkg` assets.

- Release tag: `1806`
- Release commit: `93dd29c7b4e353fcef6186fe4ac74c9757243550`
- Archive: `Effekseer1.80.6Win.zip`
- Source: <https://github.com/effekseer/Effekseer/releases/download/1806/Effekseer1.80.6Win.zip>
- SHA-256: `FAF73B3ED8DDB9775B04902CCAE3930CF8476D9BE54761DC0F15A2A2092A8C02`
- Retrieved: `2026-08-13`

The archive hash matches the digest published by the official GitHub Release
API. The complete official `Tool/` directory is copied here so that the launcher,
self-contained runtime, native dependencies, resources, and converters stay on
the same release. HEngine keeps the launcher at `Engine/Tools/Effekseer/Effekseer.exe`
for standalone compatibility.

HEngineEditor launches the official `Effekseer.exe` bootstrapper, which sets up
the Tool working directory, forwards the preview file to `bin/Effekseer.exe`,
and waits for that process to exit. Version-controlled layout defaults live in
`DefaultLayout/`; each project's writable layout cache lives in
`Project/Saved/Effekseer/Layout/`. The transient copies in the Tool root beside
the launcher are ignored by Git. On first use the cache is initialized from the
defaults, and subsequent launches load and write back the project cache.

See `LICENSE_TOOL` for the tool and bundled dependency licenses, and
`OFFICIAL_README.txt` for the upstream package notes. The official archive's
bulk `Sample/` library is intentionally not copied into the repository; effects
are selected individually through the licensed Effekseer asset intake workflow.
