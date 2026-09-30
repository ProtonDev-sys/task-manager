using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace TaskManager.Monitoring;

internal static class ProcessActions
{
    private static readonly SemaphoreSlim dumpGate = new(1, 1);
    public static void Priority(ProcessSample process, ProcessPriorityClass priority)
    {
        if (priority == ProcessPriorityClass.RealTime) throw new InvalidOperationException("Realtime priority is intentionally unavailable to protect system responsiveness.");
        WithVerifiedHandle(process, 0x200, handle => { if (!SetPriorityClass(handle, (uint)priority)) throw new Win32Exception(); });
    }

    public static bool? EfficiencyMode(ProcessSample process)
    {
        var handle = NativeMethods.OpenProcess(0x1000, false, process.Id);
        if (handle == 0) return null;
        try
        {
            if (!NativeMethods.GetProcessTimes(handle, out var created, out _, out _, out _) || created != process.Created) return null;
            var state = new PowerThrottling { Version = 1 };
            return ReadPowerState(handle, ref state)
                ? (state.ControlMask & state.StateMask & 1) != 0 : null;
        }
        finally { NativeMethods.CloseHandle(handle); }
    }

    public static void EfficiencyMode(ProcessSample process, bool enabled)
    {
        WithVerifiedHandle(process, 0x200, handle =>
        {
            var previous = new PowerThrottling { Version = 1 };
            if (!ReadPowerState(handle, ref previous)) throw new Win32Exception();
            var state = previous;
            state.ControlMask |= 1;
            state.StateMask = enabled ? state.StateMask | 1u : state.StateMask & ~1u;
            if (!SetProcessInformation(handle, 4, ref state, (uint)Marshal.SizeOf<PowerThrottling>())) throw new Win32Exception();
            if (!SetPriorityClass(handle, (uint)(enabled ? ProcessPriorityClass.Idle : ProcessPriorityClass.Normal)))
            {
                var error = new Win32Exception();
                SetProcessInformation(handle, 4, ref previous, (uint)Marshal.SizeOf<PowerThrottling>());
                throw error;
            }
        });
    }

    private static bool ReadPowerState(nint process, ref PowerThrottling state)
    {
        var size = (uint)Marshal.SizeOf<PowerThrottling>();
        if (GetProcessInformation(process, 4, ref state, size)) return true;
        return NtQueryInformationProcess(process, 77, ref state, size, out var returned) >= 0 && returned == size && state.Version == 1;
    }

    public static void Dump(ProcessSample process, string destination)
    {
        var temporary = destination + "." + Guid.NewGuid().ToString("N") + ".tmp";
        dumpGate.Wait();
        try
        {
            WithVerifiedHandle(process, 0x410, handle =>
            {
                using var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                if (!MiniDumpWriteDump(handle, (uint)process.Id, output.SafeFileHandle, 2, 0, 0, 0)) throw new Win32Exception();
            });
            File.Move(temporary, destination, true);
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); }
            finally { dumpGate.Release(); }
        }
    }

    public static (ulong Process, ulong System) Affinity(ProcessSample process)
    {
        ulong selected = 0, system = 0;
        WithVerifiedHandle(process, 0, handle =>
        {
            if (!GetProcessAffinityMask(handle, out var mask, out var available)) throw new Win32Exception();
            selected = mask; system = available;
        });
        return (selected, system);
    }

    public static void Affinity(ProcessSample process, ulong mask)
    {
        if (mask == 0) throw new InvalidOperationException("Select at least one logical processor.");
        WithVerifiedHandle(process, 0x200, handle =>
        {
            if (!GetProcessAffinityMask(handle, out _, out var system)) throw new Win32Exception();
            if ((mask & ~system) != 0) throw new InvalidOperationException("The selected processor is unavailable.");
            if (!SetProcessAffinityMask(handle, (nuint)mask)) throw new Win32Exception();
        });
    }

    private static void WithVerifiedHandle(ProcessSample process, uint access, Action<nint> action)
    {
        if (process.Id <= 4 || process.Id == Environment.ProcessId) throw new InvalidOperationException("This process cannot be changed here.");
        var handle = NativeMethods.OpenProcess(0x1000 | access, false, process.Id);
        if (handle == 0) throw new Win32Exception();
        try
        {
            if (!NativeMethods.GetProcessTimes(handle, out var created, out _, out _, out _) || created != process.Created)
                throw new InvalidOperationException("The selected process has exited. Refresh and select it again.");
            if (!NativeMethods.IsProcessCritical(handle, out var critical)) throw new Win32Exception();
            if (critical) throw new InvalidOperationException("This Windows-critical process cannot be changed here.");
            action(handle);
        }
        finally { NativeMethods.CloseHandle(handle); }
    }

    public static void End(ProcessSample process)
    {
        if (process.Id <= 4 || process.Id == Environment.ProcessId)
            throw new InvalidOperationException("System processes and this task manager cannot be ended here.");
        var handle = NativeMethods.OpenProcess(0x1001, false, process.Id);
        if (handle == 0) throw new Win32Exception();
        try
        {
            if (!NativeMethods.GetProcessTimes(handle, out var created, out _, out _, out _)) throw new Win32Exception();
            if (created != process.Created) throw new InvalidOperationException("The selected process has exited. Refresh and select it again.");
            if (!NativeMethods.IsProcessCritical(handle, out var critical)) throw new Win32Exception();
            if (critical) throw new InvalidOperationException("Ending this critical process could crash Windows. The operation was refused.");
            if (!NativeMethods.TerminateProcess(handle, 1)) throw new Win32Exception();
        }
        finally { NativeMethods.CloseHandle(handle); }
    }

    public static string PathOf(ProcessSample sample)
    {
        using var process = Process.GetProcessById(sample.Id);
        if (process.StartTime.ToUniversalTime().ToFileTimeUtc() != sample.Created)
            throw new InvalidOperationException("The selected process has exited.");
        return process.MainModule?.FileName ?? throw new InvalidOperationException("Executable path is unavailable.");
    }

    public static ProcessPriorityClass? CurrentPriority(ProcessSample process)
    {
        var handle = NativeMethods.OpenProcess(0x1000, false, process.Id);
        if (handle == 0) return null;
        try
        {
            if (!NativeMethods.GetProcessTimes(handle, out var created, out _, out _, out _) || created != process.Created) return null;
            var value = GetPriorityClass(handle);
            return value == 0 ? null : (ProcessPriorityClass)value;
        }
        finally { NativeMethods.CloseHandle(handle); }
    }

    // Brings the process's top-level window to the foreground, restoring it if minimized.
    public static void SwitchTo(ProcessSample process)
    {
        nint found = 0;
        EnumWindows((window, _) =>
        {
            if (!IsWindowVisible(window) || GetWindow(window, 4) != 0) return true;
            GetWindowThreadProcessId(window, out var owner);
            if (owner != process.Id) return true;
            found = window;
            return false;
        }, 0);
        if (found == 0) throw new InvalidOperationException("This process has no window to switch to.");
        if (IsIconic(found)) ShowWindow(found, 9);
        SetForegroundWindow(found);
    }

    private delegate bool WindowCallback(nint window, nint parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, nint parameter);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] private static extern nint GetWindow(nint window, uint command);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out uint processId);
    [DllImport("user32.dll")] private static extern bool IsIconic(nint window);
    [DllImport("user32.dll")] private static extern bool ShowWindow(nint window, int command);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint window);
    [DllImport("kernel32.dll")] private static extern uint GetPriorityClass(nint process);
    [StructLayout(LayoutKind.Sequential)]
    private struct PowerThrottling { public uint Version; public uint ControlMask; public uint StateMask; }
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetProcessInformation(nint process, int informationClass, ref PowerThrottling information, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetProcessInformation(nint process, int informationClass, ref PowerThrottling information, uint size);
    [DllImport("ntdll.dll")] private static extern int NtQueryInformationProcess(nint process, int informationClass, ref PowerThrottling information, uint size, out uint returned);
    [DllImport("dbghelp.dll", SetLastError = true)] private static extern bool MiniDumpWriteDump(nint process, uint processId, Microsoft.Win32.SafeHandles.SafeFileHandle file, uint type, nint exception, nint streams, nint callback);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetPriorityClass(nint process, uint priority);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetProcessAffinityMask(nint process, out nuint processMask, out nuint systemMask);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetProcessAffinityMask(nint process, nuint mask);
}
