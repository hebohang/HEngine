using System.Collections;
using System.Reflection;
using System.Text;
using System.Text.Json;
using HEngine;

// Only this build-time generator inspects value structure fields.
sealed class AggregateGenerator(Func<Type, string, string> read, Func<Type, string, string> write,
    Func<Type, object?, string> defaults, Func<Type, string> qualified)
{
    private readonly Dictionary<Type, int> mMethods = new();
    private readonly HashSet<Type> mBuilding = new();
    private readonly HashSet<Type> mDefaults = new();
    public readonly StringBuilder Methods = new();
    private int mParameter;
    private static string Q(string text) => JsonSerializer.Serialize(text);
    public static bool IsAggregate(Type type) => type.IsArray ||
        (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(List<>)) || type.IsDefined(typeof(ScriptStructAttribute));
    private static Type? Element(Type type) => type.IsArray ? type.GetElementType() :
        type.IsGenericType && type.GetGenericTypeDefinition() == typeof(List<>) ? type.GenericTypeArguments[0] : null;
    private static object? Empty(Type type) => type == typeof(string) ? "" : type.IsValueType ? Activator.CreateInstance(type) : null;
    private static string Identity(Type type, int depth = 0)
    {
        if (depth > 4 || type.IsPointer || type.IsByRef) throw new InvalidOperationException("Unsupported recursive/deep aggregate type.");
        if (Element(type) is { } item) return (type.IsArray ? "array:" : "list:") + Identity(item, depth + 1);
        if (type.GetCustomAttribute<ScriptStructAttribute>() is { } attribute)
        {
            if (!type.IsValueType || type.IsEnum || type.IsGenericType || !type.IsVisible || attribute.Id.Length is 0 or > 128)
                throw new InvalidOperationException("ScriptStruct requires a public nongeneric value type and a stable ID.");
            return "struct:" + attribute.Id;
        }
        return type.FullName ?? throw new InvalidOperationException("Anonymous aggregate member.");
    }
    private static (FieldInfo Field, ScriptFieldAttribute Attribute)[] Members(Type type)
    {
        var result = type.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static)
            .Where(f => f.IsDefined(typeof(ScriptFieldAttribute))).OrderBy(f => f.Name, StringComparer.Ordinal)
            .Select(f => (Field: f, Attribute: f.GetCustomAttribute<ScriptFieldAttribute>()!)).ToArray();
        var ids = new HashSet<string>(StringComparer.Ordinal);
        if (result.Length > 256) throw new InvalidOperationException("Structure exceeds 256 fields.");
        foreach (var member in result)
        {
            if (!member.Field.IsPublic || member.Field.IsStatic || member.Field.IsInitOnly) throw new InvalidOperationException("Structure fields must be public, mutable and nonstatic.");
            foreach (string id in member.Attribute.FormerIds.Prepend(member.Attribute.Id))
                if (id.Length is 0 or > 256 || !ids.Add(id)) throw new InvalidOperationException("Ambiguous structure field ID: " + id);
        }
        return result;
    }
    public string Read(Type type, string input)
    {
        string identity = Identity(type);
        if (Element(type) is { } element)
        {
            if (type.IsArray && type.GetArrayRank() != 1) throw new InvalidOperationException("Only one-dimensional arrays are supported.");
            string parameter = "item" + mParameter++;
            return $"ScriptAggregateCodec.ReadSequence({input}, {Q(type.IsArray ? "array" : "list")}, {Q(identity)}, static {parameter} => {read(element, parameter)})" + (type.IsArray ? "" : ".ToList()");
        }
        return $"ReadAggregate{Build(type)}({input})";
    }
    public string Write(Type type, string input)
    {
        string identity = Identity(type);
        if (Element(type) is { } element)
        {
            string parameter = "item" + mParameter++;
            return $"ScriptAggregateCodec.WriteSequence({Q(type.IsArray ? "array" : "list")}, {Q(identity)}, {input}, static {parameter} => {write(element, parameter)}, {Q(defaults(element, Empty(element)))})";
        }
        return $"WriteAggregate{Build(type)}({input})";
    }
    private int Build(Type type)
    {
        if (mBuilding.Contains(type)) throw new InvalidOperationException("Recursive serializable structure: " + type.FullName);
        if (mMethods.TryGetValue(type, out int existing)) return existing;
        if (mBuilding.Count >= 4) throw new InvalidOperationException("Structure nesting exceeds 4.");
        mBuilding.Add(type);
        int index = mMethods.Count; mMethods.Add(type, index);
        var members = Members(type); string name = qualified(type), identity = Identity(type);
        var reader = new StringBuilder($"private static {name} ReadAggregate{index}(global::System.Text.Json.JsonElement value) {{ var fields = ScriptAggregateCodec.ReadStruct(value, {Q(identity)}); var result = new {name}();\n");
        var writes = new List<string>();
        for (int i = 0; i < members.Length; ++i)
        {
            var (field, attribute) = members[i];
            string aliases = "new string[] {" + string.Join(",", attribute.FormerIds.Select(Q)) + "}";
            reader.AppendLine($"if (ScriptAggregateCodec.TryMember(fields, {Q(attribute.Id)}, {aliases}, out var member{i})) result.@{field.Name} = {read(field.FieldType, "member" + i)};");
            writes.Add($"new({Q(attribute.Id)}, {write(field.FieldType, "value.@" + field.Name)})");
        }
        reader.AppendLine("return result; }");
        Methods.Append(reader).AppendLine($"private static string WriteAggregate{index}({name} value) => ScriptAggregateCodec.WriteStruct({Q(identity)}, new global::System.Collections.Generic.KeyValuePair<string,string>[] {{ {string.Join(",", writes)} }});");
        mBuilding.Remove(type);
        return index;
    }
    public string Default(Type type, object? value)
    {
        if (mDefaults.Count >= 4 || !mDefaults.Add(type)) throw new InvalidOperationException("Recursive/deep aggregate default.");
        try
        {
            if (Element(type) is { } element)
            {
                var items = value is IEnumerable sequence ? sequence.Cast<object?>().ToArray() : [];
                if (items.Length > 256) throw new InvalidOperationException("Sequence default exceeds 256 items.");
                return ScriptAggregateCodec.WriteSequence(type.IsArray ? "array" : "list", Identity(type), items,
                    item => defaults(element, item), defaults(element, Empty(element)));
            }
            return ScriptAggregateCodec.WriteStruct(Identity(type), Members(type).Select(member => new KeyValuePair<string,string>(member.Attribute.Id,
                defaults(member.Field.FieldType, member.Field.GetValue(value ?? Activator.CreateInstance(type))))).ToArray());
        }
        finally { mDefaults.Remove(type); }
    }
}
