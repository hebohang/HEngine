using System.Text.Json;

namespace HEngine;

/// <summary>Explicit typed payload contract. Implementations must encode bounded value data without runtime reflection.</summary>
public interface IScriptData<T> where T : struct, IScriptData<T>
{
    /// <summary>Stable, versioned schema identity. Change it when the wire representation becomes incompatible.</summary>
    static abstract string Schema { get; }
    /// <summary>Encodes one JSON value; never encode handles, delegates or managed object graphs.</summary>
    static abstract string Encode(T value);
    /// <summary>Decodes one JSON value or throws InvalidDataException for an incompatible representation.</summary>
    static abstract T Decode(JsonElement value);
}

/// <summary>Validates a bounded schema envelope shared by saves and scene mailboxes.</summary>
public static class ScriptDataCodec<T> where T : struct, IScriptData<T>
{
    /// <summary>Encodes at most 64 KiB with JSON depth at most 16; invalid schemas and payloads throw.</summary>
    public static string Encode(T value)
    {
        if (string.IsNullOrWhiteSpace(T.Schema) || T.Schema.Length > 128) throw new InvalidDataException("Invalid script data schema.");
        string data = T.Encode(value);
        if (System.Text.Encoding.UTF8.GetByteCount(data) > 64000) throw new InvalidDataException("Script data exceeds 64 KiB.");
        using var document = JsonDocument.Parse(data, new JsonDocumentOptions { MaxDepth = 16 });
        return "{\"schema\":" + JsonSerializer.Serialize(T.Schema) + ",\"value\":" + document.RootElement.GetRawText() + "}";
    }
    /// <summary>Rejects a different schema before calling the explicit typed decoder.</summary>
    public static T Decode(string text)
    {
        if (System.Text.Encoding.UTF8.GetByteCount(text) > 65536) throw new InvalidDataException("Script data exceeds 64 KiB.");
        using var document = JsonDocument.Parse(text, new JsonDocumentOptions { MaxDepth = 17 });
        if (document.RootElement.GetProperty("schema").GetString() != T.Schema) throw new InvalidDataException("Script data schema mismatch: " + T.Schema);
        return T.Decode(document.RootElement.GetProperty("value"));
    }
}

/// <summary>A typed persistent save slot. Values survive scene Stop; writes use the host's existing atomic save service.</summary>
public readonly record struct SaveSlot<T>(string Key) where T : struct, IScriptData<T>
{
    /// <summary>Returns false for an absent save. Corrupt or incompatible data throws and is not overwritten.</summary>
    public bool TryRead(out T value)
    {
        string text = Engine.Api.ReadSave(Key);
        value = default;
        if (string.IsNullOrEmpty(text)) return false;
        value = ScriptDataCodec<T>.Decode(text);
        return true;
    }
    /// <summary>Writes explicit value data; host validation or I/O failures propagate to the script exception boundary.</summary>
    public void Write(T value) => Engine.Api.WriteSave(Key, ScriptDataCodec<T>.Encode(value));
}

/// <summary>A typed scene mailbox, not an unbounded event queue. Consumers observe the latest sequence once per poll.</summary>
public readonly record struct ScriptEvent<T>(string Name) where T : struct, IScriptData<T>
{
    /// <summary>Publishes a bounded value to the current scene. Later writes may replace unread mailbox values.</summary>
    public void Publish(T value) => Engine.Api.EmitEvent(Name, ScriptDataCodec<T>.Encode(value));
    /// <summary>Reads only a newer sequence. Keep the cursor per consumer and per Play session.</summary>
    public bool TryRead(ref ulong cursor, out T value)
    {
        string text = Engine.Api.ReadEvent(Name, cursor, out ulong sequence);
        value = default;
        if (sequence <= cursor || string.IsNullOrEmpty(text)) return false;
        value = ScriptDataCodec<T>.Decode(text);
        cursor = sequence;
        return true;
    }
}

public abstract partial class ScriptBehaviour
{
    /// <summary>Polls a typed mailbox on ordinary updates. The subscription is disposed with this behaviour's coroutines.</summary>
    public void Subscribe<T>(ScriptEvent<T> channel, Action<T> receive) where T : struct, IScriptData<T>
    {
        ArgumentNullException.ThrowIfNull(receive);
        IEnumerator<ScriptWait> Poll()
        {
            ulong cursor = 0;
            while (true)
            {
                yield return ScriptWait.NextFrame;
                if (channel.TryRead(ref cursor, out var value)) receive(value);
            }
        }
        StartCoroutine(Poll());
    }
}
