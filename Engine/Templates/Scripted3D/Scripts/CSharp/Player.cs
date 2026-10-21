using System.Numerics;
using HEngine;

[ScriptType("template.player")]
public sealed class Player : ScriptBehaviour
{
    public override void Update(double seconds)
    {
        var move = InputActions.Read("Move").Value;
        var state = Entity.State;
        state.Position += new Vector3(move.X, 0, -move.Y) * (float)seconds * 3;
        state.Rotation.Y += (float)seconds * .5f;
        Entity.Apply(state);
    }
}
