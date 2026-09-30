using System.Drawing.Drawing2D;
using TaskManager.Monitoring;

namespace TaskManager.UI;

internal sealed class PerformanceView : Control
{
    private readonly Dictionary<string, History> histories = new(StringComparer.Ordinal);
    private readonly List<Resource> resources = [];
    private CpuInventory cpu = new("Loading processor information…", "", Environment.ProcessorCount, 1);
    private MemoryHardware memoryHardware = new(0, 0, 0, "", 0);
    private bool hardwareLoading;
    private readonly Font titleFont = new("Segoe UI", 28, FontStyle.Regular, GraphicsUnit.Pixel);
    private readonly Font bodyFont = new("Segoe UI", 12, FontStyle.Regular, GraphicsUnit.Pixel);
    private readonly Font labelFont = new("Segoe UI", 11, FontStyle.Regular, GraphicsUnit.Pixel);
    private readonly Font valueFont = new("Segoe UI", 24, FontStyle.Regular, GraphicsUnit.Pixel);
    private readonly Font sidebarFont = new("Segoe UI", 16, FontStyle.Regular, GraphicsUnit.Pixel);
    private SystemSample? sample;
    private string selection = "cpu";
    private int scrollOffset;
    private bool logicalProcessors;
    internal IReadOnlyList<string> ResourceKeys => resources.Select(resource => resource.Key).ToArray();
    internal string SelectedResource { get => selection; set { if (resources.Any(resource => resource.Key == value)) { selection = value; Invalidate(); } } }
    private static readonly Color CpuColor = Color.FromArgb(17, 125, 187);
    private static readonly Color MemoryColor = Color.FromArgb(139, 18, 174);
    private static readonly Color DiskColor = Color.FromArgb(77, 166, 12);
    private static readonly Color NetworkColor = Color.FromArgb(167, 79, 1);
    private static readonly Color GpuColor = Color.FromArgb(17, 125, 187);

    public PerformanceView()
    {
        Dock = DockStyle.Fill;
        BackColor = Color.White;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        TabStop = true;
        AccessibleName = "System performance graphs";
        var menu = new ContextMenuStrip();
        var change = new ToolStripMenuItem("Change graph to");
        change.DropDownItems.Add("Overall utilization", null, (_, _) => { logicalProcessors = false; Invalidate(); });
        change.DropDownItems.Add("Logical processors", null, (_, _) => { logicalProcessors = true; Invalidate(); });
        menu.Items.Add(change);
        menu.Items.Add("Copy", null, (_, _) => Clipboard.SetText(Summary));
        ContextMenuStrip = menu;
    }

    public string Summary => sample == null ? "Waiting for performance data." :
        $"CPU: {sample.Cpu:0}%\r\nMemory: {Metrics.Bytes(sample.TotalMemory - sample.AvailableMemory)} / {Metrics.Bytes(sample.TotalMemory)}\r\n" +
        $"Processes: {sample.ProcessCount}\r\nThreads: {sample.ThreadCount}\r\nHandles: {sample.HandleCount}\r\nUp time: {sample.Uptime:d\\:hh\\:mm\\:ss}";

    protected override async void OnHandleCreated(EventArgs args)
    {
        base.OnHandleCreated(args);
        if (hardwareLoading) return;
        hardwareLoading = true;
        try
        {
            var hardware = await Task.Run(() => (Cpu: SystemInventory.Cpu(), Memory: HardwareInventory.Memory()));
            if (IsDisposed || Disposing) return;
            cpu = hardware.Cpu;
            memoryHardware = hardware.Memory;
            Invalidate();
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or UnauthorizedAccessException)
        {
            System.Diagnostics.Debug.WriteLine(error.Message);
        }
    }

    public void UpdateSample(SystemSample next)
    {
        sample = next;
        var time = next.MonotonicSeconds;
        resources.Clear();
        var speed = next.CpuSpeedFactor.HasValue && double.TryParse(cpu.BaseSpeed.Split(' ')[0], out var nominal)
            ? $"{nominal * next.CpuSpeedFactor.Value:0.00} GHz" : cpu.BaseSpeed;
        Add(new Resource("cpu", "CPU", $"{next.Cpu:0}%  {speed}", cpu.Name, CpuColor), time, next.Cpu, 0);
        for (var index = 0; index < next.CpuCores.Length; index++) AddHistory("core:" + index, time, next.CpuCores[index], 0);
        var used = next.TotalMemory - next.AvailableMemory;
        Add(new Resource("memory", "Memory", $"{used / 1073741824d:0.0}/{next.TotalMemory / 1073741824d:0.0} GB ({100d * used / next.TotalMemory:0}%)",
            Metrics.Bytes(next.TotalMemory), MemoryColor), time, 100d * used / next.TotalMemory, 0);
        foreach (var disk in next.Disks)
        {
            var segments = disk.Name.Split(' ', 2);
            var title = "Disk " + segments[0] + (segments.Length > 1 ? $" ({segments[1]})" : "");
            Add(new Resource("disk:" + disk.Name, title, disk.Active is double active ? $"{active:0}%" : "Unavailable",
                disk.Hardware?.Model ?? "Physical disk", DiskColor, disk.Hardware?.Type ?? ""), time, disk.Active ?? double.NaN, 0);
            AddHistory("transfer:" + disk.Name, time, (disk.ReadRate ?? double.NaN) + (disk.WriteRate ?? double.NaN), 0);
        }
        foreach (var network in next.Networks)
            Add(new Resource("net:" + network.Id, network.Type, SendReceive(network), network.Description, NetworkColor, network.Name), time, network.ReceiveRate, network.SendRate);
        foreach (var gpu in next.Gpus)
        {
            Add(new Resource("gpu:" + gpu.Id, "GPU " + gpu.Index, $"{gpu.Usage:0}%", gpu.Name, GpuColor, gpu.Name), time, gpu.Usage, 0);
            foreach (var engine in gpu.Engines) AddHistory("engine:" + gpu.Id + ":" + engine.Key, time, engine.Value, 0);
            AddHistory("dedicated:" + gpu.Id, time, gpu.DedicatedLimit > 0 ? 100 * gpu.Dedicated / gpu.DedicatedLimit : 0, 0);
            AddHistory("shared:" + gpu.Id, time, gpu.SharedLimit > 0 ? 100 * gpu.Shared / gpu.SharedLimit : 0, 0);
        }
        var activeKeys = resources.Select(resource => resource.Key).ToHashSet(StringComparer.Ordinal);
        foreach (var disk in next.Disks) activeKeys.Add("transfer:" + disk.Name);
        for (var index = 0; index < next.CpuCores.Length; index++) activeKeys.Add("core:" + index);
        foreach (var gpu in next.Gpus)
        {
            foreach (var engine in gpu.Engines) activeKeys.Add("engine:" + gpu.Id + ":" + engine.Key);
            activeKeys.Add("dedicated:" + gpu.Id);
            activeKeys.Add("shared:" + gpu.Id);
        }
        foreach (var stale in histories.Keys.Where(key => !activeKeys.Contains(key)).ToArray()) histories.Remove(stale);
        if (!activeKeys.Contains(selection)) selection = "cpu";
        ClampScroll();
        if (Visible) Invalidate();
    }

    private void Add(Resource resource, double time, double value, double secondary)
    {
        resources.Add(resource);
        AddHistory(resource.Key, time, value, secondary);
    }

    private void AddHistory(string key, double time, double value, double secondary)
    {
        if (!histories.TryGetValue(key, out var history)) histories[key] = history = new History();
        history.Add(time, value, secondary);
    }

    protected override void OnPaint(PaintEventArgs args)
    {
        base.OnPaint(args);
        var graphics = args.Graphics;
        var scale = DeviceDpi / 96f;
        graphics.ScaleTransform(scale, scale);
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        var width = ClientSize.Width / scale;
        var height = ClientSize.Height / scale;
        graphics.Clear(Color.White);
        if (sample == null) { DrawText(graphics, "Collecting performance data…", bodyFont, Color.DimGray, 245, 30); return; }
        var currentTime = sample.MonotonicSeconds;
        var sidebarState = graphics.Save();
        graphics.SetClip(new RectangleF(0, 0, 222, height));
        for (var index = 0; index < resources.Count; index++)
        {
            var resource = resources[index];
            var top = 12 + index * 82 - scrollOffset;
            if (top + 80 < 0 || top > height) continue;
            if (resource.Key == selection)
            {
                using var selected = new SolidBrush(Color.FromArgb(217, 234, 247));
                graphics.FillRectangle(selected, 2, top, 216, 78);
            }
            var maximum = resource.Key.StartsWith("net:", StringComparison.Ordinal) ? histories[resource.Key].Maximum(currentTime) : 100;
            DrawGraph(graphics, new RectangleF(15, top + 12, 90, 52), histories[resource.Key], currentTime, maximum, resource.Color, false);
            DrawText(graphics, resource.Title, sidebarFont, Color.Black, 116, top + 10, 98, 23);
            if (resource.Detail.Length > 0)
            {
                DrawText(graphics, resource.Detail, labelFont, Color.FromArgb(60, 60, 60), 116, top + 33, 99, 16);
                DrawText(graphics, resource.Value, labelFont, Color.FromArgb(60, 60, 60), 116, top + 49, 104, 16);
            }
            else DrawText(graphics, resource.Value, labelFont, Color.FromArgb(60, 60, 60), 116, top + 35, 99, 32);
        }
        graphics.Restore(sidebarState);
        using var sidebarBorder = new Pen(Color.FromArgb(238, 238, 238));
        graphics.DrawLine(sidebarBorder, 222, 0, 222, height);
        var selectedResource = resources.First(resource => resource.Key == selection);
        var left = 250f;
        var contentWidth = Math.Max(100, width - left - 26);
        DrawText(graphics, selectedResource.Title, titleFont, Color.Black, left, 14, contentWidth, 42);
        var subtitleWidth = Math.Max(100, contentWidth - 140);
        DrawText(graphics, selectedResource.Description, bodyFont, Color.Black, left + contentWidth - subtitleWidth, 31, subtitleWidth, 28, true);
        var isNetwork = selection.StartsWith("net:", StringComparison.Ordinal);
        var isDisk = selection.StartsWith("disk:", StringComparison.Ordinal);
        var isGpu = selection.StartsWith("gpu:", StringComparison.Ordinal);
        var chartHeight = Math.Clamp(height - (isDisk ? 410 : 300), 80, 330);
        if (isGpu) chartHeight = Math.Max(110, chartHeight - 150);
        var graphBounds = new RectangleF(left, isGpu ? 80 : 94, contentWidth, chartHeight);
        var graphMaximum = isNetwork ? histories[selection].Maximum(currentTime) : 100;
        if (!isGpu)
        {
            DrawText(graphics, selection == "memory" ? "Memory usage" : isNetwork ? "Throughput" : selection == "cpu" ? "% Utilization" : "% Active time",
                labelFont, selectedResource.Color, left, 73);
            DrawText(graphics, isNetwork ? Metrics.Bits(graphMaximum) : selection == "memory" ? $"{sample.TotalMemory / 1073741824d:0.0} GB" : "100%", labelFont, selectedResource.Color, left + contentWidth - 100, 73, 100, 18, true);
        }
        var bottom = graphBounds.Bottom;
        if (selection == "cpu" && logicalProcessors && sample.CpuCores.Length > 0)
            PaintPanels(graphics, graphBounds, currentTime, sample.CpuCores.Select((_, index) => ("core:" + index, "CPU " + index, "")).ToArray(), CpuColor);
        else if (isGpu)
        {
            // Windows shows four engine graphs, then dedicated and shared memory usage graphs.
            var gpu = sample.Gpus.First(adapter => "gpu:" + adapter.Id == selection);
            PaintPanels(graphics, graphBounds, currentTime, gpu.Engines.OrderBy(engine => EngineOrder(EngineType(engine.Key))).ThenBy(engine => engine.Key, StringComparer.Ordinal)
                .GroupBy(engine => EngineType(engine.Key), StringComparer.Ordinal).Take(4).Select(group => group.First())
                .Select(engine => ("engine:" + gpu.Id + ":" + engine.Key, EngineTitle(EngineType(engine.Key)), $"{engine.Value:0}%")).ToArray(), GpuColor);
            var memoryTop = graphBounds.Bottom + 8;
            foreach (var (key, title, limit) in new[] { ("dedicated:", "Dedicated GPU memory usage", gpu.DedicatedLimit), ("shared:", "Shared GPU memory usage", gpu.SharedLimit) })
            {
                DrawText(graphics, title, labelFont, GpuColor, left, memoryTop);
                DrawText(graphics, $"{limit / 1073741824d:0.0} GB", labelFont, GpuColor, left + contentWidth - 100, memoryTop, 100, 18, true);
                var bounds = new RectangleF(left, memoryTop + 18, contentWidth, 44);
                DrawGraph(graphics, bounds, histories[key + gpu.Id], currentTime, 100, GpuColor, false);
                memoryTop = bounds.Bottom + 8;
                bottom = bounds.Bottom;
            }
        }
        else DrawGraph(graphics, graphBounds, histories[selection], currentTime, graphMaximum, selectedResource.Color, true);
        DrawText(graphics, "60 seconds", labelFont, selectedResource.Color, left, bottom + 4);
        DrawText(graphics, "0", labelFont, selectedResource.Color, left + contentWidth - 30, bottom + 4, 30, 18, true);
        var statsTop = bottom + 37;
        if (isDisk)
        {
            var history = histories["transfer:" + selection[5..]];
            var transferBounds = new RectangleF(left, statsTop + 18, contentWidth, 80);
            DrawText(graphics, "Disk transfer rate", labelFont, DiskColor, left, statsTop - 2);
            DrawGraph(graphics, transferBounds, history, currentTime, history.Maximum(currentTime), DiskColor, true);
            DrawText(graphics, "60 seconds", labelFont, DiskColor, left, transferBounds.Bottom + 4);
            statsTop = transferBounds.Bottom + 35;
        }
        if (selection == "cpu") PaintCpu(graphics, left, statsTop, contentWidth);
        else if (selection == "memory") PaintMemory(graphics, left, statsTop, contentWidth);
        else if (isNetwork) PaintNetwork(graphics, left, statsTop, contentWidth);
        else if (selection.StartsWith("gpu:", StringComparison.Ordinal)) PaintGpu(graphics, left, statsTop);
        else PaintDisk(graphics, left, statsTop);
        if (resources.Count * 82 + 24 > height)
            DrawText(graphics, "Scroll for more devices", labelFont, Color.DimGray, 15, height - 20, 195, 18);
    }

    private void PaintCpu(Graphics graphics, float left, float top, float width)
    {
        if (sample == null) return;
        Stat(graphics, "Utilization", $"{sample.Cpu:0}%", left, top);
        var speed = sample.CpuSpeedFactor.HasValue && double.TryParse(cpu.BaseSpeed.Split(' ')[0], out var nominal)
            ? $"{nominal * sample.CpuSpeedFactor.Value:0.00} GHz" : cpu.BaseSpeed;
        Stat(graphics, "Speed", speed, left + 130, top);
        Stat(graphics, "Processes", sample.ProcessCount.ToString(), left, top + 57, false);
        Stat(graphics, "Threads", sample.ThreadCount.ToString("N0"), left + 110, top + 57, false);
        Stat(graphics, "Handles", sample.HandleCount.ToString("N0"), left + 210, top + 57, false);
        Stat(graphics, "Up time", sample.Uptime.ToString(@"d\:hh\:mm\:ss"), left, top + 109);
        if (width >= 450)
        {
            var right = left + Math.Max(330, width - 185);
            Pair(graphics, "Base speed:", cpu.BaseSpeed, right, top + 4, 185);
            Pair(graphics, "Sockets:", cpu.Sockets > 0 ? cpu.Sockets.ToString() : "—", right, top + 25, 185);
            Pair(graphics, "Cores:", cpu.Cores > 0 ? cpu.Cores.ToString() : "—", right, top + 46, 185);
            Pair(graphics, "Logical processors:", Environment.ProcessorCount.ToString(), right, top + 67, 185);
            Pair(graphics, "Virtualization:", cpu.VirtualizationEnabled ? "Enabled" : "Disabled", right, top + 88, 185);
            for (var index = 0; index < cpu.Caches.Length; index++)
                Pair(graphics, $"L{index + 1} cache:", cpu.Caches[index] > 0 ? Metrics.Bytes(cpu.Caches[index]) : "—", right, top + 109 + index * 21, 185);
        }
    }

    private void PaintPanels(Graphics graphics, RectangleF bounds, double time, (string Key, string Title, string Value)[] panels, Color color)
    {
        if (panels.Length == 0) return;
        var columns = panels.Length <= 4 ? 2 : (int)Math.Ceiling(Math.Sqrt(panels.Length * bounds.Width / Math.Max(1, bounds.Height)));
        var rows = (int)Math.Ceiling(panels.Length / (double)columns);
        var width = (bounds.Width - (columns - 1) * 10) / columns;
        var height = (bounds.Height - (rows - 1) * 22) / rows;
        for (var index = 0; index < panels.Length; index++)
        {
            var rectangle = new RectangleF(bounds.Left + index % columns * (width + 10), bounds.Top + index / columns * (height + 22), width, height);
            DrawText(graphics, panels[index].Title, labelFont, color, rectangle.Left, rectangle.Top - 19, rectangle.Width, 19);
            if (panels[index].Value.Length > 0) DrawText(graphics, panels[index].Value, labelFont, color, rectangle.Left, rectangle.Top - 19, rectangle.Width, 19, true);
            DrawGraph(graphics, rectangle, histories[panels[index].Key], time, 100, color, true);
        }
    }

    private void PaintGpu(Graphics graphics, float left, float top)
    {
        var gpu = sample?.Gpus.FirstOrDefault(adapter => "gpu:" + adapter.Id == selection);
        if (gpu == null) return;
        Stat(graphics, "Utilization", $"{gpu.Usage:0}%", left, top);
        Stat(graphics, "GPU memory", Metrics.Bytes(gpu.Dedicated + gpu.Shared), left + 180, top);
        Stat(graphics, "Dedicated GPU memory", $"{gpu.Dedicated / 1073741824d:0.0}/{gpu.DedicatedLimit / 1073741824d:0.0} GB", left, top + 65, false);
        Stat(graphics, "Shared GPU memory", $"{gpu.Shared / 1073741824d:0.0}/{gpu.SharedLimit / 1073741824d:0.0} GB", left + 210, top + 65, false);
    }

    private void PaintMemory(Graphics graphics, float left, float top, float width)
    {
        if (sample == null) return;
        DrawText(graphics, "Memory composition", labelFont, MemoryColor, left, top - 4);
        using var outline = new Pen(MemoryColor);
        using var fill = new SolidBrush(Color.FromArgb(230, 212, 239));
        graphics.FillRectangle(fill, left, top + 17, width * (sample.TotalMemory - sample.AvailableMemory) / sample.TotalMemory, 31);
        graphics.DrawRectangle(outline, left, top + 17, width, 31);
        Stat(graphics, "In use", Metrics.Bytes(sample.TotalMemory - sample.AvailableMemory), left, top + 64);
        Stat(graphics, "Available", Metrics.Bytes(sample.AvailableMemory), left + 185, top + 64);
        Stat(graphics, "Committed", $"{sample.CommitBytes / 1073741824d:0.0}/{sample.CommitLimit / 1073741824d:0.0} GB", left, top + 119, false);
        Stat(graphics, "Cached", Metrics.Bytes(sample.CachedBytes), left + 185, top + 119, false);
        if (width >= 470)
        {
            var right = left + width - 190;
            Pair(graphics, "Speed:", memoryHardware.Speed > 0 ? $"{memoryHardware.Speed} MHz" : "—", right, top + 62, 190);
            Pair(graphics, "Slots used:", memoryHardware.Slots > 0 ? $"{memoryHardware.UsedSlots} of {memoryHardware.Slots}" : "—", right, top + 83, 190);
            Pair(graphics, "Form factor:", memoryHardware.FormFactor, right, top + 104, 190);
            Pair(graphics, "Hardware reserved:", memoryHardware.InstalledBytes >= sample.TotalMemory ? Metrics.Bytes(memoryHardware.InstalledBytes - sample.TotalMemory) : "—", right, top + 125, 190);
        }
        Stat(graphics, "Paged pool", Metrics.Bytes(sample.PagedPool), left, top + 164, false);
        Stat(graphics, "Non-paged pool", Metrics.Bytes(sample.NonPagedPool), left + 185, top + 164, false);
    }

    private void PaintDisk(Graphics graphics, float left, float top)
    {
        var disk = sample?.Disks.FirstOrDefault(disk => "disk:" + disk.Name == selection);
        if (disk == null) return;
        Stat(graphics, "Active time", disk.Active is double active ? $"{active:0}%" : "Unavailable", left, top);
        Stat(graphics, "Average response time", disk.ResponseMilliseconds.HasValue ? $"{disk.ResponseMilliseconds.Value:0.0} ms" : "—", left + 190, top);
        Stat(graphics, "Read speed", disk.ReadRate is double read ? Metrics.Bytes(read) + "/s" : "—", left, top + 65);
        Stat(graphics, "Write speed", disk.WriteRate is double write ? Metrics.Bytes(write) + "/s" : "—", left + 190, top + 65);
        Pair(graphics, "Capacity:", disk.Hardware?.Capacity is long capacity ? Metrics.Bytes(capacity) : "—", left + 380, top + 4, 190);
        Pair(graphics, "Type:", disk.Hardware?.Type ?? "—", left + 380, top + 28, 190);
    }

    private void PaintNetwork(Graphics graphics, float left, float top, float width)
    {
        var network = sample?.Networks.FirstOrDefault(network => "net:" + network.Id == selection);
        if (network == null) return;
        Stat(graphics, "Send", Metrics.Bits(network.SendRate), left, top);
        Stat(graphics, "Receive", Metrics.Bits(network.ReceiveRate), left + 180, top);
        var right = left + Math.Max(330, width - 300);
        var pairWidth = Math.Min(300, width - (right - left));
        Pair(graphics, "Adapter name:", network.Name, right, top + 4, pairWidth);
        Pair(graphics, "Connection type:", network.Type, right, top + 25, pairWidth);
        Pair(graphics, "IPv4 address:", network.Address.Length > 0 ? network.Address : "—", right, top + 46, pairWidth);
        Pair(graphics, "IPv6 address:", network.Ipv6.Length > 0 ? network.Ipv6 : "—", right, top + 67, pairWidth);
    }

    // "S: 0  R: 8.0 Kbps", with the unit written once when both rates share it, as in the Windows sidebar.
    private static string SendReceive(NetworkSample network)
    {
        var send = Metrics.Bits(network.SendRate);
        var receive = Metrics.Bits(network.ReceiveRate);
        var sendUnit = send[(send.LastIndexOf(' ') + 1)..];
        return sendUnit == receive[(receive.LastIndexOf(' ') + 1)..] ? $"S: {send[..send.LastIndexOf(' ')]}  R: {receive}" : $"S: {send}  R: {receive}";
    }

    private static string EngineType(string name) => name.Split(" - ", 2)[^1];

    private static string EngineTitle(string name) => name switch { "VideoDecode" => "Video Decode", "VideoEncode" => "Video Encode", "VideoProcessing" => "Video Processing", _ => name };

    private static int EngineOrder(string name) => name switch
    {
        "3D" => 0, "Copy" => 1, "Video Encode" => 2, "VideoEncode" => 2, "Video Decode" => 3, "VideoDecode" => 3, _ => 4
    };

    private void Stat(Graphics graphics, string label, string value, float left, float top, bool large = true)
    {
        DrawText(graphics, label, labelFont, Color.FromArgb(75, 75, 75), left, top);
        DrawText(graphics, value, large ? valueFont : sidebarFont, Color.Black, left, top + 17, 185, large ? 34 : 26);
    }

    private void Pair(Graphics graphics, string label, string value, float left, float top, float width)
    {
        DrawText(graphics, label, labelFont, Color.FromArgb(75, 75, 75), left, top, width, 20);
        DrawText(graphics, value, bodyFont, Color.Black, left + 113, top, Math.Max(40, width - 113), 20, true);
    }

    private static void DrawText(Graphics graphics, string text, Font font, Color color, float left, float top,
        float width = 500, float height = 22, bool right = false)
    {
        using var brush = new SolidBrush(color);
        using var format = new StringFormat { Trimming = StringTrimming.EllipsisCharacter,
            Alignment = right ? StringAlignment.Far : StringAlignment.Near };
        graphics.DrawString(text, font, brush, new RectangleF(left, top, width, height), format);
    }

    private static void DrawGraph(Graphics graphics, RectangleF bounds, History history, double time, double maximum, Color color, bool grid)
    {
        using var border = new Pen(color);
        using var gridPen = new Pen(Color.FromArgb(42, color));
        using var fill = new SolidBrush(Color.FromArgb(24, color));
        if (grid)
        {
            for (var index = 1; index < 10; index++)
                graphics.DrawLine(gridPen, bounds.Left, bounds.Top + bounds.Height * index / 10, bounds.Right, bounds.Top + bounds.Height * index / 10);
            for (var index = 1; index < 12; index++)
                graphics.DrawLine(gridPen, bounds.Left + bounds.Width * index / 12, bounds.Top, bounds.Left + bounds.Width * index / 12, bounds.Bottom);
        }
        var points = history.Points(time, maximum, bounds, false);
        if (points.Length >= 2)
        {
            var polygon = new PointF[points.Length + 2];
            Array.Copy(points, polygon, points.Length);
            polygon[^2] = new PointF(points[^1].X, bounds.Bottom);
            polygon[^1] = new PointF(points[0].X, bounds.Bottom);
            graphics.FillPolygon(fill, polygon);
            graphics.DrawLines(border, points);
        }
        if (maximum != 100 || history.HasSecondary)
        {
            var secondary = history.Points(time, maximum, bounds, true);
            if (secondary.Length >= 2)
            {
                using var secondaryPen = new Pen(color) { DashStyle = DashStyle.Dash };
                graphics.DrawLines(secondaryPen, secondary);
            }
        }
        graphics.DrawRectangle(border, bounds.X, bounds.Y, bounds.Width, bounds.Height);
    }

    protected override void OnMouseDown(MouseEventArgs args)
    {
        base.OnMouseDown(args);
        Focus();
        var scale = DeviceDpi / 96f;
        if (args.X / scale > 220) return;
        var index = (int)(args.Y / scale - 12 + scrollOffset) / 82;
        if (index < 0 || index >= resources.Count) return;
        selection = resources[index].Key;
        Invalidate();
    }

    protected override void OnMouseWheel(MouseEventArgs args)
    {
        base.OnMouseWheel(args);
        scrollOffset -= Math.Sign(args.Delta) * 82;
        ClampScroll();
        Invalidate();
    }

    protected override void OnKeyDown(KeyEventArgs args)
    {
        base.OnKeyDown(args);
        if (args.Control && args.KeyCode == Keys.C) { Clipboard.SetText(Summary); args.Handled = true; }
        if (args.KeyCode is not (Keys.Up or Keys.Down or Keys.Home or Keys.End)) return;
        var index = resources.FindIndex(resource => resource.Key == selection);
        index = args.KeyCode switch { Keys.Home => 0, Keys.End => resources.Count - 1, Keys.Up => index - 1, _ => index + 1 };
        if (resources.Count == 0) return;
        index = Math.Clamp(index, 0, resources.Count - 1);
        selection = resources[index].Key;
        var height = ClientSize.Height * 96 / DeviceDpi;
        if (index * 82 < scrollOffset) scrollOffset = index * 82;
        if ((index + 1) * 82 + 24 > scrollOffset + height) scrollOffset = (index + 1) * 82 + 24 - height;
        ClampScroll();
        Invalidate();
        args.Handled = true;
    }

    private void ClampScroll() => scrollOffset = Math.Clamp(scrollOffset, 0, Math.Max(0, resources.Count * 82 + 24 - ClientSize.Height * 96 / DeviceDpi));

    protected override void Dispose(bool disposing)
    {
        if (disposing) { titleFont.Dispose(); bodyFont.Dispose(); labelFont.Dispose(); valueFont.Dispose(); sidebarFont.Dispose(); }
        base.Dispose(disposing);
    }

    private sealed record Resource(string Key, string Title, string Value, string Description, Color Color, string Detail = "");

    private sealed class History
    {
        private readonly Point[] samples = new Point[256];
        private int count;
        private int cursor;
        private readonly Dictionary<(double Time, double Maximum, RectangleF Bounds, bool Secondary), PointF[]> pointCache = new();
        public bool HasSecondary { get; private set; }

        public void Add(double time, double value, double secondary)
        {
            samples[cursor] = new Point(time, value, secondary);
            pointCache.Clear();
            cursor = (cursor + 1) % samples.Length;
            count = Math.Min(count + 1, samples.Length);
            HasSecondary = secondary != 0 || HasSecondary;
        }

        public double Maximum(double time)
        {
            var maximum = 125d;
            for (var index = 0; index < count; index++)
            {
                var point = samples[index];
                if (point.Time >= time - 60) maximum = Math.Max(maximum, Math.Max(point.Value, point.Secondary));
            }
            return Math.Pow(2, Math.Ceiling(Math.Log2(maximum * 1.1)));
        }

        public PointF[] Points(double time, double maximum, RectangleF bounds, bool secondary)
        {
            var key = (time, maximum, bounds, secondary);
            if (pointCache.TryGetValue(key, out var cached)) return cached;
            var points = new List<PointF>(count);
            for (var index = 0; index < count; index++)
            {
                var point = samples[(cursor - count + index + samples.Length) % samples.Length];
                var value = secondary ? point.Secondary : point.Value;
                if (point.Time < time - 60 || !double.IsFinite(value)) continue;
                points.Add(new PointF(bounds.Left + (float)((point.Time - (time - 60)) / 60) * bounds.Width,
                    bounds.Bottom - (float)Math.Clamp(value / maximum, 0, 1) * bounds.Height));
            }
            if (pointCache.Count >= 4) pointCache.Clear();
            return pointCache[key] = points.ToArray();
        }

        private readonly record struct Point(double Time, double Value, double Secondary);
    }
}
