using System.Numerics;
using System.Reflection;
using System.Text.Json;
using HEngine;

int checks = 0;
void Check(bool value, string name)
{
    if (!value) throw new InvalidOperationException(name);
    ++checks;
    Console.WriteLine($"PASS {name}");
}

foreach (string template in new[] { "FirstPerson", "TopDown" })
{
    var api = new FakeEngine(template);
    Engine.Attach(api);
    ScriptBehaviour script = template == "FirstPerson" ? new Starter.FirstPerson.FirstPersonController() : new Starter.TopDown.TopDownController();
    script.Bind(api.Owner);
    Engine.ExchangeEntity(api.Owner.Handle);
    try
    {
        Check(script.GetType().GetCustomAttribute<ScriptTypeAttribute>()!.Id == api.TypeId, $"{template}: scene resolves real script type");
        Check(api.Actions.ContainsKey("Move") && api.Actions.ContainsKey("Fire") && api.Actions.ContainsKey("Reset"), $"{template}: authored controls exist");
        script.Initialize();
        int Hits() => script is Starter.FirstPerson.FirstPersonController first ? first.Hits : ((Starter.TopDown.TopDownController)script).Hits;
        int Shots() => script is Starter.FirstPerson.FirstPersonController first ? first.Shots : ((Starter.TopDown.TopDownController)script).Shots;
        var spawn = api.Owner.State;
        api.Actions["Move"] = new(new(1, 1), false, false, true, false, false);
        script.Update(.1);
        Check(MathF.Abs(Vector3.Distance(api.Owner.State.Position, spawn.Position) - .45f) < .001f, $"{template}: diagonal movement has constant speed");
        api.ClearActions();
        api.Actions["Reset"] = new(default, true, false, true, false, false);
        script.Update(.1);
        api.ClearActions();
        Check(api.Owner.State.Position == spawn.Position, $"{template}: reset restores player pose");

        api.Actions["Fire"] = new(default, true, false, true, false, false);
        script.Update(.016);
        Check(Hits() == 1 && Shots() == 1 && api.Tags["Target2"].State.Position.Y == -10, $"{template}: real controller hits center target and hides it");
        script.Update(.01);
        Check(Shots() == 1, $"{template}: cooldown suppresses rapid fire");
        api.ClearActions();
        for (int index = 0; index < 21; ++index) script.Update(.1);
        Check(api.Tags["Target2"].State.Position.Y > 0, $"{template}: target respawns after two simulated seconds");

        api.Actions["Reset"] = new(default, true, false, true, false, false);
        script.Update(.016);
        api.ClearActions();
        Check(Hits() == 0 && Shots() == 0, $"{template}: reset clears score and cooldown");
        var player = api.Owner.State;
        player.Position.X = -2;
        api.Owner.Apply(player);
        var target = api.Tags["Target2"].State;
        target.Position.X = -2;
        api.Tags["Target2"].Apply(target);
        api.Actions["Fire"] = new(default, true, false, true, false, false);
        script.Update(.1);
        Check(Hits() == 0 && Shots() == 1, $"{template}: yellow cover occludes target");

        api.ClearActions();
        player = api.Owner.State;
        player.Position = new(-2, player.Position.Y, .6f);
        api.Owner.Apply(player);
        api.Actions["Move"] = new(new(0, 1), false, false, true, false, false);
        script.Update(.1);
        Check(api.Owner.State.Position.Z == .6f, $"{template}: movement stops at cover");
        player.Position = new(8.5f, player.Position.Y, 8.5f);
        api.Owner.Apply(player);
        api.Actions["Move"] = new(new(1, -1), false, false, true, false, false);
        for (int index = 0; index < 20; ++index) script.Update(.1);
        Check(api.Owner.State.Position.X < 9 && api.Owner.State.Position.Z < 9, $"{template}: player stays inside arena");

        api.ClearActions();
        api.Actions["Reset"] = new(default, true, false, true, false, false);
        script.Update(.1);
        api.ClearActions();
        if (template == "FirstPerson")
        {
            api.Actions["Look"] = new(new(1, 1), false, false, true, false, false);
            script.Update(.1);
            Check(api.Owner.State.Rotation == spawn.Rotation, "FirstPerson: mouse look requires right button");
            api.Actions["LookHeld"] = new(default, false, false, true, false, false);
            for (int index = 0; index < 20; ++index) script.Update(.1);
            Check(api.Owner.State.Rotation.X == 1.3f && api.Owner.State.Rotation.Y < 0, "FirstPerson: look direction and pitch clamp");
        }
        else
        {
            api.Actions["Aim"] = new(new(1, 0), false, false, true, false, false);
            script.Update(.1);
            Check(MathF.Abs(api.Owner.State.Rotation.Y + MathF.PI / 2) < .001f, "TopDown: aim changes facing independently of movement");
            api.ClearActions();
            target = api.Tags["Target1"].State;
            target.Position = spawn.Position + new Vector3(5, 0, 0);
            api.Tags["Target1"].Apply(target);
            api.Actions["Fire"] = new(default, true, false, true, false, false);
            script.Update(.1);
            Check(Hits() == 1, "TopDown: releasing aim retains direction for next shot");
        }
        Check(api.LastScore.Contains("Hits"), $"{template}: HUD receives score updates");
    }
    finally
    {
        script.Destroy();
        Check(api.Releases == 1, $"{template}: HUD released on script teardown");
        Engine.Detach();
    }
}

// Both stand-alone templates carry the same tiny rules helper; exercise each copy.
foreach (bool firstPerson in new[] { true, false })
{
    var api = new FakeEngine(firstPerson ? "FirstPerson" : "TopDown");
    Engine.Attach(api);
    try
    {
        var origin = new Vector3(0, 1, 7);
        for (int index = 1; index <= 3; ++index)
        {
            var state = api.Tags[$"Target{index}"].State;
            state.Position = new(0, 1, 7 - index * 3);
            api.Tags[$"Target{index}"].Apply(state);
        }
        var first = new Starter.FirstPerson.TargetRange();
        var top = new Starter.TopDown.TargetRange();
        Action initialize = firstPerson ? first.Initialize : top.Initialize;
        Action<Vector3, Vector3> fire = firstPerson ? first.Fire : top.Fire;
        initialize();
        fire(origin, Vector3.UnitZ);
        Check(api.Tags["Target1"].State.Position.Y == 1, $"Range {firstPerson}: ray rejects targets behind origin");
        fire(origin + Vector3.UnitX * 2, -Vector3.UnitZ);
        Check(api.Tags["Target1"].State.Position.Y == 1, $"Range {firstPerson}: parallel slab miss stays a miss");
        fire(origin, -Vector3.UnitZ);
        Check(api.Tags["Target1"].State.Position.Y == -10 && api.Tags["Target2"].State.Position.Y == 1, $"Range {firstPerson}: only nearest target is hit");
        fire(origin, -Vector3.UnitZ);
        Check(api.Tags["Target2"].State.Position.Y == -10, $"Range {firstPerson}: inactive target no longer absorbs shots");
    }
    finally { Engine.Detach(); }
}
Console.WriteLine($"Starter gameplay: {checks} checks passed (real ScriptCore, fake scene transport; no renderer).");

sealed class FakeEngine : IEngineApi
{
    private readonly Dictionary<ulong, EntityState> mStates = new();
    public Dictionary<string, Entity> Tags { get; } = new();
    public Dictionary<string, ActionState> Actions { get; } = new();
    public Entity Owner { get; }
    public string TypeId { get; } = "";
    public string LastScore { get; private set; } = "";
    public int Releases { get; private set; }

    public FakeEngine(string template)
    {
        string path = Path.Combine(AppContext.BaseDirectory, "Fixtures", template);
        using var scene = JsonDocument.Parse(File.ReadAllText(Path.Combine(path, "Main.he")));
        foreach (var property in scene.RootElement.EnumerateObject())
        {
            var item = property.Value;
            var transform = item.GetProperty("TransformComponent");
            ulong handle = (ulong)mStates.Count + 1;
            mStates.Add(handle, new() { Position = Vector(transform.GetProperty("Translation")), Rotation = Vector(transform.GetProperty("Rotation")), Scale = Vector(transform.GetProperty("Scale")) });
            Tags.Add(item.GetProperty("TagComponent").GetProperty("Tag").GetString()!, new(handle));
            if (item.TryGetProperty("ScriptComponent", out var script))
            { Owner = new(handle); TypeId = script.GetProperty("Type").GetString()!; }
        }
        using var map = JsonDocument.Parse(File.ReadAllText(Path.Combine(path, "Input.json")));
        foreach (var action in map.RootElement.GetProperty("Actions").EnumerateArray())
        {
            if (action.GetProperty("Bindings").GetArrayLength() == 0) throw new InvalidOperationException("Unbound action.");
            Actions.Add(action.GetProperty("Name").GetString()!, default);
        }
    }
    private static Vector3 Vector(JsonElement value) => new(value[0].GetSingle(), value[1].GetSingle(), value[2].GetSingle());
    public void ClearActions() { foreach (string name in Actions.Keys.ToArray()) Actions[name] = default; }
    public bool IsValid(ulong entity) => mStates.ContainsKey(entity);
    public EntityState ReadEntity(ulong entity) => mStates[entity];
    public void WriteEntity(ulong entity, in EntityState state, bool transform, bool sprite) => mStates[entity] = state;
    public InputState ReadInput() => default;
    public ActionState ReadAction(string name) => Actions[name];
    public void DestroyEntity(ulong entity) => mStates.Remove(entity);
    public Entity FindEntity(string stableId) => throw new NotSupportedException();
    public void SetEnabled(ulong entity, bool enabled) => throw new NotSupportedException();
    public string ReadTexture(ulong entity) => throw new NotSupportedException();
    public void WriteTexture(ulong entity, string path) => throw new NotSupportedException();
    public void Log(string message) => Console.WriteLine(message);
    public string ReadSave(string key) => "";
    public void WriteSave(string key, string value) => throw new NotSupportedException();
    public void EmitEvent(string name, string payload) => throw new NotSupportedException();
    public string ReadEvent(string name, ulong afterSequence, out ulong sequence) { sequence = 0; return ""; }
    public string PlatformService(string name, string request)
    {
        using var json = JsonDocument.Parse(request);
        var value = json.RootElement;
        switch (name)
        {
            case "scene.findTag": return Tags[value.GetProperty("tag").GetString()!].Handle.ToString();
            case "ui.ownedLoad": return "{\"lease\":1,\"document\":\"starter-hud\"}";
            case "ui.text": LastScore = value.GetProperty("value").GetString()!; return "true";
            case "service.release": ++Releases; return "true";
            default: throw new NotSupportedException(name);
        }
    }
}
