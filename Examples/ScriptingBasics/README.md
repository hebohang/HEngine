# C# script examples

Copy Feedback.cs to a Scene project Scripts/CSharp directory and attach the
example.feedback ScriptComponent. Define the Interact action in Config/Input.json
(see Docs/InputActions.md), and assign a real audio resource to Sound.
Pressing Interact saves a counter, logs the result and plays a UI-bus sound.
Disable/destroy releases the voice; restarting restores the saved count.

ApiSmoke.cs is the disposable binary integration probe used by
Tools/Distribution/Test-ScriptSdk.ps1. It exercises entities, transforms,
components, settings, events, UI, audio state and input. Its silent PCM fixture
validates playback controls, not audible quality. It is not a game controller.

Python expansion and binary export are deferred from this 1.1.0 scope.
