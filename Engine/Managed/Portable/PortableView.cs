using System.Text.Json;

namespace HEngine.Portable;

// Compiled into each game's managed module, not into the native ABI assembly.
// A bounded presentation protocol shared by the native and Canvas hosts.
public sealed class ViewObject
{
    public string id { get; set; } = "";
    public double x { get; set; }
    public double y { get; set; }
    public double width { get; set; } = 1;
    public double height { get; set; } = 1;
    public string color { get; set; } = "#ffffff";
}

public sealed class ViewAction
{
    public string id { get; set; } = "";
    public string text { get; set; } = "";
}

public sealed class PortableView
{
    public int schema { get; set; } = 1;
    public string mode { get; set; } = "menu";
    public string title { get; set; } = "";
    public string status { get; set; } = "";
    public double worldWidth { get; set; } = 24;
    public double worldHeight { get; set; } = 14;
    public long saveRevision { get; set; }
    public long soundRevision { get; set; }
    public string sound { get; set; } = "";
    public List<ViewObject> objects { get; set; } = new();
    public List<ViewAction> actions { get; set; } = new();
    public void Validate()
    {
        if (schema != 1 || objects is null || actions is null || objects.Count > 256 || actions.Count > 4 ||
            !double.IsFinite(worldWidth) || !double.IsFinite(worldHeight) || worldWidth <= 0 || worldHeight <= 0)
            throw new InvalidOperationException("Unsupported portable view schema, bounds or budget.");
        var ids = new HashSet<string>();
        foreach (var item in objects)
        {
            if (item is null || string.IsNullOrEmpty(item.id) || !ids.Add(item.id) ||
                !double.IsFinite(item.x) || !double.IsFinite(item.y) ||
                !double.IsFinite(item.width) || !double.IsFinite(item.height) || item.width <= 0 || item.height <= 0 ||
                item.color is null || item.color.Length != 7 || item.color[0] != '#' ||
                !uint.TryParse(item.color.AsSpan(1), System.Globalization.NumberStyles.HexNumber,
                    System.Globalization.CultureInfo.InvariantCulture, out _))
                throw new InvalidOperationException("Invalid portable object ID, geometry or color.");
        }
        ids.Clear();
        foreach (var action in actions)
            if (action is null || string.IsNullOrEmpty(action.id) || !ids.Add(action.id) || action.text is null)
                throw new InvalidOperationException("Invalid portable action.");
    }
    public string ToJson() { Validate(); return JsonSerializer.Serialize(this); }
}
