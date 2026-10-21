using System.Runtime.InteropServices.JavaScript;

[assembly: System.Runtime.Versioning.SupportedOSPlatform("browser")]
public static partial class Program
{
    public static void Main() { }
    [JSExport]
    public static string Run(string fixtures) => ParityRunner.Run(fixtures);
}
