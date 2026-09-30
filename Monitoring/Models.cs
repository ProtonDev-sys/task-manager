namespace TaskManager.Monitoring;

internal sealed record ProcessSample(int Id, int ParentId, string Name, long Created, double Cpu,
    long WorkingSet, long PrivateBytes, int Threads, int Handles, int Session, double ReadRate, double WriteRate)
{
    public string Status { get; init; } = "";
    public double? DiskRate { get; init; }
    public double? NetworkRate { get; init; }
    public long NetworkBytes { get; init; }
    public long CpuTime { get; init; }
    public double? Gpu { get; init; }
    public string GpuEngine { get; init; } = "";
}

internal sealed record DiskSample(string Name, double? Active, double? ReadRate, double? WriteRate)
{
    public double? ResponseMilliseconds { get; init; }
    public DiskHardware? Hardware { get; init; }
}

internal sealed record NetworkSample(string Id, string Name, string Description, string Address,
    long Speed, double SendRate, double ReceiveRate)
{
    public string Ipv6 { get; init; } = "";
    public string Type { get; init; } = "Ethernet";
}

internal sealed record SystemSample(DateTime Timestamp, double Cpu, ulong TotalMemory, ulong AvailableMemory,
    ulong CommitBytes, ulong CommitLimit, ulong CachedBytes, ulong PagedPool, ulong NonPagedPool,
    int ThreadCount, int HandleCount, TimeSpan Uptime, ProcessSample[] Processes,
    DiskSample[] Disks, NetworkSample[] Networks, double SampleMilliseconds)
{
    public IReadOnlyDictionary<(int Id, long Created), ProcessMetadata> Metadata { get; init; } = new Dictionary<(int, long), ProcessMetadata>();
    public IReadOnlyDictionary<int, ServiceEntry[]> Services { get; init; } = new Dictionary<int, ServiceEntry[]>();
    public double[] CpuCores { get; init; } = [];
    public string? IoError { get; init; }
    public GpuSample[] Gpus { get; init; } = [];
    public double? CpuSpeedFactor { get; init; }
    public UserSession[] Sessions { get; init; } = [];
    public double GpuMilliseconds { get; init; }
    public double MetadataMilliseconds { get; init; }
    public int ProcessCount { get; init; }
    public double MonotonicSeconds { get; init; }
    public double ProcessMilliseconds { get; init; }
    public double DeviceMilliseconds { get; init; }
    public double InventoryMilliseconds { get; init; }
    public double AttributionMilliseconds { get; init; }
}

internal static class Metrics
{
    public static double CpuPercent(long previous, long current, double seconds, int processors) =>
        seconds > 0 && processors > 0 && current >= previous
            ? Math.Clamp((current - previous) / (seconds * 100_000d * processors), 0, 100)
            : 0;

    public static double Rate(long previous, long current, double seconds) =>
        seconds > 0 && current >= previous ? (current - previous) / seconds : 0;

    public static string CpuTime(long ticks)
    {
        var duration = TimeSpan.FromTicks(Math.Max(0, ticks));
        return $"{(long)duration.TotalHours}:{duration.Minutes:00}:{duration.Seconds:00}";
    }

    public static string Bytes(double bytes)
    {
        string[] units = ["B", "KB", "MB", "GB", "TB"];
        var unit = 0;
        while (bytes >= 1024 && unit < units.Length - 1) { bytes /= 1024; unit++; }
        return $"{bytes:0.0} {units[unit]}";
    }

    public static string Bits(double bytes) => bytes >= 125_000_000 ? Tenths(bytes / 125_000_000, " Gbps")
        : bytes >= 125_000 ? Tenths(bytes / 125_000, " Mbps") : Tenths(bytes / 125, " Kbps");

    // Task Manager prints zero without a decimal ("0%", "0 MB") and everything else to one decimal place.
    private static string Tenths(double value, string suffix, string format = "0.0")
    {
        var rounded = Math.Round(value, 1);
        return rounded == 0 ? "0" + suffix : rounded.ToString(format) + suffix;
    }

    public static string Percent(double value) => Tenths(value, "%");
    public static string Megabytes(double bytes) => Tenths(bytes / 1048576d, " MB", "N1");
    public static string DiskRate(double bytesPerSecond) => Tenths(bytesPerSecond / 1048576d, " MB/s");
    public static string NetworkRate(double bytesPerSecond) => Tenths(bytesPerSecond / 125_000d, " Mbps");
    public static bool IsZero(double value, double scale = 1) => Math.Round(value / scale, 1) == 0;
}
