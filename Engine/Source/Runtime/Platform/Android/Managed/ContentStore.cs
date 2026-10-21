using Android.Content.Res;
using System.Security.Cryptography;
using System.Text.Json;

namespace HEngine.AndroidHost;

internal static class ContentStore
{
    public static string Extract(AssetManager assets, string filesRoot)
    {
        using var manifestStream = assets.Open("content/android-content.json");
        using var manifest = JsonDocument.Parse(manifestStream);
        string identity = manifest.RootElement.GetProperty("identity").GetString()!;
        if (identity.Length != 64 || !identity.All(Uri.IsHexDigit)) throw new InvalidDataException("Invalid content identity");
        string contentRoot = Path.Combine(filesRoot, "content", identity);
        string complete = Path.Combine(contentRoot, ".complete");
        if (File.Exists(complete)) return contentRoot;
        Directory.CreateDirectory(contentRoot);
        foreach (var file in manifest.RootElement.GetProperty("files").EnumerateArray())
        {
            string relative = file.GetProperty("path").GetString()!;
            string target = Path.GetFullPath(Path.Combine(contentRoot, relative));
            if (Path.IsPathRooted(relative) || !target.StartsWith(contentRoot + Path.DirectorySeparatorChar, StringComparison.Ordinal))
                throw new InvalidDataException("Content path escapes package");
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using (var input = assets.Open("content/" + relative))
            using (var output = File.Create(target + ".tmp")) input.CopyTo(output);
            using (var check = File.OpenRead(target + ".tmp"))
            {
                string hash = Convert.ToHexString(SHA256.HashData(check));
                if (!hash.Equals(file.GetProperty("sha256").GetString(), StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Content integrity failure: " + relative);
            }
            File.Move(target + ".tmp", target, true);
        }
        File.WriteAllText(complete, identity);
        return contentRoot;
    }
}
