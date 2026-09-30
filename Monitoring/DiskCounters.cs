using System.Runtime.InteropServices;

namespace TaskManager.Monitoring;

internal sealed class DiskCounters : IDisposable
{
    private nint query;
    private nint buffer;
    private uint capacity;
    private readonly nint active;
    private readonly nint reads;
    private readonly nint writes;
    private readonly nint response;
    private readonly Dictionary<int, DiskHardware> hardware = new();
    private bool primed;

    public DiskCounters()
    {
        if (PdhOpenQueryW(null, 0, out query) != 0) return;
        PdhAddEnglishCounterW(query, @"\PhysicalDisk(*)\% Idle Time", 0, out active);
        PdhAddEnglishCounterW(query, @"\PhysicalDisk(*)\Disk Read Bytes/sec", 0, out reads);
        PdhAddEnglishCounterW(query, @"\PhysicalDisk(*)\Disk Write Bytes/sec", 0, out writes);
        PdhAddEnglishCounterW(query, @"\PhysicalDisk(*)\Avg. Disk sec/Transfer", 0, out response);
    }

    public DiskSample[] Sample()
    {
        if (query == 0 || PdhCollectQueryData(query) != 0) return [];
        var idleValues = Values(active);
        var readValues = Values(reads);
        var writeValues = Values(writes);
        var responses = Values(response);
        var result = idleValues.Keys.Concat(readValues.Keys).Concat(writeValues.Keys)
            .Distinct(StringComparer.Ordinal).Where(name => name != "_Total").Order(StringComparer.Ordinal)
            .Select(name => new DiskSample(name,
                primed && idleValues.TryGetValue(name, out var idle) ? Math.Clamp(100 - idle, 0, 100) : null,
                primed && readValues.TryGetValue(name, out var read) ? Math.Max(0, read) : null,
                primed && writeValues.TryGetValue(name, out var write) ? Math.Max(0, write) : null)
                { ResponseMilliseconds = primed && responses.TryGetValue(name, out var latency) ? Math.Max(0, latency * 1000) : null,
                    Hardware = Hardware(name) }).ToArray();
        primed = true;
        return result;
    }

    private DiskHardware? Hardware(string name)
    {
        if (!int.TryParse(name.Split(' ', 2)[0], out var index)) return null;
        if (!hardware.TryGetValue(index, out var information)) hardware[index] = information = HardwareInventory.Disk(index);
        return information;
    }

    private Dictionary<string, double> Values(nint counter)
    {
        var values = new Dictionary<string, double>(StringComparer.Ordinal);
        if (counter == 0) return values;
        uint needed = 0;
        var status = PdhGetFormattedCounterArrayW(counter, 0x200, ref needed, out _, 0);
        if (status != 0x800007D2 || needed == 0 || needed > 8 * 1024 * 1024) return values;
        if (needed > capacity)
        {
            if (buffer != 0) Marshal.FreeHGlobal(buffer);
            capacity = needed;
            buffer = Marshal.AllocHGlobal(checked((int)capacity));
        }
        var size = capacity;
        if (PdhGetFormattedCounterArrayW(counter, 0x200, ref size, out var count, buffer) != 0) return values;
        var stride = Marshal.SizeOf<CounterItem>();
        if ((ulong)count * (uint)stride > size) return values;
        for (var index = 0; index < count; index++)
        {
            var item = Marshal.PtrToStructure<CounterItem>(buffer + index * stride);
            if (item.Value.Status > 1 || !double.IsFinite(item.Value.Number)) continue;
            var name = Marshal.PtrToStringUni(item.Name);
            if (name != null) values[name] = item.Value.Number;
        }
        return values;
    }

    public void Dispose()
    {
        if (query != 0) { PdhCloseQuery(query); query = 0; }
        if (buffer != 0) { Marshal.FreeHGlobal(buffer); buffer = 0; }
    }

    [StructLayout(LayoutKind.Explicit, Size = 16)]
    private struct CounterValue
    {
        [FieldOffset(0)] public uint Status;
        [FieldOffset(8)] public double Number;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CounterItem { public nint Name; public CounterValue Value; }

    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhOpenQueryW(string? source, nuint userData, out nint query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhAddEnglishCounterW(nint query, string path, nuint userData, out nint counter);
    [DllImport("pdh.dll")]
    private static extern uint PdhCollectQueryData(nint query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhGetFormattedCounterArrayW(nint counter, uint format, ref uint size, out uint count, nint items);
    [DllImport("pdh.dll")]
    private static extern uint PdhCloseQuery(nint query);
}
