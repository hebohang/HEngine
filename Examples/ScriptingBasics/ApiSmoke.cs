using System.Numerics;
using System.Text.Json;
using HEngine;

// Used by Tools/Distribution/Test-ScriptSdk.ps1 in a disposable project.
[ScriptType("example.api-smoke")]
[ScriptDependency("Assets/Audio/Probe.wav")]
[ScriptDependency("Assets/UI/Probe.rml")]
public sealed class ApiSmoke : ScriptBehaviour
{
    private UiDocument? mDocument;
    private int mPresses;
    private static void Require(bool condition, string diagnostic)
    {
        if (!condition) throw new InvalidOperationException(diagnostic);
    }

    public override void Start()
    {
        var child = HEngine.Entity.Create("Script API probe");
        child.Parent = Entity;
        Require(child.Parent == Entity && Entity.GetChildrenSnapshot().Contains(child), "hierarchy");
        var transform = child.GetComponent<Transform>();
        transform.LocalPosition = new(1, 2, 3);
        Require(transform.LocalPosition == new Vector3(1, 2, 3), "transform");
        child.AddComponent<Sprite>();
        Require(child.HasComponent<Sprite>() && child.RemoveComponent<Sprite>(), "components");
        var copy = HEngine.Entity.Instantiate(child);
        copy.Destroy(); child.Destroy();
        Require(!copy.IsValid && !child.IsValid, "destruction");
        Require(SceneServices.FindTag("Player").IsValid, "scene query");
        Require(Camera.SetFieldOfView(.8f) && Camera.ForwardRay().Direction.Length() > .99f, "camera");
        Require(!new Animator(Entity).Pause() && !SceneSequence.Pause(Entity), "missing components");
        Require(!Physics.SetVelocity(Entity, Vector3.Zero), "missing physics body");
        Require(!Physics.TryGetVelocity(Entity, out var velocity) && velocity == Vector3.Zero &&
            !Physics.AddImpulse(Entity, Vector3.One), "missing physics query/impulse");
        GameSettings.Write("Config/Probe.json", new Dictionary<string, object> { ["score"] = 7 });
        Require(GameSettings.Read("Config/Probe.json").GetProperty("score").GetInt32() == 7, "settings");
        Engine.Api.EmitEvent("sdk-probe", "copied-value");
        Require(Engine.Api.ReadEvent("sdk-probe", 0, out _) == "copied-value", "events");
        mDocument = UiDocument.Load(this, new("Assets/UI/Probe.rml"));
        Require(mDocument.SetText("result", "SDK API PASS"), "UI text");
        Require(mDocument.SetAttribute("result", "title", "C# SDK probe") && mDocument.SetStyle("result", "opacity", "0.9"), "UI attributes/style");
        string audio = "device-unavailable";
        if (Audio.IsDeviceAvailable)
        {
            using var voice = AudioPlayback.Play(this, new("Assets/Audio/Probe.wav"), new AudioOptions {
                Bus = AudioBus.UI, Loop = true, Stream = false, Position = Vector3.Zero, Volume = .05f });
            Require(voice.Pause() && voice.IsPaused, "audio pause");
            Require(voice.SetVolume(.03f) && voice.SetPitch(1) && voice.SetPosition(Vector3.One), "audio parameters");
            Require(voice.Length > 0 && voice.Seek(.01) && voice.Resume(), "audio seek/resume");
            audio = "playback-controls-passed";
        }
        Engine.Api.WriteSave("sdk-api-smoke", JsonSerializer.Serialize(new { passed = true, audio }));
        Engine.Log("SDK_API_SMOKE_PASS " + audio);
    }

    public override void Update(double seconds)
    {
        if (!InputActions.Read("Interact").Pressed) return;
        mPresses++;
        Engine.Api.WriteSave("sdk-input", mPresses.ToString());
        mDocument?.SetText("result", "Input presses: " + mPresses);
    }
}
