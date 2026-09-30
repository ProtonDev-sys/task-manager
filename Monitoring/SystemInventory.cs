using Microsoft.Win32;
using System.Runtime.InteropServices;

namespace TaskManager.Monitoring;

internal sealed record CpuInventory(string Name, string BaseSpeed, int Cores, int Sockets)
{
    public long[] Caches { get; init; } = new long[3];
    public bool VirtualizationEnabled { get; init; }
}
internal sealed record StartupEntry(string Name, string Command, string Location, string Status)
{
    public string Publisher { get; init; } = "";
    public string DisplayName { get; init; } = "";
    public string? ExecutablePath { get; init; }
    public RegistryHive Hive { get; init; }
    public RegistryView View { get; init; }
    public string? ApprovalPath { get; init; }
    public string? ValueName { get; init; }
    public byte[]? Approval { get; init; }
}
internal sealed record ServiceEntry(string Name, string DisplayName, int ProcessId, string Status)
{
    public string Group { get; init; } = "";
}

internal static class SystemInventory
{
    public static CpuInventory Cpu()
    {
        using var key = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0");
        var name = (key?.GetValue("ProcessorNameString") as string)?.Trim() ?? "Processor";
        var mhz = key?.GetValue("~MHz") is int speed ? $"{speed / 1000d:0.00} GHz" : "—";
        return new CpuInventory(name, mhz, CountTopology(0), CountTopology(3))
        { Caches = ReadCaches(), VirtualizationEnabled = IsProcessorFeaturePresent(21) };
    }

    private static long[] ReadCaches()
    {
        var caches = new long[3];
        uint size = 0;
        NativeMethods.GetLogicalProcessorInformationEx(2, 0, ref size);
        if (size == 0 || size > 1024 * 1024) return caches;
        var buffer = Marshal.AllocHGlobal((int)size);
        try
        {
            if (!NativeMethods.GetLogicalProcessorInformationEx(2, buffer, ref size)) return caches;
            var offset = 0;
            while (offset <= size - 16)
            {
                var recordSize = Marshal.ReadInt32(buffer + offset, 4);
                if (recordSize < 16 || recordSize > size - offset) break;
                var level = Marshal.ReadByte(buffer + offset, 8);
                if (Marshal.ReadInt32(buffer + offset) == 2 && level is >= 1 and <= 3)
                    caches[level - 1] += unchecked((uint)Marshal.ReadInt32(buffer + offset, 12));
                offset += recordSize;
            }
            return caches;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsProcessorFeaturePresent(uint feature);

    private static int CountTopology(int relationship)
    {
        uint size = 0;
        NativeMethods.GetLogicalProcessorInformationEx(relationship, 0, ref size);
        if (size == 0 || size > 1024 * 1024) return 0;
        var buffer = Marshal.AllocHGlobal((int)size);
        try
        {
            if (!NativeMethods.GetLogicalProcessorInformationEx(relationship, buffer, ref size)) return 0;
            var count = 0;
            var offset = 0;
            while (offset <= size - 8)
            {
                var recordSize = Marshal.ReadInt32(buffer + offset, 4);
                if (recordSize < 8 || recordSize > size - offset) return 0;
                if (Marshal.ReadInt32(buffer + offset) == relationship) count++;
                offset += recordSize;
            }
            return count;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    public static StartupEntry[] Startup()
    {
        var result = new List<StartupEntry>();
        foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
        foreach (var view in hive == RegistryHive.CurrentUser ? new[] { RegistryView.Registry64 } : new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            using var root = RegistryKey.OpenBaseKey(hive, view);
            using var run = root.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Run");
            using var approval = root.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\" +
                (view == RegistryView.Registry32 ? "Run32" : "Run"));
            if (run == null) continue;
            foreach (var name in run.GetValueNames())
            {
                if (run.GetValue(name) is not string command) continue;
                var state = approval?.GetValue(name) as byte[];
                var status = state is { Length: >= 4 } ? state[0] switch { 3 or 7 => "Disabled", _ => "Enabled" } : "Enabled";
                result.Add(new StartupEntry(name, command, $"{hive} · {view}", status)
                { Hive = hive, View = view, ValueName = name, Approval = state,
                    ApprovalPath = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\" + (view == RegistryView.Registry32 ? "Run32" : "Run") });
            }
        }
        foreach (var folder in new[] { Environment.SpecialFolder.Startup, Environment.SpecialFolder.CommonStartup })
        {
            var path = Environment.GetFolderPath(folder);
            if (!Directory.Exists(path)) continue;
            var hive = folder == Environment.SpecialFolder.Startup ? RegistryHive.CurrentUser : RegistryHive.LocalMachine;
            using var root = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);
            var approvalPath = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder";
            using var approval = root.OpenSubKey(approvalPath);
            foreach (var file in Directory.EnumerateFiles(path).Where(file => !file.EndsWith("desktop.ini", StringComparison.OrdinalIgnoreCase)))
            {
                var name = Path.GetFileName(file);
                var state = approval?.GetValue(name) as byte[];
                var status = state is { Length: >= 4 } ? state[0] switch { 3 or 7 => "Disabled", _ => "Enabled" } : "Enabled";
                result.Add(new StartupEntry(Path.GetFileNameWithoutExtension(file), file, folder.ToString(), status)
                { Hive = hive, View = RegistryView.Registry64, ApprovalPath = approvalPath, ValueName = name, Approval = state });
            }
        }
        return result.Distinct().Select(DescribeStartup).OrderBy(entry => entry.DisplayName, StringComparer.CurrentCultureIgnoreCase).ToArray();
    }

    private static StartupEntry DescribeStartup(StartupEntry entry)
    {
        var command = Environment.ExpandEnvironmentVariables(entry.Command.Trim());
        var executable = command.StartsWith('"') ? command.Split('"').ElementAtOrDefault(1) : command.Split(' ', 2)[0];
        entry = entry with { DisplayName = entry.Name };
        if (string.IsNullOrWhiteSpace(executable) || !File.Exists(executable)) return entry with { ExecutablePath = File.Exists(entry.Command) ? entry.Command : null };
        try
        {
            var version = System.Diagnostics.FileVersionInfo.GetVersionInfo(executable);
            return entry with { ExecutablePath = executable, Publisher = version.CompanyName?.Trim() ?? "",
                DisplayName = string.IsNullOrWhiteSpace(version.FileDescription) ? entry.Name : version.FileDescription.Trim() };
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        { return entry with { ExecutablePath = executable }; }
    }

    public static ServiceEntry[] Services()
    {
        var manager = OpenSCManagerW(null, null, 4);
        if (manager == 0) throw new System.ComponentModel.Win32Exception();
        nint buffer = 0;
        try
        {
            uint resume = 0;
            EnumServicesStatusExW(manager, 0, 0x30, 3, 0, 0, out var needed, out _, ref resume, null);
            if (needed == 0) return [];
            if (needed > 8 * 1024 * 1024) throw new InvalidOperationException("Service inventory exceeded its safety bound.");
            buffer = Marshal.AllocHGlobal(checked((int)needed));
            resume = 0;
            if (!EnumServicesStatusExW(manager, 0, 0x30, 3, buffer, needed, out _, out var count, ref resume, null))
                throw new System.ComponentModel.Win32Exception();
            var stride = Marshal.SizeOf<ServiceStatus>();
            var result = new ServiceEntry[count];
            for (var index = 0; index < count; index++)
            {
                var entry = Marshal.PtrToStructure<ServiceStatus>(buffer + index * stride);
                var name = Marshal.PtrToStringUni(entry.Name) ?? "";
                result[index] = new ServiceEntry(name, Marshal.PtrToStringUni(entry.DisplayName) ?? "",
                    (int)entry.ProcessId, entry.CurrentState switch
                    { 1 => "Stopped", 2 => "Start pending", 3 => "Stop pending", 4 => "Running", 5 => "Continue pending", 6 => "Pause pending", 7 => "Paused", _ => "Unknown" })
                { Group = ServiceGroup(name) };
            }
            return result.OrderBy(entry => entry.Name, StringComparer.OrdinalIgnoreCase).ToArray();
        }
        finally
        {
            if (buffer != 0) Marshal.FreeHGlobal(buffer);
            CloseServiceHandle(manager);
        }
    }

    private static readonly Dictionary<string, string> serviceGroups = new(StringComparer.OrdinalIgnoreCase);

    // The svchost group ("-k netsvcs") from the service's image path; blank for services in their own process.
    public static string ServiceGroup(string service)
    {
        lock (serviceGroups)
        {
            if (serviceGroups.TryGetValue(service, out var cached)) return cached;
            var group = "";
            try
            {
                using var key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\" + service);
                var image = key?.GetValue("ImagePath", "", RegistryValueOptions.DoNotExpandEnvironmentNames) as string ?? "";
                var marker = image.IndexOf(" -k ", StringComparison.OrdinalIgnoreCase);
                if (marker >= 0) group = image[(marker + 4)..].Trim().Split(' ')[0];
            }
            catch (Exception error) when (error is System.Security.SecurityException or UnauthorizedAccessException or IOException) { }
            if (serviceGroups.Count < 4096) serviceGroups[service] = group;
            return group;
        }
    }

    // Windows 10 Task Manager captions for shared svchost groups.
    public static string ServiceGroupCaption(string group) => group.ToLowerInvariant() switch
    {
        "netsvcs" => "Local System", "localsystemnetworkrestricted" => "Local System (Network Restricted)",
        "localservice" => "Local Service", "localservicenetworkrestricted" => "Local Service (Network Restricted)",
        "localservicenonetwork" => "Local Service (No Network)", "localservicenonetworkfirewall" => "Local Service (No Network Firewall)",
        "localserviceandnoimpersonation" => "Local Service (No Impersonation)", "networkservice" => "Network Service",
        "networkservicenetworkrestricted" => "Network Service (Network Restricted)", "networkserviceandnoimpersonation" => "Network Service (No Impersonation)",
        "rpcss" => "Remote Procedure Call", "dcomlaunch" => "DCOM Server Process Launcher", "unistacksvcgroup" => "Unistack Service Group",
        "wbiosvcgroup" => "Windows Biometric", "wersvcgroup" => "Windows Error Reporting", "print" => "Print Workflow",
        _ => group
    };

    // Firmware POST time recorded by Windows, as shown on the Startup tab.
    public static double? LastBiosTime()
    {
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Session Manager\Power");
            return key?.GetValue("FwPOSTTime") is int milliseconds && milliseconds > 0 ? milliseconds / 1000d : null;
        }
        catch (Exception error) when (error is System.Security.SecurityException or UnauthorizedAccessException or IOException) { return null; }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ServiceStatus
    {
        public nint Name;
        public nint DisplayName;
        public uint ServiceType;
        public uint CurrentState;
        public uint ControlsAccepted;
        public uint Win32ExitCode;
        public uint ServiceSpecificExitCode;
        public uint CheckPoint;
        public uint WaitHint;
        public uint ProcessId;
        public uint ServiceFlags;
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern nint OpenSCManagerW(string? machine, string? database, uint access);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumServicesStatusExW(nint manager, int level, uint type, uint state, nint buffer,
        uint size, out uint needed, out uint count, ref uint resume, string? group);
    [DllImport("advapi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseServiceHandle(nint handle);
}
