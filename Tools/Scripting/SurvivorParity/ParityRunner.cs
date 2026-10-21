using System.Text.Json;
using System.Text.Json.Nodes;
using GameScripts.Survivor;

public static class ParityRunner
{
public static string Run(string fixturesJson)
{
var options = new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
var fixtures = JsonNode.Parse(fixturesJson)!.AsArray();
int checkedCases = 0;
foreach (var fixture in fixtures)
{
    string kind = fixture!["kind"]!.GetValue<string>();
    JsonNode input = fixture["input"]!;
    object actual = kind switch
    {
        "reward" => RunReward.Compute(input.Deserialize<RunRewardInput>(options)!),
        "encounter" => new { target=EncounterRules.LocalTarget(input["tier"]!,input["progress"]!.GetValue<double>(),input["difficulty"]!.GetValue<double>(),input["challenge"]!.GetValue<double>()),rate=EncounterRules.SpawnRate(input["tier"]!,input["kills"]!.GetValue<double>(),input["difficulty"]!.GetValue<double>(),input["challenge"]!.GetValue<double>()) },
        "profile" => ProfileValue(input),
        "characterLevel" => new { might=CharacterRules.StarpawMight(input.GetValue<int>()), gold=CharacterRules.HoneyGold(input.GetValue<int>()) },
        "trinketDamage" => CombatHookRules.TrinketDamage(input["trinkets"]!.Deserialize<string[]>()!,input["tags"]!.Deserialize<string[]>()!,input["fullHealth"]!.GetValue<bool>(),input["boss"]!.GetValue<bool>(),input["elite"]!.GetValue<bool>(),input["velocityLevel"]!.GetValue<int>()),
        "defense" => CombatRules.ResolveDefense(
            input[0]!.GetValue<double>(), input[1]!.GetValue<double>(), input[2]!.GetValue<double>(),
            input[3]!.GetValue<double>(), input[4]!.GetValue<double>(), input[5]!.GetValue<double>()),
        "experience" => CombatRules.ExperienceRequirement(input.GetValue<int>()),
        "rng" => RngValues(input.GetValue<long>()),
        "weighted" => WeightedValues(input),
        "budget" => BudgetValues(input),
        "upgrade" => UpgradeValues(input, options),
        "evolution" => EvolutionValues(input, options),
        "terrain" => TerrainValues(input),
        "regions" => RegionValues(input),
        "character" => CharacterRules.CompileStartState(input["id"]!.GetValue<string>(),
            input["definition"]!.Deserialize<CharacterDefinition>(options)!, input["training"]!.GetValue<int>()),
        "challenge" => ChallengeRules.Combine(input["challenges"]!.Deserialize<ChallengeDefinition[]>(options)!,
            input["minute"]!.GetValue<int>()),
        _ => throw new InvalidOperationException("Unknown fixture " + kind)
    };
    JsonNode result = JsonSerializer.SerializeToNode(actual, options)!;
    Compare(fixture["expected"], result, kind + " case " + checkedCases);
    ++checkedCases;
}
// Invalid numeric values must fail before being used in settlement arithmetic.
foreach (double value in new[] { double.NaN, double.PositiveInfinity, double.NegativeInfinity })
{
    try { RunReward.Compute(new() { SurvivalSeconds = value }); }
    catch (ArgumentException) { continue; }
    throw new Exception("Nonfinite reward input accepted");
}
foreach (int slots in new[] { -1, 0, 2, 7 })
{
    try { RunReward.WeaponSlotRewardFactor(slots); }
    catch (ArgumentOutOfRangeException) { continue; }
    throw new Exception("Invalid slot limit accepted");
}
return $"unit-test PASS: {checkedCases} Python/C# differential cases and invalid input checks";
}

static object UpgradeValues(JsonNode input, JsonSerializerOptions options)
{
    var pool = new UpgradePool(input["config"]!.Deserialize<UpgradePoolInput>(options)!);
    var receipts = new List<UpgradeReceipt>();
    foreach (var operation in input["operations"]!.AsArray())
        receipts.Add(operation!.GetValue<string>() switch
        {
            "roll" => new("rolled", pool.Roll()),
            "reroll" => pool.Reroll(),
            "skip" => pool.Skip(),
            "banish" => pool.Banish(pool.CurrentChoices.Length > 0 ? pool.CurrentChoices[0].Id : "missing"),
            _ => throw new Exception("Unknown upgrade operation")
        });
    return new { receipts, pool.RerollsRemaining, pool.SkipsRemaining, pool.BanishesRemaining, pool.RollSerial };
}
static object ProfileValue(JsonNode input)
{
    try { return new { valid = true, profile = ProfileRules.Normalize(input["profile"]!.AsObject(), input["catalog"]!.AsObject()) }; }
    catch (ArgumentException) { return new { valid = false }; }
}
static object EvolutionValues(JsonNode input, JsonSerializerOptions options)
{
    var resolver = new EvolutionResolver(input["config"]!.Deserialize<EvolutionInput>(options)!);
    var receipts = new List<EvolutionReceipt>();
    foreach (var tier in input["tiers"]!.AsArray()) receipts.Add(resolver.ResolveChest(tier!.GetValue<int>()));
    return receipts;
}
static object TerrainValues(JsonNode input)
{
    var terrain=new TerrainSurface(input["catalog"]!.AsObject());
    return input["points"]!.AsArray().Select(p=>terrain.Height(p![0]!.GetValue<double>(),p[1]!.GetValue<double>())).ToArray();
}
static object RegionValues(JsonNode input)
{
    var controller=new RegionObjectives(input["catalog"]!.AsObject());var results=new List<object>();
    foreach(var operation in input["operations"]!.AsArray())
    {
        controller.Update(operation![0]!.GetValue<double>(),operation[1]!.GetValue<double>(),operation[2]!.GetValue<double>());
        results.Add(new {controller.ObjectiveState,completed=controller.Completed.ToArray(),states=controller.Regions.Select(r=>r.State).ToArray()});
    }
    return results;
}
static uint[] RngValues(long seed)
{
    var rng = new DeterministicRng(seed);
    var result = new uint[32];
    for (int i = 0; i < result.Length; ++i) result[i] = rng.NextU32();
    return result;
}
static int[] WeightedValues(JsonNode input)
{
    var rng = new DeterministicRng(input["seed"]!.GetValue<long>());
    double[] weights = input["weights"]!.Deserialize<double[]>()!;
    var result = new int[32];
    for (int i = 0; i < result.Length; ++i) result[i] = rng.WeightedIndex(weights);
    return result;
}
static object BudgetValues(JsonNode input)
{
    var budget = new ProjectileBudget(input["gameplay"]!.GetValue<int>(), input["visual"]!.GetValue<int>());
    var receipts = new List<string>();
    foreach (var operation in input["operations"]!.AsArray())
    {
        string command = operation!.GetValue<string>();
        switch (command)
        {
            case "reserve": receipts.Add(budget.Reserve()); break;
            case "optional": receipts.Add(budget.Reserve(false)); break;
            case "release": budget.Release(); break;
            case "releaseCore": budget.Release(false); break;
            case "reset": budget.Reset(); break;
            default: throw new Exception("Unknown budget operation");
        }
    }
    return new { receipts, budget.GameplayActive, budget.VisualActive, budget.RetryCount,
        budget.VisualDegradeCount, budget.RejectedCount, budget.Generation };
}
static void Compare(JsonNode? expected, JsonNode? actual, string location)
{
    if (expected is JsonObject obj && actual is JsonObject other)
    {
        if (obj.Count != other.Count) throw new Exception(location + ": object shape differs");
        foreach (var pair in obj) Compare(pair.Value, other[pair.Key], location + "." + pair.Key);
        return;
    }
    if (expected is JsonArray array && actual is JsonArray otherArray)
    {
        if (array.Count != otherArray.Count) throw new Exception(location + ": array length differs");
        for (int i = 0; i < array.Count; ++i) Compare(array[i], otherArray[i], location + "[" + i + "]");
        return;
    }
    if (expected?.GetValueKind() == JsonValueKind.Number && actual?.GetValueKind() == JsonValueKind.Number)
    {
        double left = expected.GetValue<double>(), right = actual.GetValue<double>();
        if (Math.Abs(left - right) <= 1e-12 * Math.Max(1, Math.Abs(left))) return;
    }
    else if (JsonNode.DeepEquals(expected, actual)) return;
    throw new Exception($"{location}: expected {expected}, got {actual}");
}

}
