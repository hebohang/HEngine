using System.Numerics;
using System.Reflection;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using HEngine;

if (args.Length == 2 && args[0] == "--resolve-order")
{
    ExecutionOrder.Resolve(args[1]);
    return;
}

if (args.Length is not (3 or 4 or 5) || args[2].Length != 64 || args[2].Any(c => !char.IsAsciiHexDigit(c)))
    throw new ArgumentException("RegistryGenerator <GameScripts.dll> <output-directory> <gameplay-source-sha256>");
string assemblyPath = Path.GetFullPath(args[0]), output = Path.GetFullPath(args[1]);
string registryClass = args.Length >= 4 ? args[3] : "GameScriptRegistry";
if (!System.Text.RegularExpressions.Regex.IsMatch(registryClass, "^[A-Za-z_][A-Za-z0-9_]*$")) throw new ArgumentException("Invalid generated registry name.");
Directory.CreateDirectory(output);
AssemblyLoadContext.Default.Resolving += (_, name) =>
{
    string candidate = Path.Combine(Path.GetDirectoryName(assemblyPath)!, name.Name + ".dll");
    return File.Exists(candidate) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(candidate) : null;
};
Assembly assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(assemblyPath);
var moduleAssemblies = new HashSet<Assembly> { assembly };
if (args.Length == 5)
{
    using var moduleManifest = JsonDocument.Parse(File.ReadAllText(args[4]));
    foreach (var module in moduleManifest.RootElement.GetProperty("modules").EnumerateArray())
    {
        string path = module.GetProperty("dll").GetString()!;
        if (Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path))) != module.GetProperty("sha256").GetString())
            throw new InvalidDataException("Module changed before portable API audit: " + path);
        moduleAssemblies.Add(AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(path)));
    }
}
string Quote(string text) => JsonSerializer.Serialize(text);
var valueSchemaIds = new Dictionary<string, Type>(StringComparer.Ordinal);
foreach (var candidate in moduleAssemblies.SelectMany(module => module.GetTypes()))
{
    var structure = candidate.GetCustomAttribute<ScriptStructAttribute>();
    if (structure is null && !candidate.IsEnum) continue;
    string id = structure is null ? "enum:" + candidate.FullName : "struct:" + structure.Id;
    if (structure is not null && (string.IsNullOrWhiteSpace(structure.Id) || structure.Id.Length > 128))
        throw new InvalidOperationException("Invalid structure schema ID on " + candidate.FullName);
    if (valueSchemaIds.TryGetValue(id, out var previous) && previous != candidate)
        throw new InvalidOperationException($"Duplicate value schema identity {id}: {previous.AssemblyQualifiedName}; {candidate.AssemblyQualifiedName}");
    valueSchemaIds[id] = candidate;
}
void Publish(string name, string content)
{
    string path = Path.Combine(output, name);
    if (!File.Exists(path) || File.ReadAllText(path) != content) File.WriteAllText(path, content, new UTF8Encoding(false));
}
string Qualified(Type type) => "global::" + (type.FullName ?? throw new InvalidOperationException("Anonymous script type.")).Replace('+', '.');
AggregateGenerator aggregates = null!;
aggregates = new((type, input) => ReadExpression(type, type.FullName ?? "", input),
    (type, input) => WriteExpression(type, input, type.FullName ?? ""),
    (type, value) => DefaultJson(type, value, type.FullName ?? ""), Qualified);
string ReadExpression(Type type, string enumId, string input = "value")
{
    if (type == typeof(bool)) return $"ScriptValueCodec.ReadBool({input})";
    if (type == typeof(int)) return $"checked((int)ScriptValueCodec.ReadInt64({input}))";
    if (type == typeof(long)) return $"ScriptValueCodec.ReadInt64({input})";
    if (type == typeof(float)) return $"checked((float)ScriptValueCodec.ReadDouble({input}))";
    if (type == typeof(double)) return $"ScriptValueCodec.ReadDouble({input})";
    if (type == typeof(string)) return $"ScriptValueCodec.ReadString({input})";
    if (type == typeof(Vector2)) return $"ScriptValueCodec.ReadVector2({input})";
    if (type == typeof(Vector3)) return $"ScriptValueCodec.ReadVector3({input})";
    if (type == typeof(Vector4)) return $"ScriptValueCodec.ReadVector4({input})";
    if (type == typeof(EntityReference)) return $"ScriptValueCodec.ReadEntity({input})";
    if (type == typeof(ResourceReference)) return $"ScriptValueCodec.ReadResource({input})";
    if (type.IsEnum) return $"({Qualified(type)})ScriptValueCodec.ReadEnum({input}, {Quote(enumId)})";
    if (AggregateGenerator.IsAggregate(type)) return aggregates.Read(type, input);
    throw new InvalidOperationException($"Unsupported exposed field type: {type.FullName}");
}
string WriteExpression(Type type, string member, string enumId)
{
    if (AggregateGenerator.IsAggregate(type)) return aggregates.Write(type, member);
    if (type.IsEnum) return $"ScriptValueCodec.WriteEnum(checked((long){member}), {Quote(enumId)})";
    if (type == typeof(int)) member = $"(long){member}";
    if (type == typeof(float)) member = $"(double){member}";
    return $"ScriptValueCodec.Write({member})";
}
string DefaultJson(Type type, object? value, string enumId)
{
    if (AggregateGenerator.IsAggregate(type)) return aggregates.Default(type, value);
    if (type.IsEnum) return ScriptValueCodec.WriteEnum(Convert.ToInt64(value), enumId);
    return value switch {
        bool v => ScriptValueCodec.Write(v), int v => ScriptValueCodec.Write((long)v), long v => ScriptValueCodec.Write(v),
        float v => ScriptValueCodec.Write((double)v), double v => ScriptValueCodec.Write(v), string v => ScriptValueCodec.Write(v),
        Vector2 v => ScriptValueCodec.Write(v), Vector3 v => ScriptValueCodec.Write(v), Vector4 v => ScriptValueCodec.Write(v),
        EntityReference v => ScriptValueCodec.Write(v), ResourceReference v => ScriptValueCodec.Write(v),
        null when type == typeof(string) => ScriptValueCodec.Write(""),
        _ => throw new InvalidOperationException($"Unsupported field default: {type}") };
}
var registrations = new StringBuilder();
var metadata = new List<object>();
var ids = new HashSet<string>(StringComparer.Ordinal);
foreach (Type type in assembly.GetTypes().OrderBy(t => t.FullName, StringComparer.Ordinal))
{
    var attribute = type.GetCustomAttribute<ScriptTypeAttribute>();
    if (attribute is null)
    {
        if (!type.IsAbstract && typeof(ScriptBehaviour).IsAssignableFrom(type)) throw new InvalidOperationException($"Script {type.FullName} requires [ScriptType] with a stable ID.");
        continue;
    }
    if (!type.IsPublic || type.IsAbstract || type.ContainsGenericParameters || !typeof(ScriptBehaviour).IsAssignableFrom(type) || type.GetConstructor(Type.EmptyTypes) is null)
        throw new InvalidOperationException($"{type.FullName}: script must be a public concrete ScriptBehaviour with a public parameterless constructor.");
    if (string.IsNullOrWhiteSpace(attribute.Id) || !ids.Add(attribute.Id)) throw new InvalidOperationException($"Duplicate or empty script ID: {attribute.Id}");
    object defaults = Activator.CreateInstance(type)!;
    var dependencies = new SortedSet<string>(StringComparer.Ordinal);
    void AddDependency(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || path.Contains('\\') || path.Contains(':') ||
            path.StartsWith('/') || path.Split('/').Any(part => part is "" or "." or ".."))
            throw new InvalidOperationException($"{type.FullName}: script dependency must be a project-relative resource path: {path}");
        dependencies.Add(path);
    }
    foreach (var dependency in type.GetCustomAttributes<ScriptDependencyAttribute>(inherit: true)) AddDependency(dependency.Path);
    string qualified = Qualified(type);
    registrations.AppendLine($"new({Quote(attribute.Id)}, static () => new {qualified}(), new ScriptFieldRegistration[] {{");
    var fields = new List<object>();
    var fieldIds = new HashSet<string>(StringComparer.Ordinal);
    foreach (FieldInfo field in type.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance).OrderBy(f => f.Name, StringComparer.Ordinal))
    {
        var exposed = field.GetCustomAttribute<ScriptFieldAttribute>();
        if (exposed is null) continue;
        if (!field.IsPublic || field.IsStatic || field.IsInitOnly) throw new InvalidOperationException($"{type.FullName}.{field.Name}: exposed field must be public, mutable and nonstatic.");
        foreach (string id in exposed.FormerIds.Prepend(exposed.Id))
            if (string.IsNullOrWhiteSpace(id) || id.Length > 256 || !fieldIds.Add(id)) throw new InvalidOperationException($"{type.FullName}.{field.Name}: duplicate, ambiguous or invalid field ID {id}.");
        string enumId = field.FieldType.FullName ?? "";
        string read;
        try { read = ReadExpression(field.FieldType, enumId); }
        catch (Exception error) { throw new InvalidOperationException($"{type.FullName}.{field.Name}: {error.Message}"); }
        string member = $"(({qualified})script).@{field.Name}";
        registrations.AppendLine($"new({Quote(exposed.Id)}, static (script, value) => {member} = {read}, static script => {WriteExpression(field.FieldType, member, enumId)}),");
        object? defaultValue = field.GetValue(defaults);
        var defaultJson = JsonDocument.Parse(DefaultJson(field.FieldType, defaultValue, enumId)).RootElement;
        void CollectResources(JsonElement value)
        {
            string kind = value.GetProperty("type").GetString()!;
            var data = value.GetProperty("value");
            if (kind == "resource" && !string.IsNullOrEmpty(data.GetString())) AddDependency(data.GetString()!);
            else if (kind == "struct") foreach (var child in data.GetProperty("fields").EnumerateObject()) CollectResources(child.Value);
            else if (kind is "list" or "array")
            {
                foreach (var child in data.GetProperty("items").EnumerateArray()) CollectResources(child);
                CollectResources(data.GetProperty("prototype"));
            }
        }
        CollectResources(defaultJson);
        if (!double.IsFinite(exposed.Min) || !double.IsFinite(exposed.Max) || exposed.Min > exposed.Max ||
            !double.IsFinite(exposed.Space) || exposed.Space < 0 || exposed.Space > 256)
            throw new InvalidOperationException($"{type.FullName}.{field.Name}: invalid inspector range/spacing.");
        if (exposed.Color && field.FieldType != typeof(Vector4)) throw new InvalidOperationException("Color requires a Vector4 field.");
        fields.Add(new { id = exposed.Id, name = string.IsNullOrEmpty(exposed.DisplayName) ? field.Name : exposed.DisplayName,
            formerIds = exposed.FormerIds, defaultValue = defaultJson,
            ui = new { tooltip = exposed.Tooltip, header = exposed.Header, space = exposed.Space, readOnly = exposed.ReadOnly,
                hidden = exposed.HideInInspector, multiline = exposed.Multiline, color = exposed.Color, hasRange = exposed.HasRange,
                min = exposed.Min, max = exposed.Max, resourceType = exposed.ResourceType, allowNull = exposed.AllowNull },
            enumValues = field.FieldType.IsEnum ? Enum.GetNames(field.FieldType).Select(name => new { name, value = Convert.ToInt64(Enum.Parse(field.FieldType, name)) }).ToArray() : [] });
    }
    foreach (PropertyInfo property in type.GetProperties())
        if (property.GetCustomAttributes().Any(a => a.GetType().Name == "ScriptFieldAttribute")) throw new InvalidOperationException($"{type.FullName}.{property.Name}: exposed properties are not supported.");
    registrations.AppendLine("}),");
    metadata.Add(new { id = attribute.Id, resource = Path.GetFileName(assemblyPath), dependencies = dependencies.ToArray(), fields,
        executionOrder = type.GetCustomAttribute<ScriptExecutionOrderAttribute>()?.Order ?? 0,
        executeBefore = type.GetCustomAttribute<ScriptExecutionOrderAttribute>()?.Before ?? [],
        executeAfter = type.GetCustomAttribute<ScriptExecutionOrderAttribute>()?.After ?? [],
        portableApiContract = 2, portableDiagnostics = PortableApiAudit.Analyze(type, moduleAssemblies) });
}
string identity = Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(assemblyPath)));
string metadataJson = JsonSerializer.Serialize(new { version = 1, identity, gameplaySourceIdentity = args[2], types = metadata });
Publish("GameScripts.Registry.g.cs", $"// Generated by RegistryGenerator. Do not edit.\nusing HEngine;\nusing System.Linq;\nnamespace HEngine.Generated;\npublic static class {registryClass} {{\npublic const string Metadata = {Quote(metadataJson)};\npublic static ScriptRegistration[] Create() => new ScriptRegistration[] {{\n{registrations}\n}};\n{aggregates.Methods}\n}}\n");
Publish("GameScripts.metadata.json", metadataJson);
Console.WriteLine($"C# registry: types={ids.Count}; assemblySha256={identity}");
