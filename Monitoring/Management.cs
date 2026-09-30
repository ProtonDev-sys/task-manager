using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace TaskManager.Monitoring;

internal sealed record UserSession(int Id, string Name, string Status);

internal static class Management
{
    public static UserSession[] Sessions()
    {
        if (!WTSEnumerateSessionsW(0, 0, 1, out var buffer, out var count)) throw new Win32Exception();
        try
        {
            if (count > 4096) throw new InvalidOperationException("Session inventory exceeded its safety bound.");
            var result = new List<UserSession>();
            for (var index = 0; index < count; index++)
            {
                var entry = buffer + index * 24;
                var id = Marshal.ReadInt32(entry);
                var name = SessionText(id, 5);
                if (name.Length == 0) continue;
                var domain = SessionText(id, 7);
                result.Add(new UserSession(id, domain.Length > 0 ? domain + "\\" + name : name,
                    Marshal.ReadInt32(entry, 16) switch { 0 => "Active", 1 => "Connected", 4 => "Disconnected", _ => "Idle" }));
            }
            return result.ToArray();
        }
        finally { WTSFreeMemory(buffer); }
    }

    private static string SessionText(int session, int type)
    {
        if (!WTSQuerySessionInformationW(0, session, type, out var buffer, out var bytes)) return "";
        try { return bytes >= 2 ? Marshal.PtrToStringUni(buffer) ?? "" : ""; }
        finally { WTSFreeMemory(buffer); }
    }

    public static void Disconnect(int session)
    {
        if (!WTSDisconnectSession(0, session, false)) throw new Win32Exception();
    }

    public static void SignOff(UserSession session)
    {
        if (!Sessions().Any(current => current.Id == session.Id && current.Name == session.Name))
            throw new InvalidOperationException("The selected session changed. Refresh and select it again.");
        if (!WTSLogoffSession(0, session.Id, false)) throw new Win32Exception();
    }

    public static void Startup(StartupEntry entry, bool enabled)
    {
        if (entry.ApprovalPath == null || entry.ValueName == null) throw new InvalidOperationException("This startup entry cannot be changed.");
        using var root = RegistryKey.OpenBaseKey(entry.Hive, entry.View);
        using var approval = root.CreateSubKey(entry.ApprovalPath, true);
        var current = approval.GetValue(entry.ValueName) as byte[];
        if (!((current == null && entry.Approval == null) || current != null && entry.Approval != null && current.SequenceEqual(entry.Approval)))
            throw new InvalidOperationException("The startup entry changed. Refresh before modifying it.");
        var state = new byte[12];
        state[0] = enabled ? (byte)2 : (byte)3;
        if (!enabled) BitConverter.GetBytes(DateTime.UtcNow.ToFileTimeUtc()).CopyTo(state, 4);
        approval.SetValue(entry.ValueName, state, RegistryValueKind.Binary);
    }

    public static void Service(string name, string action)
    {
        var manager = OpenSCManagerW(null, null, 1);
        if (manager == 0) throw new Win32Exception();
        try
        {
            var service = OpenServiceW(manager, name, action switch { "Start" => 0x14u, "Stop" => 0x24u, "Restart" => 0x34u, _ => throw new ArgumentException("Invalid service action.") });
            if (service == 0) throw new Win32Exception();
            try
            {
                if (action is "Stop" or "Restart")
                {
                    if (!ControlService(service, 1, out _) && Marshal.GetLastWin32Error() != 1062) throw new Win32Exception();
                    var timeout = Stopwatch.StartNew();
                    while (timeout.Elapsed.TotalSeconds < 20)
                    {
                        if (!QueryServiceStatus(service, out var state)) throw new Win32Exception();
                        if (state.State == 1) break;
                        Thread.Sleep(200);
                    }
                    if (!QueryServiceStatus(service, out var stopped) || stopped.State != 1)
                        throw new InvalidOperationException("The service did not stop within 20 seconds.");
                }
                if (action is "Start" or "Restart")
                    if (!StartServiceW(service, 0, 0) && Marshal.GetLastWin32Error() != 1056) throw new Win32Exception();
            }
            finally { CloseServiceHandle(service); }
        }
        finally { CloseServiceHandle(manager); }
    }

    [StructLayout(LayoutKind.Sequential)] private struct ServiceStatus
    { public uint Type, State, Accepted, Exit, SpecificExit, Checkpoint, WaitHint; }
    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool WTSEnumerateSessionsW(nint server, int reserved, int version, out nint sessions, out int count);
    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool WTSQuerySessionInformationW(nint server, int session, int information, out nint buffer, out int bytes);
    [DllImport("wtsapi32.dll")] private static extern void WTSFreeMemory(nint memory);
    [DllImport("wtsapi32.dll", SetLastError = true)] private static extern bool WTSDisconnectSession(nint server, int session, bool wait);
    [DllImport("wtsapi32.dll", SetLastError = true)] private static extern bool WTSLogoffSession(nint server, int session, bool wait);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern nint OpenSCManagerW(string? machine, string? database, uint access);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern nint OpenServiceW(nint manager, string name, uint access);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool ControlService(nint service, uint control, out ServiceStatus status);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool QueryServiceStatus(nint service, out ServiceStatus status);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool StartServiceW(nint service, int count, nint arguments);
    [DllImport("advapi32.dll")] private static extern bool CloseServiceHandle(nint handle);
}
