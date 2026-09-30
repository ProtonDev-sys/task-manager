using System.Runtime.InteropServices;

namespace TaskManager.Monitoring;

internal sealed class CounterQuery : IDisposable
{
    private nint query;
    private nint buffer;
    private uint capacity;
    private readonly Dictionary<string, nint> handles = new(StringComparer.Ordinal);
    private readonly Dictionary<string, List<string>> names = new(StringComparer.Ordinal);

    public CounterQuery(params string[] paths)
    {
        if (PdhOpenQueryW(null, 0, out query) != 0) return;
        foreach (var path in paths)
            if (PdhAddEnglishCounterW(query, path, 0, out var counter) == 0) { handles[path] = counter; names[path] = []; }
    }

    public bool Collect() => query != 0 && PdhCollectQueryData(query) == 0;

    public unsafe Dictionary<string, double> Values(string path)
    {
        var result = new Dictionary<string, double>(StringComparer.Ordinal);
        if (!handles.TryGetValue(path, out var counter)) return result;
        uint needed = 0;
        if (PdhGetFormattedCounterArrayW(counter, 0x8200, ref needed, out _, 0) != 0x800007D2 || needed == 0 || needed > 16 * 1024 * 1024) return result;
        if (needed > capacity)
        {
            if (buffer != 0) Marshal.FreeHGlobal(buffer);
            capacity = needed;
            buffer = Marshal.AllocHGlobal((int)capacity);
        }
        var size = capacity;
        if (PdhGetFormattedCounterArrayW(counter, 0x8200, ref size, out var count, buffer) != 0 || count * 24ul > size) return result;
        var cached = names[path];
        for (var index = 0; index < count; index++)
        {
            var entry = buffer + index * 24;
            if (Marshal.ReadInt32(entry, 8) is not (0 or 1)) continue;
            var value = *(double*)(entry + 16);
            if (!double.IsFinite(value)) continue;
            var pointer = Marshal.ReadIntPtr(entry);
            var offset = (long)pointer - (long)buffer;
            if (offset < 0 || offset + 2 > size || offset % 2 != 0) continue;
            var span = new ReadOnlySpan<char>((void*)pointer, (int)(size - offset) / 2);
            var terminator = span.IndexOf('\0');
            if (terminator < 0) continue;
            span = span[..terminator];
            while (cached.Count <= index) cached.Add("");
            if (!span.SequenceEqual(cached[index])) cached[index] = span.ToString();
            result[cached[index]] = value;
        }
        return result;
    }

    public void Dispose()
    {
        if (query != 0) { PdhCloseQuery(query); query = 0; }
        if (buffer != 0) { Marshal.FreeHGlobal(buffer); buffer = 0; }
    }

    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhOpenQueryW(string? source, nuint userData, out nint query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhAddEnglishCounterW(nint query, string path, nuint userData, out nint counter);
    [DllImport("pdh.dll")] private static extern uint PdhCollectQueryData(nint query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhGetFormattedCounterArrayW(nint counter, uint format, ref uint size, out uint count, nint items);
    [DllImport("pdh.dll")] private static extern uint PdhCloseQuery(nint query);
}
