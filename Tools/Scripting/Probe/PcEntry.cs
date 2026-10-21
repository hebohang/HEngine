using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

public static unsafe class PcEntry
{
    // Caller owns the output buffer; return byte length or negative failure.
    // This diagnostic export is not the script plugin ABI.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Run(byte* output, int capacity)
    {
        try
        {
            var root = new object();
            var handle = GCHandle.Alloc(root);
            try
            {
                for (var i = 0; i < 128; i++) _ = new byte[8192];
                GC.Collect();
                if (!ReferenceEquals(handle.Target, root)) throw new Exception("GC root corrupted");
                try { throw new InvalidOperationException("controlled-coreclr-exception"); }
                catch (InvalidOperationException error)
                {
                    var text = $"CoreCLR={Environment.Version}; rooted=True; framework={typeof(object).Assembly.Location}; {error}";
                    var bytes = Encoding.UTF8.GetBytes(text);
                    if (output == null || capacity < bytes.Length) return -bytes.Length;
                    bytes.CopyTo(new Span<byte>(output, capacity));
                    return bytes.Length;
                }
            }
            finally { handle.Free(); }
        }
        catch { return int.MinValue; }
    }
}
