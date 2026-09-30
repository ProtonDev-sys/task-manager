using System.Globalization;
using System.Runtime.InteropServices;

namespace TaskManager.Monitoring;

internal sealed record GpuSample(string Id, int Index, string Name, double Usage, double Dedicated, double Shared,
    ulong DedicatedLimit, ulong SharedLimit, IReadOnlyDictionary<string, double> Engines);
internal readonly record struct ProcessGpu(double Usage, string Engine);

internal sealed class GpuCounters : IDisposable
{
    private const string EnginePath = @"\GPU Engine(*)\Utilization Percentage";
    private const string DedicatedPath = @"\GPU Adapter Memory(*)\Dedicated Usage";
    private const string SharedPath = @"\GPU Adapter Memory(*)\Shared Usage";
    private const string SpeedPath = @"\Processor Information(_Total)\% Processor Performance";
    private readonly CounterQuery query = new(EnginePath, DedicatedPath, SharedPath, SpeedPath);
    private readonly Dictionary<string, Adapter> adapters = ReadAdapters();
    private readonly Dictionary<string, EngineIdentity?> identities = new(StringComparer.Ordinal);
    private bool primed;
    public double? SpeedFactor { get; private set; }

    public (GpuSample[] Adapters, Dictionary<int, ProcessGpu> Processes) Sample()
    {
        var processUsage = new Dictionary<int, ProcessGpu>();
        if (!query.Collect()) return ([], processUsage);
        var engines = query.Values(EnginePath);
        var dedicated = query.Values(DedicatedPath);
        var shared = query.Values(SharedPath);
        var speed = query.Values(SpeedPath);
        SpeedFactor = primed && speed.Count > 0 ? speed.Values.First() / 100 : null;
        var aggregate = new Dictionary<string, Dictionary<string, double>>(StringComparer.Ordinal);
        foreach (var entry in engines)
        {
            if (!identities.TryGetValue(entry.Key, out var identity)) identities[entry.Key] = identity = Parse(entry.Key);
            if (identity == null) continue;
            if (!aggregate.TryGetValue(identity.AdapterId, out var values)) aggregate[identity.AdapterId] = values = new(StringComparer.Ordinal);
            values.TryGetValue(identity.Engine, out var previous);
            values[identity.Engine] = previous + Math.Max(0, entry.Value);
            if (!primed) continue;
            var value = Math.Clamp(entry.Value, 0, 100);
            if (!processUsage.TryGetValue(identity.ProcessId, out var old) || old.Usage < value)
                processUsage[identity.ProcessId] = new ProcessGpu(value, $"GPU {(adapters.TryGetValue(identity.AdapterId, out var adapter) ? adapter.Index : 0)} - {identity.EngineType}");
        }
        var result = new List<GpuSample>();
        foreach (var entry in adapters)
        {
            if (!aggregate.TryGetValue(entry.Key, out var values)) continue;
            var usage = values.ToDictionary(engine => engine.Key, engine => primed ? Math.Clamp(engine.Value, 0, 100) : 0, StringComparer.Ordinal);
            var dedicatedUsage = dedicated.Where(value => value.Key.Contains(entry.Key, StringComparison.Ordinal)).Sum(value => value.Value);
            var sharedUsage = shared.Where(value => value.Key.Contains(entry.Key, StringComparison.Ordinal)).Sum(value => value.Value);
            result.Add(new GpuSample(entry.Key, entry.Value.Index, entry.Value.Name, usage.Values.DefaultIfEmpty().Max(),
                Math.Max(0, dedicatedUsage), Math.Max(0, sharedUsage), entry.Value.Dedicated, entry.Value.Shared, usage));
        }
        primed = true;
        if (identities.Count > engines.Count + 128)
            foreach (var obsolete in identities.Keys.Where(key => !engines.ContainsKey(key)).ToArray()) identities.Remove(obsolete);
        return (result.OrderBy(adapter => adapter.Index).ToArray(), processUsage);
    }

    private static EngineIdentity? Parse(string name)
    {
        var luid = name.IndexOf("luid_", StringComparison.Ordinal);
        var physical = name.IndexOf("_phys_", StringComparison.Ordinal);
        var engine = name.IndexOf("_eng_", StringComparison.Ordinal);
        var type = name.IndexOf("_engtype_", StringComparison.Ordinal);
        if (!name.StartsWith("pid_", StringComparison.Ordinal) || luid < 5 || physical <= luid || engine <= physical || type <= engine) return null;
        if (!int.TryParse(name.AsSpan(4, luid - 5), out var processId)) return null;
        return new EngineIdentity(processId, name[luid..physical], name[(engine + 5)..type] + " - " + name[(type + 9)..], name[(type + 9)..]);
    }

    private static unsafe Dictionary<string, Adapter> ReadAdapters()
    {
        var result = new Dictionary<string, Adapter>(StringComparer.Ordinal);
        var interfaceId = new Guid("770aae78-f26f-4dba-a829-253c83d1b387");
        if (CreateDXGIFactory1(ref interfaceId, out var factory) < 0) return result;
        try
        {
            var enumerate = (delegate* unmanaged[Stdcall]<nint, uint, nint*, int>)(*(nint**)factory)[12];
            for (uint index = 0; index < 32; index++)
            {
                nint adapter = 0;
                if (enumerate(factory, index, &adapter) < 0) break;
                try
                {
                    var describe = (delegate* unmanaged[Stdcall]<nint, Description*, int>)(*(nint**)adapter)[10];
                    Description description;
                    if (describe(adapter, &description) < 0 || (description.Flags & 2) != 0) continue;
                    var id = $"luid_0x{unchecked((uint)description.LuidHigh):x8}_0x{description.LuidLow:x8}";
                    result[id] = new Adapter((int)index, new string(description.Name), description.DedicatedVideoMemory, description.SharedSystemMemory);
                }
                finally { ((delegate* unmanaged[Stdcall]<nint, uint>)(*(nint**)adapter)[2])(adapter); }
            }
        }
        finally { ((delegate* unmanaged[Stdcall]<nint, uint>)(*(nint**)factory)[2])(factory); }
        return result;
    }

    public void Dispose() => query.Dispose();
    private sealed record Adapter(int Index, string Name, ulong Dedicated, ulong Shared);
    private sealed record EngineIdentity(int ProcessId, string AdapterId, string Engine, string EngineType);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private unsafe struct Description
    {
        public fixed char Name[128];
        public uint Vendor, Device, Subsystem, Revision;
        public ulong DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
        public uint LuidLow;
        public int LuidHigh;
        public uint Flags;
    }
    [DllImport("dxgi.dll")] private static extern int CreateDXGIFactory1(ref Guid interfaceId, out nint factory);
}
