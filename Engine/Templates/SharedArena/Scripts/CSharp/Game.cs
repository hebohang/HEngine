using HEngine;
using HEngine.Portable;
using GameScripts.Rules;

namespace GameScripts;

[ScriptType("template.shared-arena")]
[ScriptDependency("Assets/UI/Game.hui")]
[ScriptDependency("Assets/Prefabs/Pixel.hprefab")]
[ScriptDependency("Assets/Audio/Cue.wav")]
public sealed class Game : NativeViewPresenter
{
    protected override string SaveKey => "shared-arena-v1";
    protected override string HudPath => "Assets/UI/Game.hui";
    protected override string SpritePrefab => "Assets/Prefabs/Pixel.hprefab";
    protected override IGameSession CreateSession(string save)
    {
        using var stream = typeof(Game).Assembly.GetManifestResourceStream("Game.rules.json")!;
        using var reader = new StreamReader(stream);
        return new GameSession(reader.ReadToEnd(), save);
    }
}
