using HEngine;

[ScriptType("test.json-reload")]
public sealed class JsonReload : ScriptBehaviour
{
    public override void Initialize()
    {
        // Serializes a collectible anonymous request via the real host service.
        _ = SceneServices.FindTag("Player");
    }
}
