# Scripted 3D project

Move the cube with WASD or the left gamepad stick. Gameplay is in
`Scripts/CSharp/Player.cs`; the C++ entry point is the standard scene host.
The project references the engine's built-in Box resource without copying it.

From the engine checkout:

```powershell
./Tools/Agent/hectl.ps1 game check -Project <project>
./Tools/Build/Publish.ps1 -Project <project> -Profile windows -Action Check
./Tools/Build/Publish.ps1 -Project <project> -Profile windows -Action Build
```

Windows export currently uses the source engine's validated Shipping cooker and
requires its documented native/.NET toolchain. A binary Editor alone cannot
compile an arbitrary Windows executable. The build profile writes `Dist/Windows`.
Input bindings can be saved through Project Settings → Input Actions.
