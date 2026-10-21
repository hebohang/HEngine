using System.Reflection;
using System.Reflection.Emit;
using System.Numerics;

// Conservative, versioned portable API contract. Unknown calls require explicit
// review; merely compiling on Windows does not make an API usable in a mini-game.
internal static class PortableApiAudit
{
    private static readonly Dictionary<ushort, OpCode> Codes = typeof(OpCodes).GetFields()
        .Where(f => f.FieldType == typeof(OpCode)).Select(f => (OpCode)f.GetValue(null)!)
        .ToDictionary(op => unchecked((ushort)op.Value));

    private static bool IsAllowed(Type type)
    {
        if (type == typeof(IntPtr) || type == typeof(UIntPtr)) return false;
        if (type.Assembly == typeof(HEngine.ScriptBehaviour).Assembly) return true;
        if (type.Assembly != typeof(object).Assembly && type.Assembly != typeof(Vector2).Assembly && type.Assembly != typeof(BigInteger).Assembly) return false;
        string name = type.IsGenericType ? type.GetGenericTypeDefinition().FullName! : type.FullName ?? "";
        return type.IsPrimitive || type.IsEnum || name is
            "System.Object" or "System.String" or "System.Decimal" or "System.Math" or "System.MathF" or
            "System.Convert" or "System.Array" or "System.Enum" or "System.Nullable`1" or
            "System.Exception" or "System.InvalidOperationException" or "System.ArgumentException" or
            "System.ArgumentOutOfRangeException" or "System.NotSupportedException" or
            "System.Globalization.CultureInfo" or "System.Globalization.NumberFormatInfo" or
            "System.Runtime.CompilerServices.DefaultInterpolatedStringHandler" or
            "System.Runtime.CompilerServices.RuntimeHelpers" ||
            name.StartsWith("System.Numerics.Vector", StringComparison.Ordinal) ||
            name.StartsWith("System.ValueTuple", StringComparison.Ordinal) ||
            name.StartsWith("System.Collections.Generic.", StringComparison.Ordinal) ||
            name.StartsWith("System.Collections.IEnumerator", StringComparison.Ordinal) ||
            name.StartsWith("System.Action", StringComparison.Ordinal) ||
            name.StartsWith("System.Func", StringComparison.Ordinal) || name == "System.IDisposable";
    }

    internal static string[] Analyze(Type script, IReadOnlyCollection<Assembly>? modules = null)
    {
        var diagnostics = new SortedSet<string>(StringComparer.Ordinal);
        var pending = new Queue<MethodBase>();
        var visited = new HashSet<MethodBase>();
        var callbackTypes = new HashSet<Type>();
        Assembly game = script.Assembly;
        var gameAssemblies = modules ?? new[] { game };
        bool IsGame(Assembly assembly) => gameAssemblies.Contains(assembly);
        const BindingFlags members = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
        void Seed(Type type)
        {
            foreach (var method in type.GetMethods(members)) pending.Enqueue(method);
            foreach (var ctor in type.GetConstructors(members)) pending.Enqueue(ctor);
            if (type.TypeInitializer is { } initializer) pending.Enqueue(initializer);
        }
        void SeedCallbacks(Type type)
        {
            if (type.HasElementType) { SeedCallbacks(type.GetElementType()!); return; }
            if (!IsGame(type.Assembly) || !callbackTypes.Add(type)) return;
            // BCL methods may invoke virtual/interface callbacks after argument
            // types have been erased to object (formatting, sorting, equality).
            // Audit callbacks on participating game types, not every type in the
            // assembly. Interface implementations are virtual even when final.
            foreach (var callback in type.GetMethods(members))
                if (callback.IsVirtual) pending.Enqueue(callback);
            if (type.BaseType is { } parent && IsGame(parent.Assembly)) SeedCallbacks(parent);
        }
        for (Type? type = script; type is not null && IsGame(type.Assembly); type = type.BaseType) Seed(type);
        while (pending.TryDequeue(out var method))
        {
            if (!visited.Add(method)) continue;
            void Reject(string reason) => diagnostics.Add($"{method.DeclaringType?.FullName}.{method.Name}: {reason}");
            if ((method.Attributes & MethodAttributes.PinvokeImpl) != 0) { Reject("native P/Invoke is not portable; use the engine API"); continue; }
            byte[]? il = method.GetMethodBody()?.GetILAsByteArray();
            if (il is null) continue;
            try
            {
                for (int offset = 0; offset < il.Length;)
                {
                    ushort key = il[offset++];
                    if (key == 0xfe) key = (ushort)(0xfe00 | il[offset++]);
                    OpCode op = Codes[key];
                    if (op == OpCodes.Calli || op == OpCodes.Localloc || op == OpCodes.Cpblk || op == OpCodes.Initblk ||
                        op.Name?.StartsWith("ldind", StringComparison.Ordinal) == true || op.Name?.StartsWith("stind", StringComparison.Ordinal) == true)
                        Reject($"unsupported unsafe instruction {op.Name}");
                    int size = op.OperandType switch {
                        OperandType.InlineNone => 0,
                        OperandType.ShortInlineBrTarget or OperandType.ShortInlineI or OperandType.ShortInlineVar => 1,
                        OperandType.InlineVar => 2,
                        OperandType.InlineI8 or OperandType.InlineR => 8,
                        OperandType.InlineSwitch => checked(4 + BitConverter.ToInt32(il, offset) * 4),
                        _ => 4
                    };
                    if (op.OperandType == OperandType.InlineMethod)
                    {
                        MethodBase target = method.Module.ResolveMethod(BitConverter.ToInt32(il, offset),
                            method.DeclaringType?.GetGenericArguments(), method.IsGenericMethod ? method.GetGenericArguments() : null)!;
                        Type declaring = target.DeclaringType!;
                        if (target.IsConstructor) SeedCallbacks(declaring);
                        // Generic BCL helpers can call user equality/comparison/formatting
                        // implementations; inspect the participating game types as well.
                        foreach (var argument in (target.IsGenericMethod ? target.GetGenericArguments() : Type.EmptyTypes).Concat(declaring.GetGenericArguments()))
                            if (argument.Assembly == game) Seed(argument);
                        if (target.IsVirtual && declaring.IsInterface)
                            foreach (var candidate in game.GetTypes().Where(declaring.IsAssignableFrom)) Seed(candidate);
                        if (IsGame(declaring.Assembly))
                        {
                            pending.Enqueue(target);
                            if (declaring.TypeInitializer is { } initializer) pending.Enqueue(initializer);
                            // Include possible implementations of game-owned virtual/interface calls.
                            if (target.IsVirtual)
                                foreach (var candidate in gameAssemblies.SelectMany(item => item.GetTypes()).Where(declaring.IsAssignableFrom)) Seed(candidate);
                        }
                        else if (declaring.Assembly == typeof(HEngine.ScriptBehaviour).Assembly &&
                            (declaring == typeof(HEngine.PrefabLoad) || declaring == typeof(HEngine.InputActions) ||
                             declaring == typeof(HEngine.Physics) || declaring == typeof(HEngine.Animator) || declaring == typeof(HEngine.Camera) ||
                             declaring == typeof(HEngine.Navigation) || declaring == typeof(HEngine.AudioPlayback) || declaring == typeof(HEngine.VfxPlayback) ||
                             declaring == typeof(HEngine.UiDocument) ||
                             declaring == typeof(HEngine.SceneFlow) ||
                             (declaring == typeof(HEngine.Entity) && target.Name is "Create" or "Instantiate" or "get_Parent" or "set_Parent" or "GetChildrenSnapshot") ||
                             ((declaring == typeof(HEngine.IEngineApi) || declaring == typeof(HEngine.EngineApi)) && target.Name is "CreateEntity" or "Parent" or "Children" or "PrefabRequest" or "ReadAction" or "SetInputContext" or "RebindInput" or "ReadInputMap" or "WriteInputMap")))
                            Reject($"API {declaring.FullName}.{target.Name} requires the desktop scene host; unsupported on Web/WeChat");
                        else if (!IsAllowed(declaring)) Reject($"unverified portable API {declaring.FullName}.{target.Name}; use the engine API or extend the tested portable contract");
                    }
                    // A static field read also executes its declaring type initializer.
                    if (op.OperandType == OperandType.InlineField)
                    {
                        var field = method.Module.ResolveField(BitConverter.ToInt32(il, offset), method.DeclaringType?.GetGenericArguments(),
                            method.IsGenericMethod ? method.GetGenericArguments() : null)!;
                        SeedCallbacks(field.FieldType);
                        if (field.DeclaringType is { } owner && IsGame(owner.Assembly) && owner.TypeInitializer is { } initializer) pending.Enqueue(initializer);
                        else if (field.DeclaringType is { } declaring && !IsGame(declaring.Assembly) && !IsAllowed(declaring))
                            Reject($"unverified portable field {declaring.FullName}.{field.Name}");
                    }
                    if (op.OperandType == OperandType.InlineType)
                        SeedCallbacks(method.Module.ResolveType(BitConverter.ToInt32(il, offset), method.DeclaringType?.GetGenericArguments(),
                            method.IsGenericMethod ? method.GetGenericArguments() : null));
                    offset = checked(offset + size);
                    if (offset > il.Length) throw new InvalidOperationException("truncated IL operand");
                }
            }
            catch (Exception error) { Reject("could not audit IL: " + error.Message); }
        }
        return diagnostics.ToArray();
    }
}
