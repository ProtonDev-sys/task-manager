using System.Diagnostics;
using System.Security.Principal;
using Microsoft.Diagnostics.Tracing.Parsers;
using Microsoft.Diagnostics.Tracing.Session;

namespace TaskManager.Monitoring;

internal readonly record struct IoRates(double Disk, double Network, long NetworkBytes);

internal sealed class IoTrace : IDisposable
{
    private readonly object gate = new();
    private Dictionary<int, Counts> counts = new(512);
    private readonly Dictionary<int, long> networkTotals = new(512);
    private readonly TraceEventSession? session;
    private readonly Task? processing;
    private long timestamp = Stopwatch.GetTimestamp();
    public string? Error { get; private set; }
    public bool Available => session != null && Error == null;

    public IoTrace()
    {
        using var identity = WindowsIdentity.GetCurrent();
        if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator))
        {
            Error = "Run as administrator to collect per-process disk and network activity.";
            return;
        }
        try
        {
            session = new TraceEventSession($"TaskManager-{Environment.ProcessId}-{Guid.NewGuid():N}");
            session.StopOnDispose = true;
            session.EnableKernelProvider(KernelTraceEventParser.Keywords.DiskIO | KernelTraceEventParser.Keywords.DiskIOInit |
                KernelTraceEventParser.Keywords.NetworkTCPIP | KernelTraceEventParser.Keywords.Process | KernelTraceEventParser.Keywords.Thread);
            var kernel = session.Source.Kernel;
            kernel.DiskIORead += data => Add(data.ProcessID, data.TransferSize, 0);
            kernel.DiskIOWrite += data => Add(data.ProcessID, data.TransferSize, 0);
            kernel.TcpIpSend += data => Add(data.ProcessID, 0, data.size);
            kernel.TcpIpRecv += data => Add(data.ProcessID, 0, data.size);
            kernel.TcpIpSendIPV6 += data => Add(data.ProcessID, 0, data.size);
            kernel.TcpIpRecvIPV6 += data => Add(data.ProcessID, 0, data.size);
            kernel.UdpIpSend += data => Add(data.ProcessID, 0, data.size);
            kernel.UdpIpRecv += data => Add(data.ProcessID, 0, data.size);
            kernel.UdpIpSendIPV6 += data => Add(data.ProcessID, 0, data.size);
            kernel.UdpIpRecvIPV6 += data => Add(data.ProcessID, 0, data.size);
            kernel.ProcessStart += data => { lock (gate) { counts.Remove(data.ProcessID); networkTotals.Remove(data.ProcessID); } };
            processing = Task.Run(() =>
            {
                try { session.Source.Process(); }
                catch (Exception error) { Error = error.Message; }
            });
        }
        catch (Exception error) when (error is UnauthorizedAccessException or System.ComponentModel.Win32Exception or InvalidOperationException or NotSupportedException)
        {
            session?.Dispose();
            session = null;
            Error = error.Message;
        }
    }

    private void Add(int processId, long disk, long network)
    {
        if (processId < 0) return;
        lock (gate)
        {
            counts.TryGetValue(processId, out var previous);
            counts[processId] = new Counts(previous.Disk + disk, previous.Network + network);
        }
    }

    public Dictionary<int, IoRates> Sample(HashSet<int> live)
    {
        var now = Stopwatch.GetTimestamp();
        var seconds = Stopwatch.GetElapsedTime(timestamp, now).TotalSeconds;
        timestamp = now;
        var result = new Dictionary<int, IoRates>(live.Count);
        lock (gate)
        {
            foreach (var processId in live)
            {
                counts.TryGetValue(processId, out var amount);
                networkTotals.TryGetValue(processId, out var total);
                total += amount.Network;
                networkTotals[processId] = total;
                result[processId] = new IoRates(seconds > 0 ? amount.Disk / seconds : 0, seconds > 0 ? amount.Network / seconds : 0, total);
            }
            counts.Clear();
            foreach (var stale in networkTotals.Keys.Where(processId => !live.Contains(processId)).ToArray()) networkTotals.Remove(stale);
        }
        return result;
    }

    public void Dispose()
    {
        session?.Dispose();
        if (processing != null) _ = processing.ContinueWith(_ => session?.Source.Dispose(), TaskScheduler.Default);
    }

    private readonly record struct Counts(long Disk, long Network);
}
