using System.Globalization;
using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HEngine.Portable;

// The game subclass supplies rules and resource paths. No game-specific rules
// live in the native presentation adapter, and no native compilation is needed.
public abstract class NativeViewPresenter : ScriptBehaviour
{
    protected abstract IGameSession CreateSession(string save);
    protected abstract string SaveKey { get; }
    protected abstract string HudPath { get; }
    protected abstract string SpritePrefab { get; }
    private IGameSession? mSession;
    private PortableView mView = new();
    private string mDocument = "";
    private readonly Dictionary<string, Entity> mObjects = new();
    private ulong mUiSequence;
    private long mSaveRevision = -1, mSoundRevision = -1;
    private bool bPause;
    private bool bPresented;

    public override void Initialize()
    {
        mSession = CreateSession(Engine.Api.ReadSave(SaveKey));
        mDocument = SceneServices.LoadUi(HudPath);
        SceneServices.SetUiVisible(mDocument, true);
        Present(mSession.Snapshot());
    }

    public override void Update(double seconds)
    {
        string uiEvent = Engine.Api.ReadEvent("ui.event", mUiSequence, out mUiSequence);
        if (uiEvent.Length > 0)
        {
            var item = JsonNode.Parse(uiEvent)!;
            if (item["type"]?.GetValue<string>() == "click")
            {
                string id = item["source"]?.GetValue<string>() ?? "";
                if (id.StartsWith("portable_action_", StringComparison.Ordinal) &&
                    int.TryParse(id[16..], out int index) && index >= 0 && index < mView.actions.Count)
                    Present(mSession!.Command(mView.actions[index].id, ""));
            }
        }
        var buttons = SceneServices.Call<JsonObject>("input.buttons", new { });
        bool pause = buttons["pause"]?.GetValue<bool>() ?? false;
        if (pause && !bPause && mView.mode is "playing" or "paused")
            Present(mSession!.Command(mView.mode == "playing" ? "pause" : "resume", ""));
        bPause = pause;
        var input = Engine.Input.Movement;
        Present(mSession!.Step(seconds, input.X, -input.Y));
    }

    private void Present(string json)
    {
        var previous = mView;
        mView = JsonSerializer.Deserialize<PortableView>(json) ?? throw new InvalidOperationException("Missing portable view.");
        mView.Validate();
#if HE_ANDROID
        SceneServices.Call<bool>("input.touchControls", new { enabled = mView.mode == "playing" });
#endif
        var active = new HashSet<string>();
        int layer = 0;
        foreach (var item in mView.objects)
        {
            if (!active.Add(item.id)) throw new InvalidOperationException("Duplicate portable object ID.");
            if (!mObjects.TryGetValue(item.id, out var entity))
                mObjects[item.id] = entity = SceneServices.Instantiate(SpritePrefab);
            var state = entity.State;
            // Match Canvas painter order when rectangles overlap. Equal depth
            // can hide the player behind the objective on native renderers.
            state.Position = new((float)item.x, (float)-item.y, layer++ * .001f);
            state.Scale = new((float)item.width, (float)item.height, 1);
            uint color = uint.Parse(item.color.TrimStart('#'), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            state.Color = new((color >> 16 & 255) / 255f, (color >> 8 & 255) / 255f, (color & 255) / 255f, 1);
            entity.Apply(state);
        }
        foreach (string id in mObjects.Keys.Where(id => !active.Contains(id)).ToArray())
        {
            mObjects[id].Destroy();
            mObjects.Remove(id);
        }
        // SetInnerRML replaces geometry. Keep unchanged labels alive across
        // render submissions instead of destroying/recreating them every tick.
        if (!bPresented || previous.title != mView.title)
            SceneServices.SetText(mDocument, "portable_title", mView.title);
        if (!bPresented || previous.status != mView.status)
            SceneServices.SetText(mDocument, "portable_status", mView.status);
        for (int index = 0; index < 4; ++index)
        {
            string element = "portable_action_" + index;
            bool visible = index < mView.actions.Count;
            bool wasVisible = index < previous.actions.Count;
            if (!bPresented || visible != wasVisible)
                SceneServices.SetStyle(mDocument, element, "display", visible ? "block" : "none");
            if (visible && (!bPresented || !wasVisible || previous.actions[index].text != mView.actions[index].text))
                SceneServices.SetText(mDocument, element, mView.actions[index].text);
        }
        bPresented = true;
        if (mView.soundRevision != mSoundRevision)
        {
            if (mView.sound.Length > 0) SceneServices.PlayAudio(mView.sound, volume: .25f);
            mSoundRevision = mView.soundRevision;
        }
        if (mView.saveRevision != mSaveRevision)
        {
            Engine.Api.WriteSave(SaveKey, mSession!.Save());
            mSaveRevision = mView.saveRevision;
        }
    }
    public override void Destroy()
    {
        foreach (var entity in mObjects.Values) if (entity.IsValid) entity.Destroy();
        mObjects.Clear();
    }
}
