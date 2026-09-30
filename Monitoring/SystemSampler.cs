using System.ComponentModel;
using System.Diagnostics;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;

namespace TaskManager.Monitoring;

internal sealed class SystemSampler : IDisposable
{
    private Dictionary<int, PreviousProcess> previous = new(512);
    private Dictionary<int, PreviousProcess> scratch = new(512);
    private readonly List<ProcessSample> processSamples = new(512);
    private readonly Dictionary<string, PreviousNetwork> previousNetworks = new(StringComparer.Ordinal);
    private readonly DiskCounters disks = new();
    private readonly ProcessorCounters processors = new();
    private readonly GpuCounters gpu = new();
    private readonly MetadataCatalog? metadata;
    private readonly IoTrace? trace;
    private UserSession[] sessions = [];
    private long sessionRefresh;
    private Dictionary<int, ServiceEntry[]> serviceNames = new();
    private NetworkInterface[] adapters = [];
    private long adapterRefresh;
    private nint buffer;
    private uint capacity = 1024 * 1024;
    private long previousTimestamp;
    private long previousIdle;
    private long previousTotal;
    private bool disposed;

    public SystemSampler(bool enrich = false)
    {
        buffer = Marshal.AllocHGlobal((int)capacity);
        if (enrich) { metadata = new MetadataCatalog(); trace = new IoTrace(); }
    }

    public unsafe SystemSample Sample()
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        var started = Stopwatch.GetTimestamp();
        var elapsed = previousTimestamp == 0 ? 0 : Stopwatch.GetElapsedTime(previousTimestamp, started).TotalSeconds;
        uint returned;
        while (true)
        {
            var status = NativeMethods.NtQuerySystemInformation(5, buffer, capacity, out returned);
            if (status == NativeMethods.LengthMismatch)
            {
                var nextCapacity = Math.Max((ulong)capacity * 2, (ulong)returned + 65536);
                if (nextCapacity > 64 * 1024 * 1024) throw new InvalidOperationException("Process snapshot exceeded its safety bound.");
                var newBuffer = Marshal.AllocHGlobal((int)nextCapacity);
                Marshal.FreeHGlobal(buffer);
                buffer = newBuffer;
                capacity = (uint)nextCapacity;
                continue;
            }
            if (status < 0) throw new InvalidOperationException($"Process sampling failed (NTSTATUS 0x{status:X8}).");
            break;
        }

        processSamples.Clear();
        var result = processSamples;
        uint offset = 0;
        scratch.Clear();
        var current = scratch;
        while (true)
        {
            if ((ulong)offset + (uint)sizeof(NativeMethods.ProcessInformation) > returned)
                throw new InvalidOperationException("Invalid native process snapshot layout.");
            var information = *(NativeMethods.ProcessInformation*)(buffer + checked((int)offset));
            var processId = checked((int)information.UniqueProcessId);
            if (processId >= 0)
            {
                var identity = information.CreateTime;
                var time = information.UserTime + information.KernelTime;
                var found = previous.TryGetValue(processId, out var old);
                var sameIdentity = found && old.Created == identity;
                var sameProcess = sameIdentity && elapsed > 0;
                var name = sameIdentity ? old.Name : ReadName(information.ImageName, returned, processId);
                result.Add(new ProcessSample(processId, checked((int)information.InheritedFromUniqueProcessId), name, identity,
                    sameProcess ? Metrics.CpuPercent(old.Time, time, elapsed, Environment.ProcessorCount) : 0,
                    Math.Max(0, information.WorkingSetPrivateSize), checked((long)information.PrivatePageCount),
                    checked((int)information.NumberOfThreads), checked((int)information.HandleCount), checked((int)information.SessionId),
                    sameProcess ? Metrics.Rate(old.Read, information.ReadTransferCount, elapsed) : 0,
                    sameProcess ? Metrics.Rate(old.Write, information.WriteTransferCount, elapsed) : 0)
                { CpuTime = time, Status = ThreadStatus(offset, information.NumberOfThreads, information.NextEntryOffset, returned) });
                current[processId] = new PreviousProcess(identity, time, information.ReadTransferCount, information.WriteTransferCount, name);
            }
            if (information.NextEntryOffset == 0) break;
            if (information.NextEntryOffset < sizeof(NativeMethods.ProcessInformation) || information.NextEntryOffset > returned - offset)
                throw new InvalidOperationException("Invalid native process snapshot offset.");
            offset += information.NextEntryOffset;
        }
        (previous, scratch) = (scratch, previous);
        var processMilliseconds = Stopwatch.GetElapsedTime(started).TotalMilliseconds;

        if (!NativeMethods.GetSystemTimes(out var idle, out var kernel, out var user)) throw new Win32Exception();
        var total = kernel + user;
        var totalDelta = total - previousTotal;
        var cpu = previousTimestamp > 0 && totalDelta > 0
            ? Math.Clamp(100d * (totalDelta - (idle - previousIdle)) / totalDelta, 0, 100) : 0;
        previousIdle = idle;
        previousTotal = total;
        previousTimestamp = started;

        var memory = new NativeMethods.MemoryStatus { Length = (uint)Marshal.SizeOf<NativeMethods.MemoryStatus>() };
        if (!NativeMethods.GlobalMemoryStatusEx(ref memory)) throw new Win32Exception();
        var performance = new NativeMethods.PerformanceInformation { Size = (uint)Marshal.SizeOf<NativeMethods.PerformanceInformation>() };
        if (!NativeMethods.GetPerformanceInfo(ref performance, performance.Size)) throw new Win32Exception();
        var pageSize = (ulong)performance.PageSize;
        var deviceStart = Stopwatch.GetTimestamp();
        var diskSamples = disks.Sample();
        var networkSamples = SampleNetworks(started, elapsed);
        var coreSamples = processors.Sample();
        var deviceMilliseconds = Stopwatch.GetElapsedTime(deviceStart).TotalMilliseconds;
        result.Add(new ProcessSample(-1, 0, "System interrupts", 0, processors.InterruptUsage, 0, 0, 0, 0, 0, 0, 0) { DiskRate = 0, NetworkRate = 0 });
        var samples = result.ToArray();
        var inventoryStart = Stopwatch.GetTimestamp();
        if (metadata != null && (sessionRefresh == 0 || Stopwatch.GetElapsedTime(sessionRefresh, started).TotalSeconds >= 5))
        {
            try { sessions = Management.Sessions(); }
            catch (Win32Exception) { sessions = []; }
            try { serviceNames = SystemInventory.Services().Where(service => service.ProcessId > 0).GroupBy(service => service.ProcessId)
                .ToDictionary(group => group.Key, group => group.ToArray()); }
            catch (Win32Exception) { serviceNames.Clear(); }
            sessionRefresh = started;
        }
        var inventoryMilliseconds = Stopwatch.GetElapsedTime(inventoryStart).TotalMilliseconds;
        var gpuStart = Stopwatch.GetTimestamp();
        var gpuSamples = gpu.Sample();
        var gpuMilliseconds = Stopwatch.GetElapsedTime(gpuStart).TotalMilliseconds;
        var attributionStart = Stopwatch.GetTimestamp();
        if (gpuSamples.Adapters.Length > 0)
            for (var index = 0; index < samples.Length; index++)
            {
                gpuSamples.Processes.TryGetValue(samples[index].Id, out var activity);
                samples[index] = samples[index] with { Gpu = activity.Usage, GpuEngine = activity.Usage > 0 ? activity.Engine : "" };
            }
        if (trace?.Available == true)
        {
            var rates = trace.Sample(samples.Select(process => process.Id).ToHashSet());
            for (var index = 0; index < samples.Length; index++)
                if (rates.TryGetValue(samples[index].Id, out var rate))
                    samples[index] = samples[index] with { DiskRate = rate.Disk, NetworkRate = rate.Network, NetworkBytes = rate.NetworkBytes };
        }
        else if (metadata != null)
            // Without the elevated kernel trace, approximate per-process disk activity with the process's file I/O transfer rate.
            for (var index = 0; index < samples.Length; index++)
                if (samples[index].Id > 0) samples[index] = samples[index] with { DiskRate = samples[index].ReadRate + samples[index].WriteRate };
        var attributionMilliseconds = Stopwatch.GetElapsedTime(attributionStart).TotalMilliseconds;
        var metadataStart = Stopwatch.GetTimestamp();
        var processMetadata = metadata?.Update(samples, serviceNames) ?? new Dictionary<(int, long), ProcessMetadata>();
        var metadataMilliseconds = Stopwatch.GetElapsedTime(metadataStart).TotalMilliseconds;
        return new SystemSample(DateTime.Now, cpu, memory.TotalPhysical, memory.AvailablePhysical,
            (ulong)performance.CommitTotal * pageSize, (ulong)performance.CommitLimit * pageSize,
            (ulong)performance.SystemCache * pageSize, (ulong)performance.KernelPaged * pageSize,
            (ulong)performance.KernelNonpaged * pageSize, (int)performance.ThreadCount, (int)performance.HandleCount,
            TimeSpan.FromMilliseconds(NativeMethods.GetTickCount64()), samples, diskSamples, networkSamples,
            Stopwatch.GetElapsedTime(started).TotalMilliseconds)
        { CpuCores = coreSamples, Metadata = processMetadata, IoError = trace?.Error, Services = serviceNames,
            Gpus = gpuSamples.Adapters, CpuSpeedFactor = gpu.SpeedFactor, Sessions = sessions,
            GpuMilliseconds = gpuMilliseconds, MetadataMilliseconds = metadataMilliseconds, ProcessCount = (int)performance.ProcessCount,
            MonotonicSeconds = started / (double)Stopwatch.Frequency,
            ProcessMilliseconds = processMilliseconds, DeviceMilliseconds = deviceMilliseconds,
            InventoryMilliseconds = inventoryMilliseconds, AttributionMilliseconds = attributionMilliseconds };
    }

    private string ThreadStatus(uint offset, uint threads, uint next, uint returned)
    {
        if (threads == 0) return "";
        var end = next == 0 ? returned : (ulong)offset + next;
        if ((ulong)offset + 256 + threads * 80ul > end) return "";
        for (var index = 0u; index < threads; index++)
        {
            var entry = buffer + checked((int)(offset + 256 + index * 80));
            if (Marshal.ReadInt32(entry, 68) != 5 || Marshal.ReadInt32(entry, 72) is not (5 or 12)) return "";
        }
        return "Suspended";
    }

    private string ReadName(NativeMethods.UnicodeString name, uint returned, int processId)
    {
        if (name.Length == 0) return processId switch { 0 => "System Idle Process", 4 => "System", _ => $"Process {processId}" };
        var location = (long)name.Buffer - (long)buffer;
        if (location < 0 || (ulong)location + name.Length > returned || name.Length % 2 != 0)
            throw new InvalidOperationException("Invalid native process name range.");
        return Marshal.PtrToStringUni(name.Buffer, name.Length / 2) ?? $"Process {processId}";
    }

    private NetworkSample[] SampleNetworks(long timestamp, double elapsed)
    {
        if (adapterRefresh == 0 || Stopwatch.GetElapsedTime(adapterRefresh, timestamp).TotalSeconds >= 10)
        {
            try
            {
                adapters = NetworkInterface.GetAllNetworkInterfaces().Where(adapter =>
                    adapter.NetworkInterfaceType != NetworkInterfaceType.Loopback && adapter.OperationalStatus == OperationalStatus.Up).ToArray();
                var ids = adapters.Select(adapter => adapter.Id).ToHashSet(StringComparer.Ordinal);
                foreach (var stale in previousNetworks.Keys.Where(id => !ids.Contains(id)).ToArray()) previousNetworks.Remove(stale);
            }
            catch (NetworkInformationException) { adapters = []; }
            adapterRefresh = timestamp;
        }
        var result = new List<NetworkSample>(adapters.Length);
        foreach (var adapter in adapters)
        {
            try
            {
                var statistics = adapter.GetIPStatistics();
                previousNetworks.TryGetValue(adapter.Id, out var old);
                var send = old != default ? Metrics.Rate(old.Sent, statistics.BytesSent, elapsed) : 0;
                var receive = old != default ? Metrics.Rate(old.Received, statistics.BytesReceived, elapsed) : 0;
                var address = old.Address ?? string.Join(", ", adapter.GetIPProperties().UnicastAddresses
                    .Where(entry => entry.Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
                    .Select(entry => entry.Address));
                var ipv6 = old.Ipv6 ?? adapter.GetIPProperties().UnicastAddresses.Select(entry => entry.Address)
                    .Where(entry => entry.AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6)
                    .OrderBy(entry => entry.IsIPv6LinkLocal).FirstOrDefault()?.ToString() ?? "";
                result.Add(new NetworkSample(adapter.Id, adapter.Name, adapter.Description, address, adapter.Speed, send, receive)
                { Ipv6 = ipv6, Type = adapter.NetworkInterfaceType == NetworkInterfaceType.Wireless80211 ? "Wi-Fi" : "Ethernet" });
                previousNetworks[adapter.Id] = new PreviousNetwork(statistics.BytesSent, statistics.BytesReceived, address, ipv6);
            }
            catch (NetworkInformationException) { previousNetworks.Remove(adapter.Id); }
        }
        return result.ToArray();
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        Marshal.FreeHGlobal(buffer);
        buffer = 0;
        disks.Dispose();
        processors.Dispose();
        gpu.Dispose();
        metadata?.Dispose();
        trace?.Dispose();
    }

    private readonly record struct PreviousProcess(long Created, long Time, long Read, long Write, string Name);
    private readonly record struct PreviousNetwork(long Sent, long Received, string? Address, string? Ipv6);
}
