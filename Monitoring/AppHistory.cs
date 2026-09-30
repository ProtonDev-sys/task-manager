using System.Text.Json;

namespace TaskManager.Monitoring;

internal sealed record HistoryEntry(string Name, string Key, bool Packaged, long CpuTicks, long NetworkBytes);
internal sealed class AppHistory
{
    private readonly Dictionary<string, HistoryEntry> entries = new(StringComparer.OrdinalIgnoreCase);
    private Dictionary<(int Id, long Created), (long Cpu, long Network)> previous = new();
    private Dictionary<(int Id, long Created), (long Cpu, long Network)> scratch = new();
    public DateTime Started { get; private set; } = DateTime.Now;
    private readonly bool persist;
    private static string PathName => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TaskManagerClone", "history.json");
    public AppHistory(bool persist)
    {
        this.persist = persist;
        if (!persist || !File.Exists(PathName)) return;
        try
        {
            if (new FileInfo(PathName).Length > 2 * 1024 * 1024) return;
            var saved = JsonSerializer.Deserialize<SavedHistory>(File.ReadAllText(PathName));
            if (saved?.Entries == null) return;
            Started = saved.Started;
            foreach (var entry in saved.Entries.Take(2048))
                if (entry.CpuTicks >= 0 && entry.NetworkBytes >= 0) entries[entry.Key] = entry;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { }
    }

    public void Update(SystemSample sample)
    {
        scratch.Clear();
        var next = scratch;
        foreach (var process in sample.Processes)
        {
            if (process.Id <= 0) continue;
            var identity = (process.Id, process.Created);
            next[identity] = (process.CpuTime, process.NetworkBytes);
            if (!previous.TryGetValue(identity, out var old) || !sample.Metadata.TryGetValue(identity, out var metadata)) continue;
            var cpuDelta = Math.Max(0, process.CpuTime - old.Cpu);
            var networkDelta = Math.Max(0, process.NetworkBytes - old.Network);
            if (cpuDelta == 0 && networkDelta == 0) continue;
            var key = metadata.Path ?? process.Name;
            if (!entries.TryGetValue(key, out var entry))
            {
                if (entries.Count >= 2048) continue;
                entry = new HistoryEntry(metadata.DisplayName, key, metadata.Packaged, 0, 0);
            }
            entries[key] = entry with { CpuTicks = entry.CpuTicks + cpuDelta, NetworkBytes = entry.NetworkBytes + networkDelta };
        }
        (previous, scratch) = (scratch, previous);
    }

    public HistoryEntry[] Entries(bool all) => entries.Values.Where(entry => all || entry.Packaged).OrderByDescending(entry => entry.CpuTicks).ToArray();

    public void Clear()
    {
        entries.Clear();
        Started = DateTime.Now;
        Save();
    }

    public void Save()
    {
        if (!persist) return;
        Directory.CreateDirectory(Path.GetDirectoryName(PathName)!);
        var temporary = PathName + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(new SavedHistory(Started, entries.Values.ToArray())));
        File.Move(temporary, PathName, true);
    }

    private sealed record SavedHistory(DateTime Started, HistoryEntry[] Entries);
}
