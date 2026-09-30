using TaskManager.Monitoring;

namespace TaskManager.UI;

internal enum RowKind { Heading, Process, Group, Member, Service }

internal sealed record ProcessRow(RowKind Kind, ProcessSample Sample, string Name, Icon? Icon, ProcessSample[] Members)
{
    public bool Expandable { get; init; }
    public bool Expanded { get; init; }
    public int Depth { get; init; }
    public string Status { get; init; } = "";
    public (int Id, long Created) Key => (Sample.Id, Sample.Created);
}

// Mirrors Windows 10 Task Manager grouping: an app (a process with a visible top-level window) collects its descendant tree,
// processes from one package are grouped together, service-hosting processes list their services, and everything else stands alone.
internal sealed class ProcessRows
{
    private static readonly string windowsDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Windows) + "\\";
    public readonly Dictionary<(int Id, long Created), ProcessSample[]> Groups = new();
    public readonly HashSet<(int Id, long Created)> Expanded = [];
    public readonly HashSet<int> ChildRows = [];
    public bool GroupByType { get; set; }

    public ProcessRow[] Build(ProcessSample[] source, IReadOnlyDictionary<(int Id, long Created), ProcessMetadata> metadata,
        Comparison<ProcessSample> compare, Func<ProcessSample, string> name, IReadOnlyDictionary<int, ServiceEntry[]>? services = null)
    {
        Groups.Clear();
        ChildRows.Clear();
        var byId = new Dictionary<int, ProcessSample>(source.Length);
        foreach (var process in source) byId[process.Id] = process;
        var owners = new Dictionary<int, (string Key, bool App)>(source.Length);
        var grouped = new Dictionary<string, List<ProcessSample>>();
        foreach (var process in source)
        {
            if (process.Id == 0) continue;
            var owner = Owner(process, byId, metadata, owners, 0).Key;
            if (!grouped.TryGetValue(owner, out var members)) grouped[owner] = members = [];
            members.Add(process);
        }

        var roots = new List<(ProcessRow Row, ProcessSample[] Children, ServiceEntry[]? Services)>(grouped.Count);
        foreach (var (key, members) in grouped)
        {
            ProcessSample root;
            if (key.StartsWith("pid:", StringComparison.Ordinal)) root = byId[int.Parse(key.AsSpan(4))];
            else root = members.OrderByDescending(member => IsApp(member, metadata)).ThenBy(member => member.Created).First();
            var information = metadata.GetValueOrDefault((root.Id, root.Created));
            if (members.Count > 1)
            {
                var sorted = members.ToArray();
                Array.Sort(sorted, compare);
                Groups[(root.Id, root.Created)] = sorted;
                var aggregate = root with
                {
                    Cpu = members.Sum(member => member.Cpu), WorkingSet = members.Sum(member => member.WorkingSet),
                    DiskRate = members.All(member => member.DiskRate.HasValue) ? members.Sum(member => member.DiskRate!.Value) : null,
                    NetworkRate = members.All(member => member.NetworkRate.HasValue) ? members.Sum(member => member.NetworkRate!.Value) : null,
                    Gpu = members.Any(member => member.Gpu.HasValue) ? Math.Min(100, members.Sum(member => member.Gpu ?? 0)) : null,
                    GpuEngine = members.Where(member => member.Gpu > 0).OrderByDescending(member => member.Gpu).FirstOrDefault()?.GpuEngine ?? "",
                    Status = members.All(member => member.Status == "Suspended") ? "Suspended" : ""
                };
                var expanded = Expanded.Contains((root.Id, root.Created));
                var title = key.StartsWith("package:", StringComparison.Ordinal) ? information?.PackageName ?? name(root) : name(root);
                roots.Add((new ProcessRow(RowKind.Group, aggregate, $"{title} ({members.Count})", information?.Icon, sorted)
                    { Expandable = true, Expanded = expanded, Status = aggregate.Status }, sorted, null));
                continue;
            }
            ServiceEntry[]? hosted = null;
            if (services != null && services.TryGetValue(root.Id, out var found) && found.Length > 0) hosted = found;
            var icon = root.Name.Equals("svchost.exe", StringComparison.OrdinalIgnoreCase) ? TaskmgrIcons.ServiceHost : information?.Icon;
            var caption = hosted is { Length: > 1 } ? $"{name(root)} ({hosted.Length})" : name(root);
            roots.Add((new ProcessRow(RowKind.Process, root, caption, icon, [root])
                { Expandable = hosted != null, Expanded = hosted != null && Expanded.Contains((root.Id, root.Created)), Status = root.Status }, [], hosted));
        }

        roots.Sort((left, right) => compare(left.Row.Sample, right.Row.Sample));
        var categories = GroupByType ? roots.ToDictionary(root => root.Row.Key, root => Category(root.Row, metadata)) : null;
        var categoryCounts = new int[3];
        if (categories != null)
        {
            foreach (var category in categories.Values) categoryCounts[category]++;
            roots = roots.OrderBy(root => categories[root.Row.Key]).ToList();
        }
        var rows = new List<ProcessRow>(source.Length + 8);
        var previousCategory = -1;
        foreach (var (row, children, hosted) in roots)
        {
            var category = categories?.GetValueOrDefault(row.Key) ?? -1;
            if (GroupByType && category != previousCategory)
            {
                var title = category switch { 0 => "Apps", 1 => "Background processes", _ => "Windows processes" };
                var count = categoryCounts[category];
                rows.Add(new ProcessRow(RowKind.Heading, new ProcessSample(-10 - category, 0, title, 0, 0, 0, 0, 0, 0, 0, 0, 0), $"{title} ({count})", null, []));
                previousCategory = category;
            }
            rows.Add(row);
            if (!row.Expanded) continue;
            foreach (var member in children)
            {
                rows.Add(new ProcessRow(RowKind.Member, member, name(member), metadata.GetValueOrDefault((member.Id, member.Created))?.Icon, [member])
                    { Depth = 1, Status = member.Status });
                ChildRows.Add(member.Id);
            }
            if (hosted != null)
                foreach (var service in hosted.OrderBy(service => service.DisplayName, StringComparer.CurrentCultureIgnoreCase))
                    rows.Add(new ProcessRow(RowKind.Service, row.Sample, service.DisplayName, TaskmgrIcons.Service, []) { Depth = 1, Status = service.Status == "Running" ? "" : service.Status });
        }
        var expandable = rows.Where(row => row.Kind == RowKind.Process && row.Expandable).Select(row => row.Key).ToHashSet();
        Expanded.RemoveWhere(key => !Groups.ContainsKey(key) && !expandable.Contains(key));
        return rows.ToArray();
    }

    private static bool IsApp(ProcessSample process, IReadOnlyDictionary<(int Id, long Created), ProcessMetadata> metadata) =>
        metadata.TryGetValue((process.Id, process.Created), out var information) && information.IsApp;

    // The group a process belongs to, and whether that group is an application that absorbs child processes.
    private static (string Key, bool App) Owner(ProcessSample process, Dictionary<int, ProcessSample> byId,
        IReadOnlyDictionary<(int Id, long Created), ProcessMetadata> metadata, Dictionary<int, (string Key, bool App)> owners, int depth)
    {
        if (owners.TryGetValue(process.Id, out var known)) return known;
        var information = metadata.GetValueOrDefault((process.Id, process.Created));
        (string Key, bool App) result;
        if (information?.Package is { } package) result = ("package:" + package, true);
        else
        {
            result = ("pid:" + process.Id, information?.IsApp == true);
            if (depth < 64 && process.Id > 4 && byId.TryGetValue(process.ParentId, out var parent) && parent.Id != process.Id && parent.Id > 4 && parent.Created <= process.Created)
            {
                var parentOwner = Owner(parent, byId, metadata, owners, depth + 1);
                var sameImage = StringComparer.OrdinalIgnoreCase.Equals(parent.Name, process.Name);
                var shell = parent.Name.Equals("explorer.exe", StringComparison.OrdinalIgnoreCase) && parentOwner.Key == "pid:" + parent.Id;
                if (parentOwner.App && (information?.IsApp != true || sameImage) && (sameImage || !shell)) result = parentOwner;
            }
        }
        owners[process.Id] = result;
        return result;
    }

    private static int Category(ProcessRow row, IReadOnlyDictionary<(int Id, long Created), ProcessMetadata> metadata)
    {
        if (row.Members.Any(member => IsApp(member, metadata))) return 0;
        if (metadata.TryGetValue(row.Key, out var information) &&
            information.Path?.StartsWith(windowsDirectory, StringComparison.OrdinalIgnoreCase) == true) return 2;
        return row.Sample.Id <= 4 ? 2 : 1;
    }
}
