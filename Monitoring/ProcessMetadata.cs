using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace TaskManager.Monitoring;

internal sealed record ProcessMetadata(string DisplayName, string? Path, string Publisher, Icon? Icon, bool IsApp)
{
    public bool Packaged { get; init; }
    public string? Package { get; init; }
    public string? PackageName { get; init; }
    public string Description { get; init; } = "";
    public string UserName { get; init; } = "—";
    public string Virtualization { get; init; } = "Not allowed";
}

internal sealed class MetadataCatalog : IDisposable
{
    private readonly Dictionary<(int Id, long Created), ProcessMetadata> identities = new();
    private readonly HashSet<(int Id, long Created)> live = new();
    private IReadOnlyDictionary<(int Id, long Created), ProcessMetadata> snapshot = new Dictionary<(int, long), ProcessMetadata>();
    private readonly Dictionary<string, ProcessMetadata> files = new(StringComparer.OrdinalIgnoreCase);
    private readonly StringBuilder pathBuffer = new(32768);
    private readonly Dictionary<string, string> accounts = new(StringComparer.Ordinal);
    private HashSet<int> apps = [];
    private DateTime windowsUpdated;
    private bool primed;

    public IReadOnlyDictionary<(int Id, long Created), ProcessMetadata> Update(ProcessSample[] processes, IReadOnlyDictionary<int, ServiceEntry[]> services)
    {
        if ((DateTime.UtcNow - windowsUpdated).TotalSeconds >= 3)
        {
            apps = [];
            EnumWindows((window, _) =>
            {
                if (IsWindowVisible(window) && GetWindow(window, 4) == 0 && GetWindowTextLengthW(window) > 0 && !Cloaked(window))
                {
                    GetWindowThreadProcessId(window, out var processId);
                    apps.Add((int)processId);
                }
                return true;
            }, 0);
            windowsUpdated = DateTime.UtcNow;
        }
        RefreshUsers();
        live.Clear();
        var changed = false;
        // The first pass reads everything so the list is complete immediately; afterwards new files are read a few per tick.
        var budget = primed ? 24 : int.MaxValue;
        primed = true;
        foreach (var process in processes)
        {
            var key = (process.Id, process.Created);
            live.Add(key);
            if (!identities.TryGetValue(key, out var cached))
            {
                var fresh = Read(process, ref budget);
                if (fresh == null) continue;
                identities[key] = cached = fresh;
                changed = true;
            }
            var named = Named(process, cached, services);
            var isApp = apps.Contains(process.Id);
            if (!ReferenceEquals(named, cached) || cached.IsApp != isApp)
            {
                identities[key] = named with { IsApp = isApp };
                changed = true;
            }
        }
        foreach (var key in identities.Keys.Where(key => !live.Contains(key)).ToArray())
        {
            identities.Remove(key);
            changed = true;
        }
        if (changed) snapshot = new Dictionary<(int, long), ProcessMetadata>(identities);
        return snapshot;
    }

    private static ProcessMetadata Named(ProcessSample process, ProcessMetadata metadata, IReadOnlyDictionary<int, ServiceEntry[]> services)
    {
        if (!process.Name.Equals("svchost.exe", StringComparison.OrdinalIgnoreCase)) return metadata;
        var name = !services.TryGetValue(process.Id, out var hosted) || hosted.Length == 0 ? "Service Host"
            : hosted.Length == 1 ? "Service Host: " + hosted[0].DisplayName
            : "Service Host: " + SystemInventory.ServiceGroupCaption(hosted[0].Group);
        return name == metadata.DisplayName ? metadata : metadata with { DisplayName = name };
    }

    private ProcessMetadata? Read(ProcessSample process, ref int budget)
    {
        var fallback = process.Name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? process.Name[..^4] : process.Name;
        if (process.Id <= 0) return new(fallback, null, "", null, false);
        users.TryGetValue(process.Id, out var user);
        var handle = NativeMethods.OpenProcess(0x1000, false, process.Id);
        try
        {
            if (handle != 0 && (!NativeMethods.GetProcessTimes(handle, out var created, out _, out _, out _) || created != process.Created))
                return new(fallback, null, "", null, false);
            string? path = null;
            if (handle != 0)
            {
                var length = pathBuffer.Capacity;
                pathBuffer.Clear();
                if (QueryFullProcessImageNameW(handle, 0, pathBuffer, ref length)) path = pathBuffer.ToString();
            }
            // Processes this user cannot open (services, protected processes) still report their image path to the kernel query.
            path ??= KernelImagePath(process.Id);
            var virtualization = "Not allowed";
            var account = handle != 0 ? Account(handle, out virtualization) : "—";
            if (user != null) account = user;
            if (path == null) return new(fallback, null, "", null, false) { UserName = account, Virtualization = virtualization };
            if (!files.TryGetValue(path, out var file))
            {
                if (budget-- <= 0) return null;
                file = ReadFile(path, fallback);
                if (files.Count < 2048) files[path] = file;
            }
            var package = handle != 0 ? PackageFamily(handle) : null;
            return file with { UserName = account, Virtualization = virtualization, Package = package, Packaged = package != null,
                PackageName = package != null ? PackageDisplayName(handle, package) : null };
        }
        finally { if (handle != 0) NativeMethods.CloseHandle(handle); }
    }

    private static readonly Dictionary<string, string> devices = new(StringComparer.OrdinalIgnoreCase);

    private static unsafe string? KernelImagePath(int processId)
    {
        const int capacity = 1024;
        var text = stackalloc char[capacity];
        var information = stackalloc byte[24];
        *(nint*)information = processId;
        *(ushort*)(information + 8) = 0;
        *(ushort*)(information + 10) = capacity * 2;
        *(nint*)(information + 16) = (nint)text;
        if (NativeMethods.NtQuerySystemInformation(88, (nint)information, 24, out _) < 0) return null;
        var length = *(ushort*)(information + 8) / 2;
        if (length <= 0 || length > capacity) return null;
        var native = new string(text, 0, length);
        lock (devices)
        {
            if (devices.Count == 0)
                foreach (var drive in Environment.GetLogicalDrives())
                {
                    var letter = drive.TrimEnd('\\');
                    var target = new StringBuilder(512);
                    if (QueryDosDeviceW(letter, target, target.Capacity) > 0) devices[target.ToString()] = letter;
                }
            foreach (var (device, letter) in devices)
                if (native.StartsWith(device + "\\", StringComparison.OrdinalIgnoreCase)) return letter + native[device.Length..];
        }
        return null;
    }

    private readonly Dictionary<string, string?> packageNames = new(StringComparer.Ordinal);

    // The display name from the package manifest, which Task Manager uses for a packaged app's group.
    private string? PackageDisplayName(nint process, string family)
    {
        if (packageNames.TryGetValue(family, out var cached)) return cached;
        string? name = null;
        try
        {
            var length = 0;
            if (GetPackageFullName(process, ref length, null) == 122 && length is > 0 and < 1024)
            {
                var fullName = new StringBuilder(length);
                if (GetPackageFullName(process, ref length, fullName) == 0)
                {
                    var pathLength = 0;
                    GetPackagePathByFullName(fullName.ToString(), ref pathLength, null);
                    var path = new StringBuilder(Math.Max(1, pathLength));
                    if (pathLength is > 0 and < 32768 && GetPackagePathByFullName(fullName.ToString(), ref pathLength, path) == 0)
                    {
                        var manifest = System.IO.Path.Combine(path.ToString(), "AppxManifest.xml");
                        if (File.Exists(manifest) && new FileInfo(manifest).Length < 4 * 1024 * 1024)
                        {
                            var match = System.Text.RegularExpressions.Regex.Match(File.ReadAllText(manifest), "<DisplayName>([^<]+)</DisplayName>");
                            if (match.Success)
                            {
                                name = System.Net.WebUtility.HtmlDecode(match.Groups[1].Value.Trim());
                                if (name.StartsWith("ms-resource:", StringComparison.OrdinalIgnoreCase))
                                {
                                    var resolved = new StringBuilder(1024);
                                    name = SHLoadIndirectString($"@{{{fullName}? {name}}}", resolved, resolved.Capacity, 0) == 0 ? resolved.ToString() : null;
                                }
                            }
                        }
                    }
                }
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { name = null; }
        if (string.IsNullOrWhiteSpace(name)) name = null;
        if (packageNames.Count < 512) packageNames[family] = name;
        return name;
    }

    private readonly Dictionary<int, string> users = new();
    private DateTime usersUpdated;

    // Account names for every process, including ones this user cannot open, from the terminal services process list.
    private void RefreshUsers()
    {
        if ((DateTime.UtcNow - usersUpdated).TotalSeconds < 5) return;
        usersUpdated = DateTime.UtcNow;
        users.Clear();
        var level = 1;
        if (!WTSEnumerateProcessesExW(0, ref level, -2, out var information, out var count)) return;
        try
        {
            for (var index = 0; index < count; index++)
            {
                var entry = information + index * 64;
                var processId = Marshal.ReadInt32(entry, 4);
                var sid = Marshal.ReadIntPtr(entry, 16);
                if (sid == 0 || !ConvertSidToStringSidW(sid, out var text)) continue;
                string key;
                try { key = Marshal.PtrToStringUni(text) ?? ""; }
                finally { LocalFree(text); }
                if (!accounts.TryGetValue(key, out var name))
                {
                    uint nameSize = 256, domainSize = 256;
                    var nameBuffer = new StringBuilder(256);
                    var domain = new StringBuilder(256);
                    name = LookupAccountSidW(null, sid, nameBuffer, ref nameSize, domain, ref domainSize, out _) ? nameBuffer.ToString() : key;
                    if (accounts.Count < 4096) accounts[key] = name;
                }
                users[processId] = name;
            }
        }
        finally { WTSFreeMemoryExW(1, information, count); }
    }

    private static ProcessMetadata ReadFile(string path, string fallback)
    {
        var name = fallback;
        var publisher = "";
        var description = "";
        try
        {
            var version = FileVersionInfo.GetVersionInfo(path);
            if (!string.IsNullOrWhiteSpace(version.FileDescription)) name = description = version.FileDescription.Trim();
            publisher = version.CompanyName ?? "";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception) { }
        Icon? icon = null;
        var information = new ShellFileInfo();
        if (SHGetFileInfoW(path, 0, ref information, (uint)Marshal.SizeOf<ShellFileInfo>(), 0x101) != 0 && information.Icon != 0)
        {
            try { icon = (Icon)Icon.FromHandle(information.Icon).Clone(); }
            finally { DestroyIcon(information.Icon); }
        }
        return new ProcessMetadata(name, path, publisher, icon, false) { Description = description };
    }

    private static string? PackageFamily(nint process)
    {
        var length = 0;
        if (GetPackageFamilyName(process, ref length, null) != 122 || length is <= 0 or > 1024) return null;
        var name = new StringBuilder(length);
        return GetPackageFamilyName(process, ref length, name) == 0 ? name.ToString() : null;
    }

    private static bool Cloaked(nint window) => DwmGetWindowAttribute(window, 14, out var cloaked, 4) == 0 && cloaked != 0;

    private string Account(nint process, out string virtualization)
    {
        virtualization = "Not allowed";
        if (!OpenProcessToken(process, 8, out var token)) return "—";
        try
        {
            if (GetTokenInformation(token, 24, out var allowed, 4, out _) && allowed != 0)
                virtualization = GetTokenInformation(token, 25, out var enabled, 4, out _) && enabled != 0 ? "Enabled" : "Disabled";
            GetTokenInformation(token, 1, 0, 0, out var needed);
            if (needed is < 8 or > 65536) return "—";
            var buffer = Marshal.AllocHGlobal(needed);
            try
            {
                if (!GetTokenInformation(token, 1, buffer, needed, out _)) return "—";
                var sid = Marshal.ReadIntPtr(buffer);
                if (!ConvertSidToStringSidW(sid, out var text)) return "—";
                string key;
                try { key = Marshal.PtrToStringUni(text) ?? ""; }
                finally { LocalFree(text); }
                if (accounts.TryGetValue(key, out var cached)) return cached;
                uint nameSize = 0, domainSize = 0;
                LookupAccountSidW(null, sid, null, ref nameSize, null, ref domainSize, out _);
                if (nameSize > 32768 || domainSize > 32768) return "—";
                var name = new StringBuilder((int)Math.Max(1, nameSize));
                var domain = new StringBuilder((int)Math.Max(1, domainSize));
                var result = LookupAccountSidW(null, sid, name, ref nameSize, domain, ref domainSize, out _) ? name.ToString() : key;
                if (accounts.Count < 4096) accounts[key] = result;
                return result;
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
        finally { NativeMethods.CloseHandle(token); }
    }

    public void Dispose()
    {
        foreach (var metadata in files.Values) metadata.Icon?.Dispose();
        files.Clear();
        identities.Clear();
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ShellFileInfo
    {
        public nint Icon;
        public int IconIndex;
        public uint Attributes;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string DisplayName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)] public string TypeName;
    }

    private delegate bool WindowCallback(nint window, nint parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, nint parameter);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] private static extern nint GetWindow(nint window, uint command);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowTextLengthW(nint window);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out uint processId);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool QueryFullProcessImageNameW(nint process, uint flags, StringBuilder path, ref int size);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern nint SHGetFileInfoW(string path, uint attributes, ref ShellFileInfo information, uint size, uint flags);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(nint icon);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern int GetPackageFullName(nint process, ref int length, StringBuilder? name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern int GetPackagePathByFullName(string fullName, ref int length, StringBuilder? path);
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)] private static extern int SHLoadIndirectString(string source, StringBuilder output, int capacity, nint reserved);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern int QueryDosDeviceW(string device, StringBuilder target, int capacity);
    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool WTSEnumerateProcessesExW(nint server, ref int level, int session, out nint information, out int count);
    [DllImport("wtsapi32.dll")] private static extern bool WTSFreeMemoryExW(int type, nint memory, int count);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern int GetPackageFamilyName(nint process, ref int length, StringBuilder? name);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool GetTokenInformation(nint token, int type, out int value, int size, out int needed);
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(nint window, int attribute, out int value, int size);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool OpenProcessToken(nint process, uint access, out nint token);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool GetTokenInformation(nint token, int type, nint buffer, int size, out int needed);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] private static extern bool ConvertSidToStringSidW(nint sid, out nint text);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] private static extern bool LookupAccountSidW(string? machine, nint sid, StringBuilder? name, ref uint nameSize, StringBuilder? domain, ref uint domainSize, out int use);
    [DllImport("kernel32.dll")] private static extern nint LocalFree(nint memory);
}
