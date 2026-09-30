using System.Runtime.InteropServices;

namespace TaskManager.Monitoring;

internal sealed class ProcessorCounters : IDisposable
{
    private readonly int count = Environment.ProcessorCount;
    private readonly nint buffer;
    private readonly long[] idle;
    private readonly long[] total;
    private bool primed;
    private long previousInterrupt;
    private long previousAggregate;
    public double InterruptUsage { get; private set; }

    public ProcessorCounters()
    {
        buffer = Marshal.AllocHGlobal(count * 48);
        idle = new long[count];
        total = new long[count];
    }

    public double[] Sample()
    {
        if (NativeMethods.NtQuerySystemInformation(8, buffer, (uint)(count * 48), out var returned) < 0) return [];
        var available = Math.Min(count, (int)returned / 48);
        var result = new double[available];
        long aggregate = 0, interrupts = 0;
        for (var index = 0; index < available; index++)
        {
            var entry = buffer + index * 48;
            var nextIdle = Marshal.ReadInt64(entry);
            var nextTotal = Marshal.ReadInt64(entry, 8) + Marshal.ReadInt64(entry, 16);
            var elapsed = nextTotal - total[index];
            result[index] = primed && elapsed > 0 ? Math.Clamp(100d * (elapsed - (nextIdle - idle[index])) / elapsed, 0, 100) : 0;
            idle[index] = nextIdle;
            total[index] = nextTotal;
            aggregate += nextTotal;
            interrupts += Marshal.ReadInt64(entry, 24) + Marshal.ReadInt64(entry, 32);
        }
        var delta = aggregate - previousAggregate;
        InterruptUsage = primed && delta > 0 ? Math.Clamp(100d * (interrupts - previousInterrupt) / delta, 0, 100) : 0;
        previousInterrupt = interrupts;
        previousAggregate = aggregate;
        primed = true;
        return result;
    }

    public void Dispose() => Marshal.FreeHGlobal(buffer);
}
