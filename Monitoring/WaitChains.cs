using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace TaskManager.Monitoring;

internal static class WaitChains
{
    public static string Analyze(ProcessSample selected)
    {
        using var process = Process.GetProcessById(selected.Id);
        if (process.StartTime.ToUniversalTime().ToFileTimeUtc() != selected.Created)
            throw new InvalidOperationException("The selected process has exited. Refresh and select it again.");
        var session = OpenThreadWaitChainSession(0, 0);
        if (session == 0) throw new Win32Exception();
        try
        {
            var result = new StringBuilder();
            var inspected = 0;
            foreach (ProcessThread thread in process.Threads)
            {
                using (thread)
                {
                    if (inspected++ >= 128) { result.AppendLine("Additional threads omitted (128-thread safety bound)."); break; }
                    var nodes = new WaitNode[16];
                    uint count = (uint)nodes.Length;
                    if (!GetThreadWaitChain(session, 0, 0, (uint)thread.Id, ref count, nodes, out var cycle))
                    {
                        result.AppendLine($"Thread {thread.Id}: unavailable ({new Win32Exception().Message})");
                        continue;
                    }
                    if (count > nodes.Length) throw new InvalidOperationException("The wait chain exceeded its native safety bound.");
                    if (nodes.Take((int)count).Any(node => node.Type == 8 && node.ThreadId == thread.Id && node.ProcessId != selected.Id))
                    {
                        result.AppendLine($"Thread {thread.Id}: exited during analysis.");
                        continue;
                    }
                    result.Append($"Thread {thread.Id}: ");
                    for (var index = 0; index < count; index++)
                    {
                        if (index > 0) result.Append(" → ");
                        var node = nodes[index];
                        result.Append(node.Type == 8 ? $"Process {node.ProcessId}, thread {node.ThreadId}" : ObjectName(node.Type));
                        result.Append(" (" + StatusName(node.Status) + ")");
                    }
                    if (cycle) result.Append(" — possible deadlock cycle");
                    result.AppendLine();
                }
            }
            return result.Length > 0 ? result.ToString() : "No threads are available.";
        }
        finally { CloseThreadWaitChainSession(session); }
    }

    private static string ObjectName(uint type) => type switch
    {
        1 => "Critical section", 2 => "SendMessage", 3 => "Mutex", 4 => "ALPC", 5 => "COM",
        6 => "Thread wait", 7 => "Process wait", 9 => "COM activation", 11 => "Socket I/O", 12 => "SMB I/O", _ => "Unknown wait object"
    };

    private static string StatusName(uint status) => status switch
    {
        1 => "Access denied", 2 => "Running", 3 => "Blocked", 4 or 5 => "Process only", 6 => "Owned",
        7 => "Not owned", 8 => "Abandoned", 10 => "Error", _ => "Unknown"
    };

    [StructLayout(LayoutKind.Explicit, Size = 280)]
    private struct WaitNode
    {
        [FieldOffset(0)] public uint Type;
        [FieldOffset(4)] public uint Status;
        [FieldOffset(8)] public uint ProcessId;
        [FieldOffset(12)] public uint ThreadId;
    }

    [DllImport("advapi32.dll", SetLastError = true)] private static extern nint OpenThreadWaitChainSession(uint flags, nint callback);
    [DllImport("advapi32.dll")] private static extern void CloseThreadWaitChainSession(nint session);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool GetThreadWaitChain(nint session, nint context, uint flags, uint threadId, ref uint count, [Out] WaitNode[] nodes, out bool cycle);
}
