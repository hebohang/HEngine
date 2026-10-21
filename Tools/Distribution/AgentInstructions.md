# HEngine binary installation

This is a versioned binary engine, not a C++ source checkout. Keep gameplay,
scenes, assets and settings in the selected project. Do not edit installed engine
files to implement a game or copy engine content into the project.

Start by reading `HEngine.distribution.json`, the project's `Project.heproject`,
and `Docs/AI_GAME_DEVELOPMENT.md`. Use `Docs/ScriptingGuide.md` and the generated
`Docs/Api` reference for gameplay. `Docs/ReleaseSDK-1.1.0.md` contains historical
implementation and release evidence; its old roadmap is not the current API.
`HEngine.Runtime.json` describes the installed native runtime features. The
binary Editor's Plugins panel can enable its bundled Path Tracing/OIDN plugin
for the current project after restart. OIDN is loaded only on a denoise request;
it is not a Shipping dependency. Other native plugin combinations still need a
matching source build. Never copy vendor DLLs from an unrelated SDK or PATH.
Use `hectl.bat project current -Json -Project <path>` to inspect project selection.
Create projects with `hectl.bat project create -Project <new-path> -Name <name>`;
`-Template Scripted3D` provides a C# movement example. That template currently
has a Windows build profile, not a verified four-platform game.
`-Template SharedArena` creates a 2D game with shared C# rules and all four build
profiles. Its PortableView hosts support colored rectangles, text/actions and
audio cues; inspect the template README and actual validation records before
making broader renderer or device claims. Use `hectl publish check -BuildProfile <id>`
then `hectl publish build -BuildProfile <id>` through the same backend used by Editor.

Start gameplay work with `Docs/AI_GAME_DEVELOPMENT.md` and `Docs/ScriptingGuide.md`.
Use `Docs/Api/index.md` or `Docs/Api/api-index.json` for the installed script API,
and `Examples/ScriptingBasics`
for lifecycle/resource examples. `Docs/ScriptingApiAudit-1.1.0.md` documents gaps
and language/platform differences. `Docs/Scripting.md` is historical evidence;
`Tools/Scripting/Api.schema.json` defines the transport, not the gameplay API.
Use
`Docs/InputActions.md` for input, and `Docs/BuildAndPublish.md` for export.
C# editing is supported, but compiling it requires the pinned .NET SDK and the
project's build dependencies. The entry point is
`Tools/Scripting/CompileEditor.ps1 -Project <path>`; report actual diagnostics.
Never claim an SDK is installed merely because the .NET runtime is present.

Use `hectl deps check -Profile SDK -Project <path>` to diagnose script/export
dependencies; `deps install -Profile SDK` installs missing supported tools.
Windows Scene projects use the installed precompiled C# Shipping template.
The 1.1.0 binary game export contract covers C#. Existing Editor Python support
does not imply Python Player/export support; that expansion is deferred.
Their descriptor must explicitly declare `RuntimeMode: "Scene"`; custom native
gameplay requires a matching custom player or the source workflow. Android uses
its matching ARM64 export template plus the pinned JDK/Android SDK/.NET workload;
check the release evidence before claiming that an installation contains it.
Windows, Android, Web and WeChat use
`Tools/Build/Publish.ps1 -Project <path> -Profile <profile> -Action Check`
then `-Action Build`, provided the project has an appropriate profile and tools.
Check `Config/BuildProfiles.json`; do not invent a platform profile or promise
that unsupported components can run on another platform.

With `DevelopmentTemplates/Windows-x64/Template.json` installed, use
`hectl agent start -Project <path> -BuildProfile windows` for an owned development
session. It validates the template, compiles project C# into isolated session
storage and starts the precompiled host without a native build. Use `agent discover`
and `agent keys` before sending commands; key names are lowercase. Query entities,
inject input, advance frames and inspect captures/logs. Always `agent stop` in a
finally block. Shipping export rejects this development template. Source-oriented
`game check` and custom native gameplay still require the source workflow.

Read and follow the project's own instructions. Keep changes reviewable; do not
overwrite unrelated user work. State separately what was edited, compiled,
exported and actually run. Building JavaScript or creating files does not prove
gameplay works. Store logs/screenshots on disk and keep command output bounded.
Signing credentials stay with the user and never enter project manifests/logs.

# Discover the supported platform contract before planning a game

Run `hectl capabilities -Json` without a project to query the binary SDK's current
support boundary. Build Profiles can declare `requiredFeatures` for preflight.
`supported` describes an implementation, not a successful device test; `adapter`
requires project presentation code and actual platform validation. Features not
listed are unclassified. Native C# scene behaviours do not automatically run on
Web or WeChat: share `IGameSession` rules and supply a supported presentation host.
