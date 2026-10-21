using System.Text.Json;
using System.Text.Json.Nodes;

// Resolve once after every module has registered, never by scanning runtime assemblies.
internal static class ExecutionOrder
{
    public static void Resolve(string directory)
    {
        string metadataPath = Path.Combine(directory, "GameScripts.metadata.json");
        var metadata = JsonNode.Parse(File.ReadAllText(metadataPath))!;
        var entries = metadata["types"]!.AsArray().Select(item => item!.AsObject()).ToArray();
        var types = entries.ToDictionary(item => item["id"]!.GetValue<string>(), StringComparer.Ordinal);
        var edges = types.Keys.ToDictionary(id => id, _ => new HashSet<string>(StringComparer.Ordinal), StringComparer.Ordinal);
        var incoming = types.Keys.ToDictionary(id => id, _ => 0, StringComparer.Ordinal);
        foreach (var (id, entry) in types)
        {
            foreach (string property in new[] { "executeBefore", "executeAfter" })
            {
                var seen = new HashSet<string>(StringComparer.Ordinal);
                foreach (var value in entry[property]?.AsArray() ?? [])
                {
                    string target = value!.GetValue<string>();
                    if (!seen.Add(target) || target == id || !types.ContainsKey(target))
                        throw new InvalidDataException($"Invalid execution dependency: {id} {property} {target} (duplicate, self or missing type).");
                    string prerequisite = property == "executeBefore" ? id : target;
                    string destination = property == "executeBefore" ? target : id;
                    if (edges[prerequisite].Add(destination)) incoming[destination]++;
                }
            }
        }
        var ready = new SortedSet<(int Order, string Id)>(Comparer<(int Order, string Id)>.Create((a, b) =>
            a.Order != b.Order ? a.Order.CompareTo(b.Order) : StringComparer.Ordinal.Compare(a.Id, b.Id)));
        int DefaultOrder(string id) => types[id]["executionOrder"]?.GetValue<int>() ?? 0;
        foreach (var id in types.Keys) if (incoming[id] == 0) ready.Add((DefaultOrder(id), id));
        var ordered = new List<string>();
        while (ready.Count > 0)
        {
            var next = ready.Min; ready.Remove(next); ordered.Add(next.Id);
            foreach (var dependent in edges[next.Id]) if (--incoming[dependent] == 0) ready.Add((DefaultOrder(dependent), dependent));
        }
        if (ordered.Count != types.Count)
            throw new InvalidDataException("Script execution dependency cycle involving: " + string.Join(", ", incoming.Where(item => item.Value > 0).Select(item => item.Key).Order(StringComparer.Ordinal)));
        for (int rank = 0; rank < ordered.Count; ++rank)
        {
            var entry = types[ordered[rank]];
            entry["defaultExecutionOrder"] = DefaultOrder(ordered[rank]);
            entry["executionOrder"] = rank;
        }
        string json = metadata.ToJsonString();
        string sourcePath = Path.Combine(directory, "GameScripts.Registry.g.cs");
        string source = File.ReadAllText(sourcePath);
        const string marker = "public const string Metadata = ";
        int start = source.IndexOf(marker, StringComparison.Ordinal);
        int newline = start < 0 ? -1 : source.IndexOf('\n', start);
        int end = newline < 0 ? -1 : source.LastIndexOf(';', newline);
        if (start < 0 || end < 0) throw new InvalidDataException("Missing generated metadata declaration.");
        source = source[..(start + marker.Length)] + JsonSerializer.Serialize(json) + source[end..];
        File.WriteAllText(sourcePath, source);
        File.WriteAllText(metadataPath, json);
        Console.WriteLine($"Script execution order: {ordered.Count} types; acyclic stable ordering verified.");
    }
}
