using TaskManager.Monitoring;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace TaskManager.UI;

internal sealed class ProcessList : ListView
{
    // Column indices for the Processes tab.
    private const int NameColumn = 0, StatusColumn = 1, PidColumn = 2, CpuColumn = 3, MemoryColumn = 4, DiskColumn = 5,
        NetworkColumn = 6, GpuColumn = 7, EngineColumn = 8, PowerColumn = 9, TrendColumn = 10;

    // Windows 10 Task Manager heat map, sampled from the stock application.
    private static readonly Color[] Heat =
    [
        Color.FromArgb(255, 244, 196), Color.FromArgb(249, 236, 168), Color.FromArgb(255, 228, 135), Color.FromArgb(255, 210, 100),
        Color.FromArgb(255, 189, 85), Color.FromArgb(255, 167, 69), Color.FromArgb(252, 141, 55)
    ];
    private static readonly Color Selection = Color.FromArgb(205, 232, 255);
    private static readonly Color Hover = Color.FromArgb(229, 243, 255);
    private static readonly Color HeaderText = Color.FromArgb(76, 96, 122);
    private static readonly string[] PowerNames = ["Very low", "Low", "Moderate", "High", "Very high"];

    private ProcessRow[] rows = [];
    private ProcessSample[] source = [];
    private string filter = "";
    private bool memoryPercent;
    private bool networkPercent;
    internal string Filter
    {
        get => filter;
        set
        {
            var next = value.Trim();
            if (filter == next) return;
            filter = next;
            SortRows();
        }
    }
    internal int VisibleRowCount => rows.Count(row => row.Kind != RowKind.Heading);
    private IReadOnlyDictionary<(int Id, long Created), ProcessMetadata> metadata = new Dictionary<(int, long), ProcessMetadata>();
    private IReadOnlyDictionary<int, ServiceEntry[]> services = new Dictionary<int, ServiceEntry[]>();
    private readonly ProcessRows grouping = new();
    public bool GroupByType { get => grouping.GroupByType; set => grouping.GroupByType = value; }
    private readonly Dictionary<int, ListViewItem> itemCache = new();
    private readonly Dictionary<(int Id, long Created), double> trends = new();
    private readonly bool details;
    private int sortColumn;
    private bool descending;
    private int hotRow = -1;
    private double totalMemory, usedMemory, linkSpeed;
    private double lastTrendTime;
    private Font percentFont = new("Segoe UI", 12);
    private Font headingFont = new("Segoe UI", 11.25f);
    private readonly HeaderWindow header = new();
    private readonly ImageList? rowHeightImages;
    private readonly Bitmap? rowHeightBitmap;

    public ProcessSample? SelectedProcess => SelectedRow is { Kind: not (RowKind.Heading or RowKind.Service) } row && row.Sample.Id >= 0 ? row.Sample : null;
    public ProcessSample[]? SelectedGroup => SelectedRow is { Kind: RowKind.Group } row ? row.Members : null;
    public string? SelectedName => SelectedRow?.Name;
    public bool SelectedIsGroup => SelectedRow?.Expandable == true;
    public bool SelectedExpanded => SelectedRow?.Expanded == true;
    private ProcessRow? SelectedRow => SelectedIndices.Count == 0 || SelectedIndices[0] >= rows.Length ? null : rows[SelectedIndices[0]];

    public ProcessList(bool details)
    {
        this.details = details;
        sortColumn = details ? 0 : CpuColumn;
        descending = !details;
        Dock = DockStyle.Fill;
        View = View.Details;
        FullRowSelect = true;
        HideSelection = false;
        MultiSelect = false;
        AllowColumnReorder = true;
        VirtualMode = true;
        OwnerDraw = !details;
        DoubleBuffered = true;
        BorderStyle = BorderStyle.None;
        Font = new Font("Segoe UI", 9);
        if (!details)
        {
            // A 27px-tall transparent image gives the stock 28px row pitch.
            rowHeightImages = new ImageList { ImageSize = new Size(16, 27), ColorDepth = ColorDepth.Depth32Bit };
            rowHeightBitmap = new Bitmap(16, 27);
            rowHeightImages.Images.Add(rowHeightBitmap);
            SmallImageList = rowHeightImages;
            Columns.Add("Name", 357);
            Columns.Add("Status", 120);
            Columns.Add("PID", 50, HorizontalAlignment.Right);
            Columns.Add("CPU", 73, HorizontalAlignment.Right);
            Columns.Add("Memory", 132, HorizontalAlignment.Right);
            Columns.Add("Disk", 73, HorizontalAlignment.Right);
            Columns.Add("Network", 73, HorizontalAlignment.Right);
            Columns.Add("GPU", 73, HorizontalAlignment.Right);
            Columns.Add("GPU engine", 200);
            Columns.Add("Power usage", 100);
            Columns.Add("Power usage trend", 100);
        }
        else
        {
            SmallImageList = IconCache.Images;
            Columns.Add("Name", 200);
            Columns.Add("PID", 60, HorizontalAlignment.Right);
            Columns.Add("Status", 80);
            Columns.Add("User name", 100);
            Columns.Add("CPU", 40, HorizontalAlignment.Right);
            Columns.Add("Memory (active private working set)", 110, HorizontalAlignment.Right);
            Columns.Add("UAC virtualization", 100);
            Columns.Add("Description", 280);
            Columns.Add("Session ID", 0, HorizontalAlignment.Right);
            Columns.Add("Threads", 0, HorizontalAlignment.Right);
            Columns.Add("Handles", 0, HorizontalAlignment.Right);
            Columns.Add("I/O read", 0, HorizontalAlignment.Right);
            Columns.Add("I/O write", 0, HorizontalAlignment.Right);
            Columns.Add("CPU time", 0, HorizontalAlignment.Right);
        }
        RetrieveVirtualItem += (_, args) => args.Item = ItemAt(args.ItemIndex);
        CacheVirtualItems += (_, args) =>
        {
            for (var index = args.StartIndex; index <= Math.Min(args.EndIndex, args.StartIndex + 200); index++) ItemAt(index);
        };
        ColumnClick += (_, args) =>
        {
            descending = sortColumn == args.Column ? !descending : !details && args.Column is >= CpuColumn and <= GpuColumn or PowerColumn or TrendColumn;
            sortColumn = args.Column;
            SortRows();
        };
        DrawColumnHeader += PaintHeader;
        DrawItem += (_, args) => { if (View != View.Details) args.DrawDefault = true; };
        DrawSubItem += PaintCell;
        AccessibleName = details ? "Process details" : "Running processes";
        MouseDown += (_, args) =>
        {
            if (details || args.Button != MouseButtons.Left) return;
            var hit = HitTest(args.Location);
            if (hit.Item == null || hit.Item.Index >= rows.Length) return;
            var row = rows[hit.Item.Index];
            if (row.Expandable && args.X - hit.Item.Bounds.Left < Scale(26)) Toggle(row);
        };
        MouseDoubleClick += (_, args) =>
        {
            if (details || HitTest(args.Location).Item is not { } item || item.Index >= rows.Length) return;
            if (rows[item.Index].Expandable) Toggle(rows[item.Index]);
        };
        MouseMove += (_, args) => SetHot(HitTest(args.Location).Item?.Index ?? -1);
        MouseLeave += (_, _) => SetHot(-1);
        KeyDown += (_, args) =>
        {
            if (args.Control && args.KeyCode == Keys.C && SelectedRow is { } selected)
            {
                Clipboard.SetText(string.Join('\t', Values(selected)));
                args.SuppressKeyPress = true;
                return;
            }
            if (details || SelectedRow is not { Expandable: true } row) return;
            if (args.KeyCode == Keys.Right && !row.Expanded || args.KeyCode == Keys.Left && row.Expanded) { Toggle(row); args.Handled = true; }
        };
    }

    private int Scale(int value) => (int)Math.Round(value * DeviceDpi / 96d);

    private void SetHot(int index)
    {
        if (details || index == hotRow) return;
        var previous = hotRow;
        hotRow = index;
        if (previous >= 0 && previous < rows.Length) RedrawItems(previous, previous, true);
        if (index >= 0 && index < rows.Length) RedrawItems(index, index, true);
    }

    private void Toggle(ProcessRow row)
    {
        if (!grouping.Expanded.Add(row.Key)) grouping.Expanded.Remove(row.Key);
        SortRows(row.Sample);
    }

    public void ToggleSelected() { if (SelectedRow is { Expandable: true } row) Toggle(row); }

    public void ExpandAll(bool expand)
    {
        grouping.Expanded.Clear();
        if (expand)
        {
            foreach (var key in grouping.Groups.Keys) grouping.Expanded.Add(key);
            foreach (var row in rows.Where(row => row.Kind == RowKind.Process && row.Expandable)) grouping.Expanded.Add(row.Key);
        }
        SortRows();
    }

    internal ColumnSettings CaptureColumns() => new()
    {
        Widths = Columns.Cast<ColumnHeader>().Select(column => (int)Math.Round(column.Width * 96d / DeviceDpi)).ToArray(),
        Order = Columns.Cast<ColumnHeader>().Select(column => column.DisplayIndex).ToArray(),
        SortColumn = sortColumn, Descending = descending, MemoryPercent = memoryPercent, NetworkPercent = networkPercent
    };

    internal void RestoreColumns(ColumnSettings? settings)
    {
        if (settings == null) return;
        if (settings.Widths is { } widths && widths.Length == Columns.Count && widths[0] > 0)
            for (var index = 0; index < Columns.Count; index++) Columns[index].Width = (int)(Math.Clamp(settings.Widths[index], 0, 1200) * DeviceDpi / 96d);
        else return;
        if (settings.Order is { } order && order.Length == Columns.Count && order.Order().SequenceEqual(Enumerable.Range(0, Columns.Count)))
            for (var index = 0; index < Columns.Count; index++) Columns[index].DisplayIndex = settings.Order[index];
        if (settings.SortColumn >= 0 && settings.SortColumn < Columns.Count) sortColumn = settings.SortColumn;
        descending = settings.Descending;
        memoryPercent = settings.MemoryPercent;
        networkPercent = settings.NetworkPercent;
        SortRows();
    }

    private void ShowColumns(Point screenPosition)
    {
        var entries = Columns.Cast<ColumnHeader>().Select(column =>
        {
            var selected = column;
            return new MenuEntry(column.Text, () => selected.Width = selected.Width > 0 ? 0 : Scale(details ? 100 : 80),
                () => selected.Width > 0, () => selected.Index != 0);
        }).ToList();
        if (!details)
        {
            entries.Add(MenuEntry.Separator);
            entries.Add(new("Resource values", Children:
            [
                new("Memory", Children:
                [
                    new("Values", () => { memoryPercent = false; itemCache.Clear(); Invalidate(); }, () => !memoryPercent, Radio: true),
                    new("Percents", () => { memoryPercent = true; itemCache.Clear(); Invalidate(); }, () => memoryPercent, Radio: true)
                ]),
                new("Network", Children:
                [
                    new("Values", () => { networkPercent = false; itemCache.Clear(); Invalidate(); }, () => !networkPercent, Radio: true),
                    new("Percents", () => { networkPercent = true; itemCache.Clear(); Invalidate(); }, () => networkPercent, Radio: true)
                ])
            ]));
        }
        // Posted, not run inline: the header's own mouse handling must finish before a modal menu loop starts.
        BeginInvoke(() => NativeMenu.Show(this, screenPosition, entries));
    }

    protected override void OnHandleCreated(EventArgs args)
    {
        base.OnHandleCreated(args);
        NativeMethods.SetWindowTheme(Handle, "Explorer", null);
        header.ContextAction = ShowColumns;
        header.Attach(SendMessageW(Handle, 0x101F, 0, 0), details ? 0 : Scale(43));
        if (!details) SendMessageW(Handle, 5, 0, (nint)((ClientSize.Height << 16) | (ClientSize.Width & 0xffff)));
    }

    protected override void OnDpiChangedAfterParent(EventArgs args)
    {
        base.OnDpiChangedAfterParent(args);
        percentFont.Dispose();
        headingFont.Dispose();
        percentFont = new Font("Segoe UI", 12);
        headingFont = new Font("Segoe UI", 11.25f);
    }

    protected override void OnHandleDestroyed(EventArgs args)
    {
        header.Detach();
        base.OnHandleDestroyed(args);
    }

    public void UpdateSample(SystemSample sample, string? query = null)
    {
        if (query != null) filter = query.Trim();
        var selected = SelectedRow;
        var top = TopItem?.Index ?? 0;
        source = details ? sample.Processes.Where(process => process.Id >= 0).ToArray() : sample.Processes;
        metadata = sample.Metadata;
        services = sample.Services;
        totalMemory = sample.TotalMemory;
        usedMemory = Math.Max(1, sample.TotalMemory - sample.AvailableMemory);
        linkSpeed = sample.Networks.Where(network => network.Speed > 0).Select(network => network.Speed / 8d).DefaultIfEmpty(125_000_000).Max();
        if (!details)
        {
            Columns[CpuColumn].Text = $"{sample.Cpu:0}%\nCPU";
            Columns[MemoryColumn].Text = $"{100d * (sample.TotalMemory - sample.AvailableMemory) / sample.TotalMemory:0}%\nMemory";
            Columns[DiskColumn].Text = $"{sample.Disks.Where(disk => disk.Active.HasValue).Select(disk => disk.Active!.Value).DefaultIfEmpty(0).Max():0}%\nDisk";
            var networkUtilization = sample.Networks.Where(network => network.Speed > 0)
                .Select(network => Math.Clamp(800d * Math.Max(network.SendRate, network.ReceiveRate) / network.Speed, 0, 100)).DefaultIfEmpty(0).Max();
            Columns[NetworkColumn].Text = $"{networkUtilization:0}%\nNetwork";
            Columns[GpuColumn].Text = sample.Gpus.Length > 0 ? $"{sample.Gpus.Max(gpu => gpu.Usage):0}%\nGPU" : "0%\nGPU";
            Columns[PowerColumn].Text = "\nPower usage";
            Columns[TrendColumn].Text = "\nPower usage trend";
            UpdateTrends(sample.MonotonicSeconds);
        }
        SortRows(selected?.Sample, top, selected?.Kind == RowKind.Service ? selected.Name : null);
    }

    // Instantaneous power estimate from CPU, GPU, disk and network activity (Windows' energy estimator is not public).
    private double PowerScore(ProcessSample process) =>
        process.Cpu + (process.Gpu ?? 0) * 0.5 + (process.DiskRate ?? 0) / 1048576d * 0.2 + (process.NetworkRate ?? 0) / 125_000d * 0.1;

    private static int PowerLevel(double score) => score < 0.5 ? 0 : score < 3 ? 1 : score < 10 ? 2 : score < 25 ? 3 : 4;

    private void UpdateTrends(double time)
    {
        var elapsed = lastTrendTime == 0 ? 0 : Math.Clamp(time - lastTrendTime, 0, 10);
        lastTrendTime = time;
        var weight = elapsed == 0 ? 1 : 1 - Math.Exp(-elapsed / 120);
        var live = new HashSet<(int, long)>(source.Length);
        foreach (var process in source)
        {
            var key = (process.Id, process.Created);
            live.Add(key);
            trends[key] = trends.TryGetValue(key, out var previous) ? previous + (PowerScore(process) - previous) * weight : PowerScore(process);
        }
        foreach (var stale in trends.Keys.Where(key => !live.Contains(key)).ToArray()) trends.Remove(stale);
    }

    private double Trend(ProcessRow row) => row.Members.Length == 0 ? 0 : row.Members.Sum(member => trends.GetValueOrDefault((member.Id, member.Created)));

    public void SelectProcess(int processId)
    {
        var index = Array.FindIndex(rows, row => row.Kind != RowKind.Service && row.Sample.Id == processId && row.Kind != RowKind.Group);
        if (index < 0 && !details)
        {
            var parent = grouping.Groups.FirstOrDefault(group => group.Value.Any(process => process.Id == processId));
            if (parent.Value != null) { grouping.Expanded.Add(parent.Key); SortRows(); index = Array.FindIndex(rows, row => row.Kind == RowKind.Member && row.Sample.Id == processId); }
            if (index < 0) index = Array.FindIndex(rows, row => row.Sample.Id == processId);
        }
        if (index < 0) return;
        SelectedIndices.Clear();
        SelectedIndices.Add(index);
        EnsureVisible(index);
        Focus();
    }

    private void SortRows(ProcessSample? selected = null, int top = -1, string? selectedService = null)
    {
        var previous = SelectedRow;
        selected ??= previous?.Sample;
        selectedService ??= previous?.Kind == RowKind.Service ? previous.Name : null;
        if (top < 0) top = TopItem?.Index ?? 0;
        var filtered = filter.Length == 0 ? source : source.Where(process =>
            SearchFilter.Matches(process, metadata.GetValueOrDefault((process.Id, process.Created)), filter)).ToArray();
        if (details)
        {
            var sorted = (ProcessSample[])filtered.Clone();
            Array.Sort(sorted, Compare);
            rows = sorted.Select(process => new ProcessRow(RowKind.Process, process, process.Name, null, [process])).ToArray();
        }
        else rows = grouping.Build(filtered, metadata, Compare, NameOf, services);
        itemCache.Clear();
        BeginUpdate();
        if (VirtualListSize != rows.Length) VirtualListSize = rows.Length;
        if (selected != null)
        {
            var index = Array.FindIndex(rows, row => row.Sample.Id == selected.Id && row.Sample.Created == selected.Created &&
                (selectedService == null ? row.Kind != RowKind.Service : row.Kind == RowKind.Service && row.Name == selectedService));
            if (SelectedIndices.Count != (index >= 0 ? 1 : 0) || index >= 0 && SelectedIndices[0] != index)
            {
                SelectedIndices.Clear();
                if (index >= 0) SelectedIndices.Add(index);
            }
        }
        if (rows.Length > 0 && top > 0) TopItem = Items[Math.Min(top, rows.Length - 1)];
        EndUpdate();
        Invalidate();
    }

    private int Compare(ProcessSample left, ProcessSample right)
    {
        var result = details ? sortColumn switch
        {
            1 => left.Id.CompareTo(right.Id), 2 => StringComparer.OrdinalIgnoreCase.Compare(DetailStatus(left), DetailStatus(right)),
            3 => StringComparer.OrdinalIgnoreCase.Compare(Info(left)?.UserName, Info(right)?.UserName),
            4 => left.Cpu.CompareTo(right.Cpu), 5 => left.WorkingSet.CompareTo(right.WorkingSet),
            6 => StringComparer.OrdinalIgnoreCase.Compare(Info(left)?.Virtualization, Info(right)?.Virtualization),
            7 => StringComparer.CurrentCultureIgnoreCase.Compare(Info(left)?.Description, Info(right)?.Description),
            8 => left.Session.CompareTo(right.Session), 9 => left.Threads.CompareTo(right.Threads), 10 => left.Handles.CompareTo(right.Handles),
            11 => left.ReadRate.CompareTo(right.ReadRate), 12 => left.WriteRate.CompareTo(right.WriteRate), 13 => left.CpuTime.CompareTo(right.CpuTime),
            _ => StringComparer.OrdinalIgnoreCase.Compare(left.Name, right.Name)
        } : sortColumn switch
        {
            CpuColumn => left.Cpu.CompareTo(right.Cpu), MemoryColumn => left.WorkingSet.CompareTo(right.WorkingSet),
            DiskColumn => Nullable.Compare(left.DiskRate, right.DiskRate), NetworkColumn => Nullable.Compare(left.NetworkRate, right.NetworkRate),
            GpuColumn => Nullable.Compare(left.Gpu, right.Gpu), PidColumn => left.Id.CompareTo(right.Id),
            StatusColumn => StringComparer.OrdinalIgnoreCase.Compare(left.Status, right.Status),
            EngineColumn => StringComparer.OrdinalIgnoreCase.Compare(left.GpuEngine, right.GpuEngine),
            PowerColumn => PowerScore(left).CompareTo(PowerScore(right)),
            TrendColumn => trends.GetValueOrDefault((left.Id, left.Created)).CompareTo(trends.GetValueOrDefault((right.Id, right.Created))),
            _ => StringComparer.CurrentCultureIgnoreCase.Compare(NameOf(left), NameOf(right))
        };
        if (result == 0) result = left.Id.CompareTo(right.Id);
        return descending ? -result : result;
    }

    private ProcessMetadata? Info(ProcessSample process) => metadata.GetValueOrDefault((process.Id, process.Created));

    private static string DetailStatus(ProcessSample process) => process.Status == "" ? "Running" : process.Status;

    private string[] Values(ProcessRow row)
    {
        var process = row.Sample;
        if (details)
        {
            var information = Info(process);
            return [process.Name, process.Id.ToString(), DetailStatus(process), information?.UserName ?? "", $"{Math.Min(99, Math.Round(process.Cpu)):00}",
                $"{process.WorkingSet / 1024:N0} K", information?.Virtualization ?? "", information?.Description ?? "",
                process.Session.ToString(), process.Threads.ToString("N0"), process.Handles.ToString("N0"),
                Metrics.Bytes(process.ReadRate) + "/s", Metrics.Bytes(process.WriteRate) + "/s", Metrics.CpuTime(process.CpuTime)];
        }
        if (row.Kind == RowKind.Heading) return [row.Name, "", "", "", "", "", "", "", "", "", ""];
        if (row.Kind == RowKind.Service) return [row.Name, row.Status, "", "", "", "", "", "", "", "", ""];
        var power = PowerNames[PowerLevel(PowerScore(process))];
        return [row.Name, row.Status, row.Kind == RowKind.Group || process.Id < 0 ? "" : process.Id.ToString(), Metrics.Percent(process.Cpu),
            memoryPercent ? Metrics.Percent(100 * process.WorkingSet / Math.Max(1, totalMemory)) : Metrics.Megabytes(process.WorkingSet),
            process.DiskRate is double disk ? Metrics.DiskRate(disk) : "—", process.NetworkRate is double network ? networkPercent ? Metrics.Percent(Math.Clamp(100 * network / linkSpeed, 0, 100)) : Metrics.NetworkRate(network) : "—",
            Metrics.Percent(process.Gpu ?? 0), process.GpuEngine, power, PowerNames[PowerLevel(Trend(row))]];
    }

    private ListViewItem ItemAt(int index)
    {
        if (index < 0 || index >= rows.Length) return new ListViewItem("");
        if (itemCache.TryGetValue(index, out var cached)) return cached;
        var row = rows[index];
        var item = new ListViewItem(Values(row)) { ImageIndex = details ? IconCache.Index(Info(row.Sample)?.Icon ?? TaskmgrIcons.Process) : 0 };
        if (itemCache.Count > 512) itemCache.Clear();
        itemCache[index] = item;
        return item;
    }

    private static string DisplayName(string name) => name.ToLowerInvariant() switch
    {
        "explorer.exe" => "Windows Explorer", "taskmanager.exe" or "taskmgr.exe" => "Task Manager",
        "dwm.exe" => "Desktop Window Manager", "svchost.exe" => "Service Host",
        _ => name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name[..^4] : name
    };

    // File description when there is one; otherwise the image file name, as Windows shows for "ssh.exe".
    private string NameOf(ProcessSample process)
    {
        var information = Info(process);
        if (process.Id == 4) return "System";
        if (information == null) return DisplayName(process.Name);
        if (information.Description.Length > 0 || information.DisplayName.StartsWith("Service Host", StringComparison.Ordinal)) return information.DisplayName;
        return information.Path != null ? process.Name : DisplayName(process.Name);
    }

    // Heat level from the share of the relevant total. Zero after rounding is the palest shade, like the stock app.
    private int Level(int column, ProcessRow row)
    {
        var process = row.Sample;
        var (value, total, zero) = column switch
        {
            CpuColumn => (process.Cpu, 100d, Metrics.IsZero(process.Cpu)),
            MemoryColumn => (process.WorkingSet, usedMemory, Metrics.IsZero(process.WorkingSet, 1048576)),
            DiskColumn => (process.DiskRate ?? 0, 1073741824d, Metrics.IsZero(process.DiskRate ?? 0, 1048576)),
            NetworkColumn => (process.NetworkRate ?? 0, linkSpeed, Metrics.IsZero(process.NetworkRate ?? 0, 125000)),
            GpuColumn => (process.Gpu ?? 0, 100d, Metrics.IsZero(process.Gpu ?? 0)),
            PowerColumn => (PowerLevel(PowerScore(process)), 4d, false),
            TrendColumn => (PowerLevel(Trend(row)), 4d, false),
            _ => (0d, 1d, true)
        };
        if (column is PowerColumn or TrendColumn) return 1 + (int)value;
        if (zero) return 0;
        var share = value / Math.Max(1, total);
        return share < 0.10 ? 1 : share < 0.20 ? 2 : share < 0.40 ? 3 : share < 0.60 ? 4 : share < 0.80 ? 5 : 6;
    }

    private static bool IsHeat(int column) => column is >= CpuColumn and <= GpuColumn or PowerColumn or TrendColumn;

    private static Color Blend(Color over, Color under, double amount) => Color.FromArgb(
        (int)Math.Round(under.R + (over.R - under.R) * amount), (int)Math.Round(under.G + (over.G - under.G) * amount), (int)Math.Round(under.B + (over.B - under.B) * amount));

    private void PaintHeader(object? sender, DrawListViewColumnHeaderEventArgs args)
    {
        if (details) { args.DrawDefault = true; return; }
        var graphics = args.Graphics;
        var bounds = args.Bounds;
        var hot = args.State.HasFlag(ListViewItemStates.Hot);
        using (var background = new SolidBrush(hot ? Hover : Color.White)) graphics.FillRectangle(background, bounds);
        using (var separator = new Pen(Color.FromArgb(229, 229, 229)))
            graphics.DrawLine(separator, bounds.Right - 1, bounds.Top + Scale(3), bounds.Right - 1, bounds.Bottom - 2);
        using (var line = new Pen(Color.FromArgb(229, 229, 229)))
            graphics.DrawLine(line, bounds.Left, bounds.Bottom - 1, bounds.Right, bounds.Bottom - 1);
        var text = args.Header?.Text ?? "";
        var lines = text.Split('\n');
        var label = lines[^1];
        var right = args.Header?.TextAlign == HorizontalAlignment.Right || IsHeat(args.ColumnIndex);
        var labelBounds = new Rectangle(bounds.Left + Scale(6), bounds.Bottom - Scale(21), bounds.Width - Scale(12), Scale(18));
        var flags = TextFormatFlags.Bottom | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding | (right ? TextFormatFlags.Right : TextFormatFlags.Left);
        if (lines.Length == 2 && lines[0].Length > 0)
            TextRenderer.DrawText(graphics, lines[0], percentFont, new Rectangle(bounds.Left + Scale(4), bounds.Top + Scale(2), bounds.Width - Scale(10), Scale(24)),
                Color.Black, TextFormatFlags.Right | TextFormatFlags.Top | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
        TextRenderer.DrawText(graphics, label, Font, labelBounds, HeaderText, flags);
        if (args.ColumnIndex == sortColumn)
        {
            // The thin sort chevron sits near the top-left of the header.
            using var pen = new Pen(Color.FromArgb(118, 118, 118), 1);
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            var x = bounds.Left + (IsHeat(args.ColumnIndex) ? Scale(9) : bounds.Width / 2 - Scale(4));
            float y = bounds.Top + Scale(6);
            float width = Scale(8), height = Scale(4);
            PointF[] points = descending ? [new(x, y), new(x + width / 2, y + height), new(x + width, y)]
                : [new(x, y + height), new(x + width / 2, y), new(x + width, y + height)];
            graphics.DrawLines(pen, points);
            graphics.SmoothingMode = SmoothingMode.Default;
        }
    }

    private void PaintCell(object? sender, DrawListViewSubItemEventArgs args)
    {
        if (details) { args.DrawDefault = true; return; }
        if (args.ItemIndex < 0 || args.ItemIndex >= rows.Length) return;
        var graphics = args.Graphics;
        var row = rows[args.ItemIndex];
        var column = args.ColumnIndex;
        var bounds = args.Bounds;
        var selected = args.Item?.Selected == true;
        var hot = args.ItemIndex == hotRow;
        if (row.Kind == RowKind.Heading)
        {
            graphics.FillRectangle(Brushes.White, bounds);
            if (column == NameColumn)
                TextRenderer.DrawText(graphics, row.Name, headingFont, new Rectangle(bounds.Left + Scale(8), bounds.Top + Scale(4), Math.Max(0, ClientSize.Width - Scale(16)), bounds.Height - Scale(4)),
                    Color.FromArgb(30, 57, 91), TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            return;
        }
        var heat = IsHeat(column) && row.Kind != RowKind.Service;
        var background = heat ? Heat[Level(column, row)] : Color.White;
        if (selected) background = heat ? Blend(background, Selection, 0.25) : Selection;
        else if (hot) background = heat ? Blend(background, Hover, 0.25) : Hover;
        using (var brush = new SolidBrush(background)) graphics.FillRectangle(brush, bounds);
        if (heat)
        {
            using var divider = new Pen(Blend(Color.FromArgb(190, 140, 130), background, 0.25));
            graphics.DrawLine(divider, bounds.Right - 1, bounds.Top, bounds.Right - 1, bounds.Bottom);
        }
        var text = args.SubItem?.Text ?? "";
        var textBounds = Rectangle.Inflate(bounds, -Scale(6), 0);
        if (column == NameColumn)
        {
            var indent = row.Depth * Scale(22);
            if (row.Expandable) DrawChevron(graphics, bounds.Left + Scale(10) + indent, bounds.Top + bounds.Height / 2, row.Expanded);
            var icon = row.Icon ?? (row.Kind == RowKind.Service ? TaskmgrIcons.Service : TaskmgrIcons.Process);
            var size = Scale(16);
            var iconBounds = new Rectangle(bounds.Left + Scale(28) + indent, bounds.Top + (bounds.Height - size) / 2, size, size);
            try { graphics.DrawIcon(icon, iconBounds); }
            catch (ObjectDisposedException) { graphics.DrawIcon(TaskmgrIcons.Process, iconBounds); }
            textBounds = new Rectangle(bounds.Left + Scale(50) + indent, bounds.Top, Math.Max(0, bounds.Width - Scale(56) - indent), bounds.Height);
        }
        var right = Columns[column].TextAlign == HorizontalAlignment.Right;
        TextRenderer.DrawText(graphics, text, Font, textBounds, Color.Black,
            TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.SingleLine | TextFormatFlags.NoPrefix | TextFormatFlags.NoPadding | (right ? TextFormatFlags.Right : TextFormatFlags.Left));
    }

    private void DrawChevron(Graphics graphics, int left, int middle, bool expanded)
    {
        using var pen = new Pen(Color.FromArgb(128, 128, 128), 1);
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        float x = left, y = middle;
        var unit = DeviceDpi / 96f;
        PointF[] points = expanded
            ? [new(x - 1 * unit, y - 2 * unit), new(x + 3 * unit, y + 2 * unit), new(x + 7 * unit, y - 2 * unit)]
            : [new(x + 1 * unit, y - 4 * unit), new(x + 5 * unit, y), new(x + 1 * unit, y + 4 * unit)];
        graphics.DrawLines(pen, points);
        graphics.SmoothingMode = SmoothingMode.Default;
    }

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
        if (disposing) { rowHeightImages?.Dispose(); rowHeightBitmap?.Dispose(); percentFont.Dispose(); headingFont.Dispose(); }
    }

    private sealed class HeaderWindow : NativeWindow
    {
        private int height;
        public Action<Point>? ContextAction { get; set; }
        public void Attach(nint handle, int targetHeight)
        {
            Detach();
            height = targetHeight;
            if (handle != 0) AssignHandle(handle);
        }

        public void Detach() { if (Handle != 0) ReleaseHandle(); }

        protected override void WndProc(ref Message message)
        {
            if (message.Msg == 0x7b && ContextAction != null)
            {
                ContextAction(Cursor.Position);
                message.Result = 0;
                return;
            }
            base.WndProc(ref message);
            if (height <= 0 || message.Msg != 0x1205 || message.LParam == 0) return;
            var rectangle = Marshal.ReadIntPtr(message.LParam);
            var position = Marshal.ReadIntPtr(message.LParam, IntPtr.Size);
            if (rectangle == 0 || position == 0) return;
            var top = Marshal.ReadInt32(rectangle, 4);
            var oldHeight = Marshal.ReadInt32(position, 2 * IntPtr.Size + 12);
            Marshal.WriteInt32(rectangle, 4, top + height - oldHeight);
            Marshal.WriteInt32(position, 2 * IntPtr.Size + 12, height);
        }
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern nint SendMessageW(nint window, uint message, nint wParam, nint lParam);
}
