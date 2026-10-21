using HEngine;

// Add a ScriptComponent of type example.feedback, assign a real sound asset,
// and define the Interact action in Config/Input.json.
[ScriptType("example.feedback")]
public sealed class Feedback : ScriptBehaviour
{
    [ScriptField("sound", ResourceType = "Audio")]
    public ResourceReference Sound = new("");
    private AudioPlayback? mVoice;
    private int mCount;

    public override void Start()
    {
        var saved = Engine.Api.ReadSave("feedback-count");
        int.TryParse(saved, out mCount);
    }

    public override void Update(double seconds)
    {
        if (!InputActions.Read("Interact").Pressed) return;
        mCount++;
        Engine.Api.WriteSave("feedback-count", mCount.ToString());
        Engine.Log($"Interaction {mCount}");
        mVoice?.Dispose();
        if (!string.IsNullOrEmpty(Sound.Id) && Audio.IsDeviceAvailable)
            mVoice = AudioPlayback.Play(this, Sound, new AudioOptions { Bus = AudioBus.UI, Volume = .7f });
    }

    public override void Disable() { mVoice?.Dispose(); mVoice = null; }
}
