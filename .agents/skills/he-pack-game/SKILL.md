---
name: he-pack-game
description: Build and verify the cooked shipping game package with PACK_GAME enabled. Use only for package, cooker, manifest, shipping dependency, or distributable artifact requests.
---

# Package game

Use the unified explicit package route:

```powershell
.\hectl.bat package
.\hectl.bat package -Project D:\Games\MyGame
```

It delegates to the authoritative `Win-PackGame.bat` workflow with
`PACK_GAME=ON`, `HE_BUILD_DEV_GAME=OFF`, and an isolated Release build for the
selected `Project.heproject`. Success requires the `PackageGame` target to
complete and publish the default project to repository-root
`dist/<ExecutableName>` (external projects use a hashed namespace beneath
`dist/projects`). Do not treat an HGame
development build as package evidence.

Report `build-only`, the exact command, PASS/FAIL, and the package directory. Inspect cooker/package logs on failure.
