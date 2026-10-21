using System.Numerics;
using System.Text;
using System.Text.Json;

namespace HEngine;

/// <summary>One generated stable field binding. Readers/writers use the bounded value schema.</summary>
/// <param name="Id">Stable field identity.</param>
/// <param name="Write">Generated typed assignment after schema validation.</param>
/// <param name="Read">Generated typed value encoder.</param>
public sealed record ScriptFieldRegistration(string Id, Action<ScriptBehaviour, JsonElement> Write, Func<ScriptBehaviour, string> Read);
/// <summary>Explicit generated behaviour factory and fields; runtime assembly scanning is unnecessary.</summary>
/// <param name="Id">Stable scene script type ID.</param>
/// <param name="Create">Typed factory called once per runtime instance.</param>
/// <param name="Fields">Generated field bindings, unique by stable field ID.</param>
public sealed record ScriptRegistration(string Id, Func<ScriptBehaviour> Create, ScriptFieldRegistration[] Fields);

// Deliberately limited value schema, matching ScriptFieldsJson. No managed
// object graphs, private fields, delegates or arbitrary polymorphic serializers.
/// <summary>Strict scalar codecs for generated bindings; mismatched tags, identities and nonfinite values throw.</summary>
public static class ScriptValueCodec
{
    private static JsonElement Value(JsonElement json, string type)
    {
        if (json.GetProperty("type").GetString() != type) throw new InvalidDataException($"Expected script field type {type}.");
        return json.GetProperty("value");
    }
    private static string Encode(string type, Action<Utf8JsonWriter> write)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject(); writer.WriteString("type", type); writer.WritePropertyName("value");
            write(writer); writer.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }
    /// <summary>Reads the declared scalar/vector type; invalid tags, dimensions, numeric ranges or JSON shapes throw.</summary>
    public static bool ReadBool(JsonElement json) => Value(json, "bool").GetBoolean();
    /// <summary>Reads the declared scalar/vector type; invalid tags, dimensions, numeric ranges or JSON shapes throw.</summary>
    public static long ReadInt64(JsonElement json) => Value(json, "int64").GetInt64();
    /// <summary>Reads the declared scalar/vector type; invalid tags, dimensions, numeric ranges or JSON shapes throw.</summary>
    public static double ReadDouble(JsonElement json)
    {
        double value = Value(json, "float64").GetDouble();
        return double.IsFinite(value) ? value : throw new InvalidDataException("Script number must be finite.");
    }
    /// <summary>Reads the declared scalar/vector type; invalid tags, dimensions, numeric ranges or JSON shapes throw.</summary>
    public static string ReadString(JsonElement json) => Value(json, "string").GetString() ?? "";
    /// <summary>Reads the declared scalar/vector type; invalid tags, dimensions, numeric ranges or JSON shapes throw.</summary>
    public static EntityReference ReadEntity(JsonElement json) => new(Value(json, "entity").GetString() ?? "");
    /// <summary>Reads the declared scalar/vector type; invalid tags, dimensions, numeric ranges or JSON shapes throw.</summary>
    public static ResourceReference ReadResource(JsonElement json) => new(Value(json, "resource").GetString() ?? "");
    private static float[] Vector(JsonElement json, int size)
    {
        var value = Value(json, $"vec{size}");
        if (value.GetArrayLength() != size) throw new InvalidDataException("Script vector size differs.");
        var values = new float[size];
        for (int i = 0; i < size; i++) { values[i] = value[i].GetSingle(); if (!float.IsFinite(values[i])) throw new InvalidDataException("Script vector must be finite."); }
        return values;
    }
    /// <summary>Reads the declared scalar/vector type; invalid tags, dimensions, numeric ranges or JSON shapes throw.</summary>
    public static Vector2 ReadVector2(JsonElement json) { var v = Vector(json, 2); return new(v[0], v[1]); }
    /// <summary>Reads the declared scalar/vector type; invalid tags, dimensions, numeric ranges or JSON shapes throw.</summary>
    public static Vector3 ReadVector3(JsonElement json) { var v = Vector(json, 3); return new(v[0], v[1], v[2]); }
    /// <summary>Reads the declared scalar/vector type; invalid tags, dimensions, numeric ranges or JSON shapes throw.</summary>
    public static Vector4 ReadVector4(JsonElement json) { var v = Vector(json, 4); return new(v[0], v[1], v[2], v[3]); }
    /// <summary>Reads an enum integer only when its stable enum identity matches.</summary>
    public static long ReadEnum(JsonElement json, string type)
    {
        var value = Value(json, "enum");
        if (value.GetProperty("type").GetString() != type) throw new InvalidDataException("Script enum identity differs.");
        return value.GetProperty("value").GetInt64();
    }
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string Write(bool value) => Encode("bool", w => w.WriteBooleanValue(value));
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string Write(long value) => Encode("int64", w => w.WriteNumberValue(value));
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string Write(double value) => Encode("float64", w => w.WriteNumberValue(value));
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string Write(string value) => Encode("string", w => w.WriteStringValue(value));
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string Write(EntityReference value) => Encode("entity", w => w.WriteStringValue(value.Id ?? ""));
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string Write(ResourceReference value) => Encode("resource", w => w.WriteStringValue(value.Id ?? ""));
    private static string WriteVector(params float[] values) => Encode($"vec{values.Length}", w => { w.WriteStartArray(); foreach (float value in values) w.WriteNumberValue(value); w.WriteEndArray(); });
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string Write(Vector2 value) => WriteVector(value.X, value.Y);
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string Write(Vector3 value) => WriteVector(value.X, value.Y, value.Z);
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string Write(Vector4 value) => WriteVector(value.X, value.Y, value.Z, value.W);
    /// <summary>Encodes a typed value with its schema tag; nonfinite numbers are rejected by the JSON writer.</summary>
    public static string WriteEnum(long value, string type) => Encode("enum", w => { w.WriteStartObject(); w.WriteString("type", type); w.WriteNumber("value", value); w.WriteEndObject(); });
}
