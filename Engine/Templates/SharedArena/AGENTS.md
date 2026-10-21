# SharedArena game project

Keep gameplay in this project. The selected HEngine installation contains the
compiled engine and public managed SDK; a C++ checkout is not required.

- Read `README.md`, `Config/GameRules.json`, `Scripts/CSharp/Rules/GameSession.cs`,
  and the engine's `Tools/Build/Capabilities.json` before changing the game.
- All four profiles compile the same rules file. Keep platform services in the
  engine's PortableView hosts; do not fork the game rules per platform.
- The current view supports colored rectangular sprites, status text, up to four
  actions, a bounded 256-object scene and audio cues. It is a 2D prototype host,
  not an implementation of native physics or a complete 3D browser renderer.
- Change rules and content in small steps. Preserve `IGameSession` and the view
  protocol, validate start/move/pause/resume/win/loss/restart and save reload.
- Use `hectl capabilities -Json`, `hectl agent start -BuildProfile windows`,
  `hectl agent discover`, and `hectl agent call`; always stop your owned session
  in `finally`. Query `ui.element.rect` before clicking, and inspect a screenshot
  as well as entity movement. No native compilation is needed for the SDK route.
- Publish through `hectl publish check|build -BuildProfile <windows|android|web|wechat>`.
  Report build, browser interaction, simulator and real-device results separately.
- Android uses development signing by default. Keep secrets out of the project.
  WeChat's tourist AppID is for local testing; use the owner's AppID for release.
- Do not overwrite unrelated files or change the workspace's default project.
