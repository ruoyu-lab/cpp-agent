using System;
using System.Runtime.InteropServices;

internal static class Native
{
    private const string Library = "agent_capi_shared";

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr agent_last_error();

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void agent_string_free(IntPtr value);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int agent_capi_negotiate_abi_version(int minVersion, int maxVersion, out int version);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int agent_runner_create_with_echo_model(out IntPtr runner);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int agent_runner_run(IntPtr runner, string input, string sessionId, out IntPtr resultJson);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void agent_runner_release(IntPtr runner);
}

internal static class Program
{
    private static void RequireOk(int status, string context)
    {
        if (status == 0)
        {
            return;
        }

        var error = Marshal.PtrToStringUTF8(Native.agent_last_error()) ?? string.Empty;
        throw new InvalidOperationException($"{context}: {error}");
    }

    private static void Main()
    {
        RequireOk(Native.agent_capi_negotiate_abi_version(3, 3, out _), "negotiate");
        RequireOk(Native.agent_runner_create_with_echo_model(out var runner), "create");

        IntPtr result = IntPtr.Zero;
        try
        {
            RequireOk(Native.agent_runner_run(runner, "hello from C#", "csharp-example", out result), "run");
            Console.WriteLine(Marshal.PtrToStringUTF8(result));
        }
        finally
        {
            if (result != IntPtr.Zero)
            {
                Native.agent_string_free(result);
            }
            Native.agent_runner_release(runner);
        }
    }
}
