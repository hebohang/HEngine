using System.Text.Json;

namespace HEngine;

/// <summary>Schema-2 codecs used by explicit generated bindings. No runtime field reflection or object graph serializer.</summary>
public static class ScriptAggregateCodec
{
    private static JsonElement Open(JsonElement json, string kind, string identity)
    {
        if (json.GetProperty("type").GetString() != kind || json.GetProperty("value").GetProperty("type").GetString() != identity)
            throw new InvalidDataException($"Expected {kind} identity {identity}.");
        int count = 0;
        Validate(json, 0, ref count);
        return json.GetProperty("value");
    }
    private static void Validate(JsonElement value, int depth, ref int count)
    {
        if (depth > 4 || ++count > 4096) throw new InvalidDataException("Aggregate exceeds depth 4 or 4096 values.");
        string? kind = value.GetProperty("type").GetString();
        if (kind is not ("struct" or "list" or "array")) return;
        var body = value.GetProperty("value");
        string identity = body.GetProperty("type").GetString() ?? "";
        if (identity.Length is 0 or > 256) throw new InvalidDataException("Invalid aggregate identity.");
        var children = body.GetProperty(kind == "struct" ? "fields" : "items");
        if (kind == "struct")
        {
            int members = 0;
            foreach (var child in children.EnumerateObject())
            {
                if (++members > 256 || child.Name.Length is 0 or > 256) throw new InvalidDataException("Invalid structure members.");
                Validate(child.Value, depth + 1, ref count);
            }
        }
        else
        {
            if (children.GetArrayLength() > 256) throw new InvalidDataException("Sequence exceeds 256 items.");
            foreach (var child in children.EnumerateArray()) Validate(child, depth + 1, ref count);
            Validate(body.GetProperty("prototype"), depth + 1, ref count);
        }
    }
    /// <summary>Checks stable structure identity; returned members are borrowed from the input document.</summary>
    public static JsonElement ReadStruct(JsonElement value, string identity) => Open(value, "struct", identity).GetProperty("fields");
    /// <summary>Resolves the current ID followed by explicit FormerIds; missing members retain generated defaults.</summary>
    public static bool TryMember(JsonElement fields, string id, string[] formerIds, out JsonElement value)
    {
        if (fields.TryGetProperty(id, out value)) return true;
        foreach (string former in formerIds) if (fields.TryGetProperty(former, out value)) return true;
        return false;
    }
    /// <summary>Reads at most 256 elements using the supplied generated typed reader.</summary>
    public static T[] ReadSequence<T>(JsonElement value, string kind, string identity, Func<JsonElement, T> read)
    {
        var items = Open(value, kind, identity).GetProperty("items");
        var result = new T[items.GetArrayLength()];
        for (int index = 0; index < result.Length; ++index) result[index] = read(items[index]);
        return result;
    }
    private static string Encode(string kind, string identity, Action<Utf8JsonWriter> body)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject(); writer.WriteString("type", kind); writer.WritePropertyName("value");
            writer.WriteStartObject(); writer.WriteString("type", identity); body(writer); writer.WriteEndObject(); writer.WriteEndObject();
        }
        if (stream.Length > 1024 * 1024) throw new InvalidDataException("Aggregate exceeds 1 MiB.");
        string text = System.Text.Encoding.UTF8.GetString(stream.ToArray());
        using var document = JsonDocument.Parse(text, new JsonDocumentOptions { MaxDepth = 32 });
        _ = Open(document.RootElement, kind, identity);
        return text;
    }
    /// <summary>Writes explicit generated members. Arbitrary objects and delegates cannot be persisted.</summary>
    public static string WriteStruct(string identity, KeyValuePair<string, string>[] fields) => Encode("struct", identity, writer =>
    {
        writer.WritePropertyName("fields"); writer.WriteStartObject();
        foreach (var field in fields) { writer.WritePropertyName(field.Key); writer.WriteRawValue(field.Value); }
        writer.WriteEndObject();
    });
    /// <summary>Writes a bounded sequence and a generated element prototype for editor insertion. Null sequences become empty.</summary>
    public static string WriteSequence<T>(string kind, string identity, IEnumerable<T>? values, Func<T, string> write, string prototype) => Encode(kind, identity, writer =>
    {
        if (kind is not ("list" or "array")) throw new ArgumentException("Invalid sequence kind.");
        writer.WritePropertyName("prototype"); writer.WriteRawValue(prototype);
        writer.WritePropertyName("items"); writer.WriteStartArray();
        int count = 0;
        if (values is not null) foreach (T value in values)
        {
            if (++count > 256) throw new InvalidDataException("Sequence exceeds 256 items.");
            writer.WriteRawValue(write(value));
        }
        writer.WriteEndArray();
    });
}
