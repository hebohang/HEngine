using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using System.Security.Cryptography;
using System.Reflection;
using HEngine.EditorScripting;
using HEngine.Scripting.Interop;

unsafe class Program
{
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Engine(Request* request)
    {
        request->Result = 1;
        if (request->Output != 0 && request->OutputCapacity > 0)
        {
            *(byte*)request->Output = (byte)'0';
            request->OutputLength = 1;
        }
        return 0;
    }

    private static (int Status, ulong Result, string Output) Call(ManagedOperation operation, string input = "", string name = "", ulong instance = 0, uint callback = 0)
    {
        byte[] data = Encoding.UTF8.GetBytes(input), label = Encoding.UTF8.GetBytes(name), output = new byte[1024 * 1024];
        fixed (byte* inputPtr = data, namePtr = label, outputPtr = output)
        {
            Request request = new() { Version = 1, Operation = (uint)operation, Input = (nint)inputPtr, InputLength = data.Length,
                Name = (nint)namePtr, NameLength = label.Length, Output = (nint)outputPtr, OutputCapacity = output.Length,
                Instance = instance, Entity = 1, Callback = callback, HostEntry = (nint)(delegate* unmanaged[Cdecl]<Request*, int>)&Engine };
            int status = NativeEntry.DispatchManaged((nint)(&request));
            return (status, request.Result, Encoding.UTF8.GetString(output, 0, request.OutputLength));
        }
    }
    private static void Require(bool condition, string message) { if (!condition) throw new Exception(message); }
    private static void Pass(ManagedOperation operation, string input = "")
    {
        var result = Call(operation, input);
        Require(result.Status == 0, operation + ": " + result.Output);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void TrackPendingTask(ulong handle, Task task)
    {
        // Test-only root injection; production registration never uses this reflective path.
        var context = (System.Runtime.Loader.AssemblyLoadContext)typeof(NativeEntry).GetField("sContext", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;
        var runtime = context.Assemblies.Single(assembly => assembly.GetName().Name == "HEngine.ScriptRuntime").GetType("HEngine.Scripting.ScriptRuntime")!;
        var instances = (System.Collections.IDictionary)runtime.GetField("sInstances", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;
        object instance = instances[handle]!;
        object script = instance.GetType().GetProperty("Script")!.GetValue(instance)!;
        script.GetType().GetMethod("TrackTask")!.Invoke(script, [task, "reload cancellation timeout fixture"]);
    }
    static void Main(string[] args)
    {
        string directory = Path.GetFullPath(args[0]);
        string type = args.Length > 1 ? args[1] : "sample.touch";
        Pass(ManagedOperation.Reload, directory);
        string metadata = Call(ManagedOperation.Describe).Output;
        for (int round = 0; round < 12; ++round)
        {
            Console.WriteLine($"Reload round {round + 1}/12 ({type})");
            Pass(ManagedOperation.Initialize);
            var created = Call(ManagedOperation.Create, name: type);
            Require(created.Status == 0 && created.Result != 0, created.Output);
            if (type == "test.json-reload")
                Require(Call(ManagedOperation.Invoke, instance: created.Result, callback: (uint)ScriptLifecycle.Initialize).Status == 0,
                    "JSON request fixture failed");
            var liveReload = Call(ManagedOperation.Reload);
            Require(liveReload.Status != 0 && liveReload.Output.Contains("roots remain"), "Live root reload was accepted");
            Require(Call(ManagedOperation.ReadFields, instance: created.Result).Status == 0, "Rejected reload damaged live instances");
            Require(Call(ManagedOperation.Reload, directory + "-missing").Status != 0, "Missing generation accepted");
            Require(Call(ManagedOperation.Describe).Output == metadata, "Failed validation changed active metadata");
            Require(Call(ManagedOperation.Release, instance: created.Result).Status == 0, "Release failed");
            Pass(ManagedOperation.Reload);
            Require(Call(ManagedOperation.Describe).Output == metadata, "Reload changed metadata identity");
        }
        if (type == "test.json-reload")
        {
            Console.WriteLine("PASS: 12 JSON scene-service Play/Stop/reloads; live-root refusal; candidate preservation");
            return;
        }
        Pass(ManagedOperation.Initialize);
        var pendingInstance = Call(ManagedOperation.Create, name: type);
        Require(pendingInstance.Status == 0, pendingInstance.Output);
        var pendingTask = new TaskCompletionSource();
        TrackPendingTask(pendingInstance.Result, pendingTask.Task);
        Require(Call(ManagedOperation.Release, instance: pendingInstance.Result).Status == 0, "Pending task prevented instance release");
        var timeout = Call(ManagedOperation.Reload);
        Require(timeout.Status != 0 && timeout.Output.Contains("timeout (250 ms)") && timeout.Output.Contains("reload cancellation timeout fixture"), "Pending task unload did not time out with origin: " + timeout.Output);
        Require(Call(ManagedOperation.Describe).Output == metadata, "Cancellation timeout destroyed the valid generation");
        pendingTask.SetCanceled();
        Pass(ManagedOperation.Reload);
        Pass(ManagedOperation.Initialize);
        if (type == "sample.touch")
        {
            var validated = Call(ManagedOperation.EditFields, "{\"schema\":1,\"fields\":{\"speed\":{\"type\":\"float64\",\"value\":-12}}}", "sample.workflow", callback: (uint)ScriptLifecycle.OnValidate);
            Require(validated.Status == 0 && JsonNode.Parse(validated.Output)!["fields"]!["speed"]!["value"]!.GetValue<double>() == 0, "OnValidate failed: " + validated.Output);
            var reset = Call(ManagedOperation.EditFields, "{\"schema\":1,\"fields\":{}}", "sample.workflow", callback: (uint)ScriptLifecycle.Reset);
            Require(reset.Status == 0 && JsonNode.Parse(reset.Output)!["fields"]!["speed"]!["value"]!.GetValue<double>() == 2, "Reset failed: " + reset.Output);
            var fields = JsonNode.Parse(reset.Output)!;
            var waypoint = fields["fields"]!["waypoint"]!["value"]!["fields"]!.AsObject();
            waypoint["name"] = waypoint["label"]!.DeepClone();
            waypoint["name"]!["value"] = "Former ID migrated";
            waypoint.Remove("label");
            var aggregate = Call(ManagedOperation.EditFields, fields.ToJsonString(), "sample.workflow", callback: (uint)ScriptLifecycle.OnValidate);
            Require(aggregate.Status == 0, "Aggregate restore failed: " + aggregate.Output);
            var restored = JsonNode.Parse(aggregate.Output)!;
            Require(restored["schema"]!.GetValue<int>() == 2 && restored["fields"]!["waypoint"]!["value"]!["fields"]!["label"]!["value"]!.GetValue<string>() == "Former ID migrated", "Nested FormerIds migration failed");
            Require(restored["fields"]!["waypoints"]!["value"]!["items"]!.AsArray().Count == 1 && restored["fields"]!["durations"]!["value"]!["items"]!.AsArray().Count == 2, "Collection defaults lost");
        }
        string bad = Path.Combine(Path.GetTempPath(), "HEngine 回收 mismatch " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(bad);
        foreach (string file in Directory.EnumerateFiles(directory)) File.Copy(file, Path.Combine(bad, Path.GetFileName(file)));
        string metadataPath = Path.Combine(bad, "GameScripts.metadata.json");
        var altered = JsonNode.Parse(File.ReadAllText(metadataPath))!;
        altered["types"]![0]!["resource"] = "WrongGeneration.dll";
        File.WriteAllText(metadataPath, altered.ToJsonString());
        var receipt = JsonNode.Parse(File.ReadAllText(Path.Combine(bad, "build.json")))!;
        foreach (var entry in receipt["managedFiles"]!.AsArray())
            if (entry!["path"]!.GetValue<string>() == "GameScripts.metadata.json") entry["sha256"] = Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(metadataPath)));
        File.WriteAllText(Path.Combine(bad, "build.json"), receipt.ToJsonString());
        var mismatch = Call(ManagedOperation.Reload, bad);
        Require(mismatch.Status != 0 && mismatch.Output.Contains("Embedded registration differs"), "Mismatched embedded registration accepted: " + mismatch.Output);
        Require(Call(ManagedOperation.Describe).Output == metadata, "Preflight failure changed active registration");
        // Deliberately keep one known context root alive. This test is last,
        // because successful protection intentionally requires process restart.
        object root = typeof(NativeEntry).GetField("sContext", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;
        var blocked = Call(ManagedOperation.Reload);
        Require(blocked.Status != 0 && blocked.Output.Contains("retired context is still alive"), "Pinned old context was reported as reclaimed");
        Require(Call(ManagedOperation.Describe).Status != 0, "Blocked host executed old code");
        GC.KeepAlive(root);
        Console.WriteLine("PASS: 12 collectible reloads; live-root refusal; cancellation timeout and recovery; candidate preservation; Reset/OnValidate; embedded identity; retained-context safe block");
    }
}
