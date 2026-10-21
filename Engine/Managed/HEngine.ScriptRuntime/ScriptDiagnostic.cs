using System.Text.Json;

namespace HEngine.Scripting;

internal static class ScriptDiagnostic
{
    // Explicit writer keeps this cold error path independent of reflection and
    // publishing trim roots. Preserve chained exceptions in detail as well.
    public static string Encode(Exception error)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject();
            writer.WriteNumber("scriptError", 1);
            writer.WriteString("message", error.GetType().FullName + ": " + error.Message);
            writer.WriteString("stack", error.ToString());
            writer.WriteEndObject();
        }
        return System.Text.Encoding.UTF8.GetString(stream.ToArray());
    }
}
