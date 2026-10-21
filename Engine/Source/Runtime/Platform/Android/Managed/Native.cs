using System.Runtime.InteropServices;
using HEngine.Scripting;
using HEngine.Scripting.Interop;

namespace HEngine.AndroidHost;

internal static unsafe class Native
{
    private const string Library = "hegame";
    [DllImport(Library, EntryPoint = "he_android_prepare")] public static extern int Prepare();
    [DllImport(Library, EntryPoint = "he_android_error")] private static extern int Error(byte[] output, int capacity);
    public static string LastError()
    {
        byte[] buffer = new byte[8192];
        return System.Text.Encoding.UTF8.GetString(buffer, 0, Error(buffer, buffer.Length));
    }
    [DllImport(Library, EntryPoint = "he_android_run", CallingConvention = CallingConvention.Cdecl)]
    private static extern int Run([MarshalAs(UnmanagedType.LPUTF8Str)] string content,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string saved, nint entry);
    public static int RunGame(string content, string saved)
        => Run(content, saved, (nint)(delegate* unmanaged[Cdecl]<Request*, int>)&NativeEntry.Dispatch);
    [DllImport(Library, EntryPoint = "he_android_surface")] public static extern void Surface(nint environment, nint surface);
    [DllImport(Library, EntryPoint = "he_android_resume")] public static extern void Resume(int resumed);
    [DllImport(Library, EntryPoint = "he_android_stop")] public static extern void Stop();
    [DllImport(Library, EntryPoint = "he_android_touch_controls")] public static extern int TouchControls();
    [DllImport(Library, EntryPoint = "he_android_input")] public static extern void Input(float x, float y, float moveX, float moveY, int buttons);
}
