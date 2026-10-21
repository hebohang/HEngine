using System;
using System.Diagnostics;
using System.IO;
using System.Text;

// Native build output must not pass through PowerShell 5's OEM/UTF-16 redirects.
// Managed callbacks drain both pipes concurrently without requiring a runspace.
public static class HEProjectProcess
{
    public static int Run(string executable, string arguments, string directory, string log)
    {
        var utf8 = new UTF8Encoding(false);
        var start = new ProcessStartInfo(executable, arguments) {
            WorkingDirectory = directory, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = utf8, StandardErrorEncoding = utf8
        };
        using (var output = new StreamWriter(new FileStream(log, FileMode.Create, FileAccess.Write,
            FileShare.ReadWrite), new UTF8Encoding(true)))
        using (var process = new Process { StartInfo = start })
        {
            output.AutoFlush = true;
            var gate = new object();
            DataReceivedEventHandler write = (sender, data) => {
                if (data.Data != null) lock (gate) output.WriteLine(data.Data);
            };
            process.OutputDataReceived += write;
            process.ErrorDataReceived += write;
            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();
            return process.ExitCode;
        }
    }
}
