using System.Globalization;
using System.Reflection;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using System.Xml.Linq;

if (args.Length != 6 || args[0] != "--assembly" || args[2] != "--xml" || args[4] != "--output")
    throw new ArgumentException("Usage: --assembly <ScriptCore.dll> --xml <ScriptCore.xml> --output <json>");
string assemblyPath = Path.GetFullPath(args[1]), xmlPath = Path.GetFullPath(args[3]), output = Path.GetFullPath(args[5]);
var docs = XDocument.Load(xmlPath).Descendants("member").Select(e => e.Attribute("name")!.Value).ToHashSet(StringComparer.Ordinal);
var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(assemblyPath);
var declarations = new SortedDictionary<string, string>(StringComparer.Ordinal);
const BindingFlags Flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;

foreach (var type in assembly.GetExportedTypes())
{
    string owner = type.FullName!.Replace('+', '.');
    bool record = type.GetMethods(Flags).Any(m => m.Name == "<Clone>$") ||
        (type.IsValueType && type.GetMethods(Flags).Any(m => m.Name == "PrintMembers" &&
            m.CustomAttributes.Any(a => a.AttributeType.Name == "CompilerGeneratedAttribute")));
    string kind = type.IsEnum ? "enum" : type.IsInterface ? "interface" : type.IsValueType ?
        (type.CustomAttributes.Any(a => a.AttributeType.Name == "IsReadOnlyAttribute") ? "readonly " : "") + (record ? "record struct" : "struct") :
        type.IsAbstract && type.IsSealed ? "static class" : type.IsAbstract ? "abstract class" : type.IsSealed ? "sealed class" : "class";
    if (record && !type.IsValueType) kind = kind.Replace("class", "record class");
    Add("T:" + owner, $"public {kind} {Readable(type)}");
    foreach (var field in type.GetFields(Flags))
    {
        if (field.IsSpecialName) continue;
        Add("F:" + owner + "." + field.Name, type.IsEnum ? field.Name + " = " + Convert.ToString(field.GetRawConstantValue(), CultureInfo.InvariantCulture) :
            $"public {(field.IsLiteral ? "const " : field.IsStatic ? "static " : "")}{(field.IsInitOnly ? "readonly " : "")}{Readable(field.FieldType)} {field.Name}", field.IsPublic);
    }
    foreach (var property in type.GetProperties(Flags))
    {
        bool init = property.SetMethod?.ReturnParameter.GetRequiredCustomModifiers().Any(t => t.Name == "IsExternalInit") == true;
        string accessors = (property.GetMethod?.IsPublic == true ? "get; " : "") + (property.SetMethod?.IsPublic == true ? init ? "init; " : "set; " : "");
        Add("P:" + owner + "." + property.Name + ParametersId(property.GetIndexParameters()),
            $"public {((property.GetMethod ?? property.SetMethod)!.IsStatic ? "static " : "")}{Readable(property.PropertyType)} {property.Name} {{ {accessors}}}",
            property.GetMethod?.IsPublic == true || property.SetMethod?.IsPublic == true);
    }
    foreach (var constructor in type.GetConstructors(Flags))
        Add("M:" + owner + ".#ctor" + ParametersId(constructor.GetParameters()),
            "public " + Readable(type) + "(" + string.Join(", ", constructor.GetParameters().Select(Parameter)) + ")", constructor.IsPublic);
    foreach (var method in type.GetMethods(Flags))
    {
        if (method.IsSpecialName) continue;
        string genericId = method.IsGenericMethod ? "``" + method.GetGenericArguments().Length : "";
        string generic = method.IsGenericMethod ? "<" + string.Join(", ", method.GetGenericArguments().Select(t => t.Name)) + ">" : "";
        Add("M:" + owner + "." + method.Name + genericId + ParametersId(method.GetParameters()),
            $"{(method.IsFamily ? "protected" : "public")} {Modifiers(method)}{Readable(method.ReturnType)} {method.Name}{generic}({string.Join(", ", method.GetParameters().Select(Parameter))}){Constraints(method)}",
            record && method.IsPublic && method.Name == "Deconstruct");
    }
}
var result = new {
    schema = 1,
    assemblySha256 = Hash(assemblyPath), xmlSha256 = Hash(xmlPath),
    declarations
};
Directory.CreateDirectory(Path.GetDirectoryName(output)!);
string json = JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true }) + "\n";
if (!File.Exists(output) || File.ReadAllText(output) != json) File.WriteAllText(output, json);
Console.WriteLine($"C# reference signatures: {declarations.Count}");

void Add(string id, string declaration, bool publicData = false) { if (publicData || docs.Contains(id)) declarations[id] = declaration; }
static string Hash(string path) => Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)));
static string Modifiers(MethodInfo method) => (method.IsStatic ? "static " : "") + (method.IsAbstract ? "abstract " :
    method.IsVirtual && !method.IsFinal ? method.GetBaseDefinition() != method ? "override " : "virtual " : "");
static string Constraints(MethodInfo method)
{
    if (!method.IsGenericMethod) return "";
    string result = "";
    foreach (var parameter in method.GetGenericArguments())
    {
        var constraints = new List<string>();
        var attributes = parameter.GenericParameterAttributes;
        bool value = attributes.HasFlag(GenericParameterAttributes.NotNullableValueTypeConstraint);
        if (value) constraints.Add("struct");
        else if (attributes.HasFlag(GenericParameterAttributes.ReferenceTypeConstraint)) constraints.Add("class");
        constraints.AddRange(parameter.GetGenericParameterConstraints().Where(t => t != typeof(ValueType)).Select(Readable));
        if (!value && attributes.HasFlag(GenericParameterAttributes.DefaultConstructorConstraint)) constraints.Add("new()");
        if (constraints.Count > 0) result += " where " + parameter.Name + " : " + string.Join(", ", constraints);
    }
    return result;
}
static string ParametersId(ParameterInfo[] parameters) => parameters.Length == 0 ? "" : "(" + string.Join(",", parameters.Select(p => XmlType(p.ParameterType))) + ")";
static string XmlType(Type type)
{
    if (type.IsByRef) return XmlType(type.GetElementType()!) + "@";
    if (type.IsPointer) return XmlType(type.GetElementType()!) + "*";
    if (type.IsArray) return XmlType(type.GetElementType()!) + (type.GetArrayRank() == 1 ? "[]" : "[" + string.Join(",", Enumerable.Repeat("0:", type.GetArrayRank())) + "]");
    if (type.IsGenericParameter) return (type.DeclaringMethod is null ? "`" : "``") + type.GenericParameterPosition;
    if (!type.IsGenericType) return type.FullName!.Replace('+', '.');
    string name = type.GetGenericTypeDefinition().FullName!.Replace('+', '.');
    name = System.Text.RegularExpressions.Regex.Replace(name, "`[0-9]+", "");
    return name + "{" + string.Join(",", type.GetGenericArguments().Select(XmlType)) + "}";
}
static string Readable(Type type)
{
    if (type.IsByRef) return "ref " + Readable(type.GetElementType()!);
    if (type.IsPointer) return Readable(type.GetElementType()!) + "*";
    if (type.IsArray) return Readable(type.GetElementType()!) + "[" + new string(',', type.GetArrayRank() - 1) + "]";
    if (type.IsGenericParameter) return type.Name;
    if (type.IsGenericType)
    {
        if (type.GetGenericTypeDefinition() == typeof(Nullable<>)) return Readable(type.GetGenericArguments()[0]) + "?";
        return type.Name.Split('`')[0] + "<" + string.Join(", ", type.GetGenericArguments().Select(Readable)) + ">";
    }
    return type.FullName switch {
        "System.Void" => "void", "System.Boolean" => "bool", "System.Int32" => "int", "System.UInt32" => "uint",
        "System.Int64" => "long", "System.UInt64" => "ulong", "System.Single" => "float", "System.Double" => "double",
        "System.String" => "string", "System.Object" => "object", "System.IntPtr" => "nint", "System.UIntPtr" => "nuint", _ => type.Name };
}
static string Parameter(ParameterInfo parameter)
{
    Type type = parameter.ParameterType;
    string prefix = type.IsByRef ? parameter.IsOut ? "out " : parameter.IsIn ? "in " : "ref " : "";
    string result = prefix + Readable(type.IsByRef ? type.GetElementType()! : type) + " " + parameter.Name;
    if (!parameter.HasDefaultValue) return result;
    object? value = parameter.DefaultValue;
    string text = value switch {
        null => type.IsValueType && Nullable.GetUnderlyingType(type) is null ? "default" : "null", string s => JsonSerializer.Serialize(s), bool b => b ? "true" : "false",
        float f => f.ToString("R", CultureInfo.InvariantCulture) + "f",
        double d => d.ToString("R", CultureInfo.InvariantCulture),
        _ when type.IsEnum => Readable(type) + "." + Enum.GetName(type, value),
        _ => Convert.ToString(value, CultureInfo.InvariantCulture) ?? "default" };
    return result + " = " + text;
}
