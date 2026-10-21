using System.Numerics;

namespace HEngine;

/// <summary>Snapshot for one presentation frame; edge flags do not consume input.</summary>
public readonly record struct ActionState(Vector2 Value, bool Pressed, bool Released, bool Held, bool HoldPerformed, bool DoubleTapped, int PressCount = 0);

/// <summary>Supported binding sources. Key and mouse codes use the engine input code tables.</summary>
public enum ActionDevice
{
    /// <summary>Keyboard key.</summary>
    Key,
    /// <summary>Mouse button.</summary>
    MouseButton,
    /// <summary>Normalized gamepad button.</summary>
    GamepadButton,
    /// <summary>Normalized gamepad axis.</summary>
    GamepadAxis,
    /// <summary>Pointer horizontal delta.</summary>
    PointerX,
    /// <summary>Pointer vertical delta.</summary>
    PointerY
}

/// <summary>Maps a device code to action axis 0/1 with a finite nonzero scale.</summary>
public readonly record struct ActionBinding(ActionDevice Device, int Code, int Axis = 0, float Scale = 1);

/// <summary>Named desktop actions with project defaults and explicit player preference persistence.</summary>
public static class InputActions
{
    /// <summary>Reads an action; a misspelled or unknown action throws.</summary>
    public static ActionState Read(string name) => Engine.Api.ReadAction(name);
    /// <summary>Selects an existing context, such as Gameplay or UI.</summary>
    public static void SetContext(string context) => Engine.Api.SetInputContext(context);
    /// <summary>Changes a binding without mutating the project asset on disk.</summary>
    public static void Rebind(string action, int index, ActionBinding binding) => Engine.Api.RebindInput(action, index, binding);
    /// <summary>Saves player bindings through the host's atomic save service.</summary>
    public static void Save(string slot = "input-bindings") => Engine.Api.WriteSave(slot, Engine.Api.ReadInputMap());
    /// <summary>Loads saved bindings; false means the slot is absent. Invalid data throws without applying it.</summary>
    public static bool Load(string slot = "input-bindings")
    {
        string json = Engine.Api.ReadSave(slot);
        if (string.IsNullOrEmpty(json)) return false;
        Engine.Api.WriteInputMap(json);
        return true;
    }
}
