using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using TaskManager.Monitoring;

namespace TaskManager.UI;

internal sealed partial class MainForm : Form
{
    private readonly AppSettings settings;
    private readonly bool diagnosticMode;
    private readonly bool monitorInDiagnostics;
    private readonly TabControl tabs = new() { Dock = DockStyle.Top, Padding = new Point(6, 3) };
    private readonly Panel content = new() { Dock = DockStyle.Fill, BackColor = Color.White };
    private readonly Control[] pages = new Control[7];
    private readonly ProcessList processes = new(false);
    private readonly ProcessList details = new(true);
    private readonly PerformanceView performance = new();
    private readonly InventoryView users = InventoryList(("User", 240, false), ("Status", 120, false), ("CPU", 80, true), ("Memory", 110, true), ("Disk", 90, true), ("Network", 90, true), ("ID", 0, true));
    private readonly InventoryView startup = InventoryList(("Name", 280, false), ("Publisher", 200, false), ("Status", 110, false), ("Startup impact", 110, false));
    private readonly InventoryView services = InventoryList(("Name", 170, false), ("PID", 60, true), ("Description", 300, false), ("Status", 90, false), ("Group", 200, false));
    private readonly InventoryView compact = InventoryList(("Name", 300, false));
    private readonly Panel footer = new() { Dock = DockStyle.Bottom, BackColor = Color.White };
    private readonly Label status = new() { AutoSize = false, TextAlign = ContentAlignment.MiddleLeft };
    private readonly Button endTask = new() { Text = "End task", Enabled = false, UseVisualStyleBackColor = true };
    private readonly DetailsButton toggleDetails = new();
    private readonly LinkLabel resourceMonitor = new() { Text = "Open Resource Monitor", AutoSize = true, LinkColor = Color.FromArgb(0, 102, 204), ActiveLinkColor = Color.FromArgb(0, 102, 204) };
    private readonly Label startupBios = new() { AutoSize = true, ForeColor = Color.FromArgb(76, 96, 122) };
    private readonly System.Windows.Forms.Timer refreshTimer = new() { Interval = 1000 };
    private readonly Panel searchPanel = new() { Dock = DockStyle.Top, Height = 38, Padding = new Padding(10, 5, 10, 5) };
    private readonly TextBox searchBox = new() { Dock = DockStyle.Fill, PlaceholderText = "Search name, PID or publisher (Alt+F)", AccessibleName = "Search tasks" };
    private readonly NotifyIcon tray = new() { Text = "Task Manager" };
    private int notificationQueued;
    internal double LastDeliveryMilliseconds { get; private set; }
    internal double LastUiUpdateMilliseconds { get; private set; }
    internal string SearchText { get => searchBox.Text; set => searchBox.Text = value; }
    private readonly CancellationTokenSource cancellation = new();
    private readonly SemaphoreSlim refreshSignal = new(0, 1);
    private readonly List<MenuEntry> menuCommands = [];
    private nint menuBar;
    private sealed record PublishedSample(SystemSample Sample, long Published);
    private PublishedSample? pending;
    private SystemSample? current;
    private string? samplerError;
    private Task? samplingTask;
    private int interval;
    private bool minimized;
    private bool compactMode;
    private bool inventoryLoading;
    private bool resourcesDisposed;
    private bool endingTasks;
    private string? inventoryError;
    private int inventoryTab = -1;
    private DateTime inventoryUpdated = DateTime.MinValue;
    private Size detailedSize;
    private readonly HashSet<int> expandedSessions = [];

    internal SystemSample? CurrentSample => current;
    internal Task? SamplingTask => samplingTask;
    internal string? SamplingError => Volatile.Read(ref samplerError);
    internal int UpdateInterval { get => Volatile.Read(ref interval); set => SetSpeed(value); }
    internal int SelectedTab { get => tabs.SelectedIndex; set => tabs.SelectedIndex = value; }
    internal ProcessList ProcessView => processes;
    internal ProcessList DetailsView => details;
    internal PerformanceView Performance => performance;
    internal void RefreshNow() => RequestRefresh();
    internal void ToggleDetailsForTest() => ToggleDetails();

    public MainForm(AppSettings? settings = null, bool monitorInDiagnostics = false)
    {
        diagnosticMode = settings != null;
        appHistory = new AppHistory(!diagnosticMode);
        this.monitorInDiagnostics = monitorInDiagnostics;
        this.settings = settings ?? AppSettings.Load();
        processes.GroupByType = this.settings.GroupByType;
        processes.RestoreColumns(this.settings.ProcessColumns);
        details.RestoreColumns(this.settings.DetailColumns);
        interval = this.settings.UpdateInterval;
        Text = "Task Manager";
        try { Icon = Icon.ExtractAssociatedIcon(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "Taskmgr.exe")) ?? SystemIcons.Application; }
        catch (Exception error) when (error is IOException or ArgumentException) { Icon = SystemIcons.Application; }
        Font = new Font("Segoe UI", 9);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(this.settings.Width, this.settings.Height);
        MinimumSize = new Size(740, 480);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = SystemColors.Window;
        TopMost = this.settings.AlwaysOnTop;
        KeyPreview = true;
        allHistory = this.settings.ShowAllHistory;

        var history = CreateHistoryView();
        AddTab(0, "Processes", processes);
        AddTab(1, "Performance", performance);
        AddTab(2, "App history", history);
        AddTab(3, "Startup", WrapStartup());
        AddTab(4, "Users", users);
        AddTab(5, "Details", details);
        AddTab(6, "Services", services);
        tabs.SelectedIndex = this.settings.SelectedTab;
        tabs.SelectedIndexChanged += (_, _) => TabChanged();
        compact.Visible = false;
        compact.Dock = DockStyle.Fill;
        compact.HeaderStyle = ColumnHeaderStyle.None;
        compact.SmallImageList = IconCache.Images;
        compact.Resize += (_, _) => compact.Columns[0].Width = Math.Max(Scale(100), compact.ClientSize.Width);
        startup.SmallImageList = IconCache.Images;
        users.SmallImageList = IconCache.Images;

        toggleDetails.Click += (_, _) => ToggleDetails();
        endTask.Click += (_, _) => EndSelected();
        resourceMonitor.LinkClicked += (_, _) => TryAction(() => Launch(tabs.SelectedIndex == 6 ? "services.msc" : "resmon.exe"));
        footer.Controls.AddRange([toggleDetails, status, resourceMonitor, endTask]);
        footer.Layout += (_, _) => LayoutFooter();
        Padding = new Padding(0, 2, 0, 0);
        Controls.Add(content);
        Controls.Add(compact);
        Controls.Add(tabs);
        searchPanel.Controls.Add(searchBox);
        Controls.Add(searchPanel);
        Controls.Add(footer);
        searchBox.TextChanged += (_, _) => ApplySearch();
        tray.Icon = Icon;
        tray.DoubleClick += (_, _) => RestoreWindow();
        var trayMenu = new ContextMenuStrip();
        trayMenu.Items.Add("Show Task Manager", null, (_, _) => RestoreWindow());
        trayMenu.Items.Add("Exit", null, (_, _) => Close());
        tray.ContextMenuStrip = trayMenu;

        foreach (var list in new ListView[] { processes, details, compact })
        {
            list.SelectedIndexChanged += (_, _) => UpdateEndButton();
            list.KeyDown += (_, args) => { if (args.KeyCode == Keys.Delete) { EndSelected(); args.Handled = true; } };
        }
        startup.SelectedIndexChanged += (_, _) => UpdateEndButton();
        users.SelectedIndexChanged += (_, _) => UpdateEndButton();
        users.DoubleClick += (_, _) => ToggleUserProcesses();
        users.KeyDown += (_, args) =>
        {
            if (users.SelectedItems.FirstOrDefault()?.Tag is not UserSession session) return;
            if (args.KeyCode == Keys.Right && !expandedSessions.Contains(session.Id) || args.KeyCode == Keys.Left && expandedSessions.Contains(session.Id))
            {
                ToggleUserProcesses();
                args.Handled = true;
            }
        };
        AddProcessMenu(processes);
        AddProcessMenu(details);
        AddProcessMenu(compact);
        AddManagementMenus();
        refreshTimer.Tick += (_, _) => ConsumeSample();
        Resize += (_, _) =>
        {
            var isMinimized = WindowState == FormWindowState.Minimized;
            Volatile.Write(ref minimized, isMinimized);
            if (isMinimized && this.settings.HideWhenMinimized && !diagnosticMode)
            {
                tray.Visible = true;
                Hide();
            }
        };
        TabChanged();
    }

    private static InventoryView InventoryList(params (string Name, int Width, bool Right)[] columns)
    {
        var list = new InventoryView();
        foreach (var column in columns) list.Columns.Add(column.Name, column.Width, column.Right ? HorizontalAlignment.Right : HorizontalAlignment.Left);
        return list;
    }

    private Control WrapStartup()
    {
        var panel = new Panel { Dock = DockStyle.Fill, BackColor = Color.White };
        var top = new Panel { Dock = DockStyle.Top, Height = 26, BackColor = Color.White };
        startupBios.Text = SystemInventory.LastBiosTime() is double seconds ? $"Last BIOS time:  {seconds:0.0} seconds" : "";
        top.Controls.Add(startupBios);
        top.Resize += (_, _) => startupBios.Location = new Point(top.Width - startupBios.Width - 18, 4);
        panel.Controls.Add(startup);
        panel.Controls.Add(top);
        return panel;
    }

    private void AddTab(int index, string title, Control page)
    {
        tabs.TabPages.Add(new TabPage(title) { BackColor = Color.White });
        page.Dock = DockStyle.Fill;
        page.Visible = false;
        pages[index] = page;
        content.Controls.Add(page);
    }

    protected override void OnHandleCreated(EventArgs args)
    {
        base.OnHandleCreated(args);
        var size = ClientSize;
        menuCommands.Clear();
        menuBar = NativeMenu.Build(MenuBar(), menuCommands, true);
        NativeMenu.SetMenu(Handle, compactMode ? 0 : menuBar);
        ClientSize = size;
        SendMessageW(tabs.Handle, 0x1331, 0, (nint)(12 * DeviceDpi / 96));
        LayoutChrome();
    }

    private MenuEntry[] MenuBar() =>
    [
        new("&File", Children: [new("&Run new task", RunNewTask), MenuEntry.Separator, new("E&xit", Close)]),
        new("&Options", Children:
        [
            new("&Always on top", () => TopMost = !TopMost, () => TopMost),
            new("&Minimize on use", () => settings.MinimizeOnUse = !settings.MinimizeOnUse, () => settings.MinimizeOnUse),
            new("&Hide when minimized", () => settings.HideWhenMinimized = !settings.HideWhenMinimized, () => settings.HideWhenMinimized),
            MenuEntry.Separator,
            new("Show &history for all processes", () => { allHistory = !allHistory; RefreshHistory(); }, () => allHistory)
        ]),
        new("&View", Children:
        [
            new("&Refresh now\tF5", RequestRefresh),
            new("&Update speed", Children:
            [
                new("&High", () => SetSpeed(500), () => interval == 500, Radio: true),
                new("&Normal", () => SetSpeed(1000), () => interval == 1000, Radio: true),
                new("&Low", () => SetSpeed(4000), () => interval == 4000, Radio: true),
                new("&Paused", () => SetSpeed(0), () => interval == 0, Radio: true)
            ]),
            MenuEntry.Separator,
            new("&Group by type", () => { processes.GroupByType = !processes.GroupByType; RefreshActiveView(); }, () => processes.GroupByType, () => tabs.SelectedIndex == 0 && !compactMode),
            new("&Expand all", () => processes.ExpandAll(true), Enabled: () => tabs.SelectedIndex == 0 && !compactMode),
            new("&Collapse all", () => processes.ExpandAll(false), Enabled: () => tabs.SelectedIndex == 0 && !compactMode)
        ])
    ];

    protected override void WndProc(ref Message message)
    {
        if (message.Msg == 0x117) NativeMenu.Refresh(message.WParam, menuCommands);
        if (message.Msg == 0x111 && message.LParam == 0 && (message.WParam >> 16) == 0)
        {
            var id = (int)(message.WParam & 0xffff);
            if (id > 0 && id <= menuCommands.Count) { menuCommands[id - 1].Action?.Invoke(); return; }
        }
        base.WndProc(ref message);
    }

    protected override void OnKeyDown(KeyEventArgs args)
    {
        base.OnKeyDown(args);
        if (args.KeyCode == Keys.F5) { RequestRefresh(); args.Handled = true; }
        else if (args.Control && args.KeyCode == Keys.Tab && !compactMode)
        {
            tabs.SelectedIndex = (tabs.SelectedIndex + (args.Shift ? 6 : 1)) % 7;
            args.Handled = true;
        }
    }

    protected override bool ProcessCmdKey(ref Message message, Keys keyData)
    {
        if (keyData is (Keys.Alt | Keys.F) or (Keys.Control | Keys.F))
        {
            if (compactMode) ToggleDetails();
            searchBox.Focus();
            searchBox.SelectAll();
            return true;
        }
        if (keyData == Keys.Escape && searchBox.TextLength > 0)
        {
            searchBox.Clear();
            pages[tabs.SelectedIndex].Focus();
            return true;
        }
        if (keyData == (Keys.Control | Keys.Shift | Keys.N)) { RunNewTask(); return true; }
        if ((keyData & Keys.Modifiers) == Keys.Alt && (keyData & Keys.KeyCode) is >= Keys.D1 and <= Keys.D7)
        {
            if (compactMode) ToggleDetails();
            tabs.SelectedIndex = (int)(keyData & Keys.KeyCode) - (int)Keys.D1;
            return true;
        }
        return base.ProcessCmdKey(ref message, keyData);
    }

    protected override void OnKeyUp(KeyEventArgs args)
    {
        base.OnKeyUp(args);
        if (args.KeyCode == Keys.ControlKey) ConsumeSample();
    }

    private void ApplySearch()
    {
        switch (tabs.SelectedIndex)
        {
            case 0: processes.Filter = searchBox.Text; break;
            case 2: historyList.Filter = searchBox.Text; break;
            case 3: startup.Filter = searchBox.Text; break;
            case 4: users.Filter = searchBox.Text; break;
            case 5: details.Filter = searchBox.Text; break;
            case 6: services.Filter = searchBox.Text; break;
        }
        UpdateEndButton();
    }

    private void RestoreWindow()
    {
        Show();
        WindowState = FormWindowState.Normal;
        tray.Visible = false;
        Activate();
        RequestRefresh();
    }

    protected override void OnDpiChanged(DpiChangedEventArgs args)
    {
        base.OnDpiChanged(args);
        LayoutChrome();
    }

    private int Scale(int value) => (int)Math.Round(value * DeviceDpi / 96d);

    private void LayoutChrome()
    {
        tabs.Height = tabs.ItemSize.Height + Scale(4);
        searchPanel.Height = Scale(38);
        footer.Height = Scale(51);
        content.Padding = Padding.Empty;
        LayoutFooter();
        LayoutPage();
    }

    private void LayoutPage()
    {
        // Measured against Windows 10 Task Manager: the process header starts 12px below the tab strip.
        var top = compactMode ? 0 : tabs.SelectedIndex switch { 0 => Scale(12), 1 => Scale(6), 5 or 6 => Scale(12), 2 or 3 => Scale(6), 4 => Scale(12), _ => 0 };
        content.Padding = new Padding(tabs.SelectedIndex == 0 ? Scale(1) : 0, top, 0, 0);
    }

    private void LayoutFooter()
    {
        toggleDetails.Bounds = new Rectangle(Scale(8), Scale(11), toggleDetails.PreferredSize.Width, Scale(28));
        endTask.Bounds = new Rectangle(footer.Width - Scale(94), Scale(13), Scale(85), Scale(23));
        var right = endTask.Visible ? endTask.Left - Scale(8) : footer.Width - Scale(12);
        resourceMonitor.Location = new Point(right - resourceMonitor.Width - (endTask.Visible ? Scale(8) : 0), (footer.Height - resourceMonitor.Height) / 2);
        var statusLeft = toggleDetails.Right + Scale(16);
        status.Bounds = new Rectangle(statusLeft, Scale(11), Math.Max(0, (resourceMonitor.Visible ? resourceMonitor.Left : right) - statusLeft - Scale(8)), Scale(28));
    }

    protected override void OnShown(EventArgs args)
    {
        base.OnShown(args);
        if (diagnosticMode && !monitorInDiagnostics) return;
        samplingTask = Task.Run(SamplingLoop);
        refreshTimer.Start();
    }

    private async Task SamplingLoop()
    {
        var token = cancellation.Token;
        try
        {
            using var sampler = new SystemSampler(true);
            PublishSample(sampler.Sample());
            await Task.Delay(200, token).ConfigureAwait(false);
            var force = true;
            while (!token.IsCancellationRequested)
            {
                var updateInterval = Volatile.Read(ref interval);
                if (updateInterval != 0 || force)
                {
                    try
                    {
                        PublishSample(sampler.Sample());
                        Volatile.Write(ref samplerError, null);
                    }
                    catch (Exception error) when (error is Win32Exception or InvalidOperationException or OverflowException)
                    { Volatile.Write(ref samplerError, error.Message); }
                }
                var delay = updateInterval == 0 ? Timeout.Infinite : Volatile.Read(ref minimized) ? Math.Max(updateInterval, 4000) : updateInterval;
                force = await refreshSignal.WaitAsync(delay, token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception error) { Volatile.Write(ref samplerError, error.Message); }
    }

    private void ConsumeSample()
    {
        if (WindowState == FormWindowState.Minimized || (ModifierKeys & Keys.Control) != 0) return;
        var published = Interlocked.Exchange(ref pending, null);
        if (published != null)
        {
            var next = published.Sample;
            LastDeliveryMilliseconds = Stopwatch.GetElapsedTime(published.Published).TotalMilliseconds;
            var started = Stopwatch.GetTimestamp();
            current = next;
            appHistory.Update(next);
            performance.UpdateSample(next);
            RefreshActiveView();
            LastUiUpdateMilliseconds = Stopwatch.GetElapsedTime(started).TotalMilliseconds;
        }
        var error = Volatile.Read(ref samplerError);
        if (error == null && tabs.SelectedIndex is 3 or 6 && inventoryTab == tabs.SelectedIndex) error = inventoryError;
        status.ForeColor = error == null ? SystemColors.ControlText : Color.Firebrick;
        status.Text = error != null ? "Sampling unavailable: " + error : interval == 0 && !compactMode ? "Updates paused" : "";
        if (!compactMode && tabs.SelectedIndex is 3 or 6 && !inventoryLoading &&
            (inventoryTab != tabs.SelectedIndex || (interval != 0 && tabs.SelectedIndex == 6 && (DateTime.UtcNow - inventoryUpdated).TotalSeconds >= 5)))
            _ = RefreshInventory(tabs.SelectedIndex);
    }

    private void PublishSample(SystemSample sample)
    {
        Interlocked.Exchange(ref pending, new PublishedSample(sample, Stopwatch.GetTimestamp()));
        if (cancellation.IsCancellationRequested || Volatile.Read(ref minimized) || Interlocked.Exchange(ref notificationQueued, 1) != 0) return;
        try
        {
            BeginInvoke(() =>
            {
                Interlocked.Exchange(ref notificationQueued, 0);
                if (!IsDisposed && !Disposing) ConsumeSample();
            });
        }
        catch (InvalidOperationException) { Interlocked.Exchange(ref notificationQueued, 0); }
    }

    private void RefreshActiveView()
    {
        if (current == null) return;
        if (compactMode) RefreshCompact();
        else switch (tabs.SelectedIndex)
        {
            case 0: processes.UpdateSample(current, searchBox.Text); break;
            case 2: RefreshHistory(); break;
            case 4: RefreshUsers(); break;
            case 5: details.UpdateSample(current, searchBox.Text); break;
        }
        UpdateEndButton();
    }

    private void RefreshCompact()
    {
        if (current == null) return;
        var rows = new List<ListViewItem>();
        var seen = new HashSet<string>(StringComparer.CurrentCultureIgnoreCase);
        foreach (var process in current.Processes.OrderBy(process => process.Created))
        {
            if (!current.Metadata.TryGetValue((process.Id, process.Created), out var information) || !information.IsApp) continue;
            var name = information.PackageName ?? information.DisplayName;
            if (seen.Add(name)) rows.Add(new ListViewItem(name) { Tag = process, ImageIndex = IconCache.Index(information.Icon) });
        }
        rows.Sort((left, right) => StringComparer.CurrentCultureIgnoreCase.Compare(left.Text, right.Text));
        compact.ReplaceRows(rows);
    }

    private void RefreshUsers()
    {
        if (current == null) return;
        var rows = new List<ListViewItem>();
        var sessions = current.Processes.ToLookup(process => process.Session);
        foreach (var session in current.Sessions)
        {
            var group = sessions[session.Id].ToArray();
            var expanded = expandedSessions.Contains(session.Id);
            rows.Add(new ListViewItem([(expanded ? "▾ " : "▸ ") + session.Name[(session.Name.LastIndexOf('\\') + 1)..], session.Status, Metrics.Percent(group.Sum(process => process.Cpu)),
                Metrics.Megabytes(group.Sum(process => process.WorkingSet)), group.All(process => process.DiskRate.HasValue) ? Metrics.DiskRate(group.Sum(process => process.DiskRate!.Value)) : "—",
                group.All(process => process.NetworkRate.HasValue) ? Metrics.NetworkRate(group.Sum(process => process.NetworkRate!.Value)) : "—", session.Id.ToString()]) { Tag = session });
            if (!expanded) continue;
            foreach (var process in group.Where(process => process.Id > 0).OrderBy(process => process.Name, StringComparer.CurrentCultureIgnoreCase))
            {
                var information = current.Metadata.GetValueOrDefault((process.Id, process.Created));
                rows.Add(new ListViewItem(["    " + (information?.DisplayName ?? process.Name), process.Status, Metrics.Percent(process.Cpu), Metrics.Megabytes(process.WorkingSet),
                    process.DiskRate.HasValue ? Metrics.DiskRate(process.DiskRate.Value) : "—", process.NetworkRate.HasValue ? Metrics.NetworkRate(process.NetworkRate.Value) : "—", process.Id.ToString()])
                    { Tag = process, ImageIndex = IconCache.Index(information?.Icon) });
            }
        }
        users.ReplaceRows(rows);
    }

    private void ToggleUserProcesses()
    {
        if (users.SelectedItems.FirstOrDefault()?.Tag is not UserSession session) return;
        if (!expandedSessions.Add(session.Id)) expandedSessions.Remove(session.Id);
        RefreshUsers();
    }

    private async Task RefreshInventory(int tab)
    {
        inventoryLoading = true;
        try
        {
            if (tab == 3)
            {
                var entries = await Task.Run(SystemInventory.Startup);
                if (IsDisposed || Disposing) return;
                startup.Filter = searchBox.Text;
                startup.ReplaceRows(entries.Select(entry => new ListViewItem([entry.DisplayName, entry.Publisher, entry.Status, "Not measured"])
                    { Tag = entry, ImageIndex = IconCache.Index(entry.ExecutablePath) }));
            }
            else
            {
                var entries = await Task.Run(SystemInventory.Services);
                if (IsDisposed || Disposing) return;
                services.Filter = searchBox.Text;
                services.ReplaceRows(entries.Select(entry => new ListViewItem([entry.Name, entry.ProcessId > 0 ? entry.ProcessId.ToString() : "", entry.DisplayName, entry.Status, entry.Group]) { Tag = entry }));
            }
            inventoryTab = tab;
            inventoryUpdated = DateTime.UtcNow;
            inventoryError = null;
        }
        catch (Exception error) when (error is Win32Exception or IOException or UnauthorizedAccessException or System.Security.SecurityException or InvalidOperationException)
        {
            inventoryError = "Inventory unavailable: " + error.Message;
            if (!IsDisposed) status.Text = inventoryError;
            inventoryTab = tab;
            inventoryUpdated = DateTime.UtcNow;
        }
        finally { inventoryLoading = false; }
    }

    private void TabChanged()
    {
        content.SuspendLayout();
        for (var index = 0; index < pages.Length; index++) pages[index].Visible = index == tabs.SelectedIndex;
        LayoutPage();
        content.ResumeLayout();
        resourceMonitor.Visible = !compactMode && tabs.SelectedIndex is 1 or 6;
        resourceMonitor.Text = tabs.SelectedIndex == 6 ? "Open Services" : "Open Resource Monitor";
        endTask.Visible = compactMode || tabs.SelectedIndex is 0 or 3 or 4 or 5;
        endTask.Text = tabs.SelectedIndex == 3 ? "Disable" : tabs.SelectedIndex == 4 ? "Disconnect" : "End task";
        LayoutFooter();
        RefreshActiveView();
        ApplySearch();
        if (tabs.SelectedIndex == 1) performance.Invalidate();
        UpdateEndButton();
    }

    private ProcessSample? Selected() => compactMode ? compact.SelectedItems.Count > 0 ? compact.SelectedItems[0].Tag as ProcessSample : null :
        tabs.SelectedIndex switch { 0 => processes.SelectedProcess, 4 => users.SelectedItems.FirstOrDefault()?.Tag as ProcessSample, 5 => details.SelectedProcess, _ => null };

    private void UpdateEndButton()
    {
        if (endingTasks) { endTask.Enabled = false; return; }
        if (!compactMode && tabs.SelectedIndex == 3)
        {
            endTask.Enabled = startup.SelectedItems.Count > 0;
            endTask.Text = startup.SelectedItems.Count > 0 && startup.SelectedItems[0].Tag is StartupEntry { Status: "Disabled" } ? "Enable" : "Disable";
            return;
        }
        if (!compactMode && tabs.SelectedIndex == 4 && Selected() == null)
        {
            endTask.Text = "Disconnect";
            endTask.Enabled = users.SelectedItems.FirstOrDefault()?.Tag is UserSession;
            return;
        }
        if (!compactMode && tabs.SelectedIndex == 4) endTask.Text = "End task";
        var selected = Selected();
        endTask.Enabled = selected != null && selected.Id > 4 && selected.Id != Environment.ProcessId;
    }

    private async void EndSelected()
    {
        if (endingTasks) return;
        if (!compactMode && tabs.SelectedIndex == 3) { ChangeSelectedStartup(); return; }
        if (!compactMode && tabs.SelectedIndex == 4 && Selected() == null) { DisconnectSelectedUser(); return; }
        var selected = Selected();
        if (selected == null || selected.Id <= 4 || selected.Id == Environment.ProcessId) return;
        var group = !compactMode && tabs.SelectedIndex == 0 ? processes.SelectedGroup : null;
        if (group is { Length: > 1 })
        {
            if (!Confirm($"Do you want to end {processes.SelectedName} and its {group.Length - 1} related processes?\r\n\r\nAny unsaved work in these processes will be lost.")) return;
            await EndProcesses(group.Where(member => member.Id > 4 && member.Id != Environment.ProcessId).ToArray());
        }
        else
        {
            if (!Confirm($"Do you want to end {selected.Name}?\r\n\r\nAny unsaved work in this process will be lost.")) return;
            await EndProcesses([selected]);
        }
    }

    private async Task EndProcesses(ProcessSample[] selected)
    {
        endingTasks = true;
        UpdateEndButton();
        try { await Task.Run(() => { foreach (var process in selected) ProcessActions.End(process); }); }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException or UnauthorizedAccessException)
        { if (!IsDisposed) MessageBox.Show(this, error.Message, "Task Manager", MessageBoxButtons.OK, MessageBoxIcon.Information); }
        finally
        {
            endingTasks = false;
            if (!IsDisposed && !Disposing) { RequestRefresh(); UpdateEndButton(); }
        }
    }

    private void AddProcessMenu(ListView list)
    {
        void Show(Point screen)
        {
            var selected = list == compact ? Selected() : ((ProcessList)list).SelectedProcess;
            if (selected == null) return;
            var entries = new List<MenuEntry>();
            if (list == processes && processes.SelectedIsGroup)
                entries.Add(new(processes.SelectedExpanded ? "Collapse" : "Expand", processes.ToggleSelected, Default: true));
            if (metadataIsApp(selected)) entries.Add(new("Switch to", () => SwitchTo(selected), Default: list != processes || !processes.SelectedIsGroup));
            entries.Add(new("End task", EndSelected, Enabled: () => selected.Id > 4 && selected.Id != Environment.ProcessId));
            if (selected.Name.Equals("explorer.exe", StringComparison.OrdinalIgnoreCase))
                entries.Add(new("Restart Windows Explorer", () => RestartExplorer(selected)));
            var efficiency = ProcessActions.EfficiencyMode(selected);
            entries.Add(new("Efficiency mode", () =>
            {
                if (efficiency != true && !Confirm("Enable Efficiency mode for " + selected.Name + "? This lowers CPU priority and enables energy-efficient execution. The process may become less responsive. Disabling restores Normal priority.")) return;
                TryAction(() => ProcessActions.EfficiencyMode(selected, efficiency != true));
                RequestRefresh();
            }, () => efficiency == true, () => efficiency.HasValue && selected.Id > 4 && selected.Id != Environment.ProcessId));
            entries.Add(new("Create memory dump file", async () => await DumpSelected(selected), Enabled: () => selected.Id > 4 && selected.Id != Environment.ProcessId));
            if (list == details)
            {
                entries.Add(new("End process tree", () => EndTree(selected), Enabled: () => selected.Id > 4 && selected.Id != Environment.ProcessId));
                entries.Add(MenuEntry.Separator);
                entries.Add(PriorityMenu(selected));
                entries.Add(new("Set affinity", () => TryAction(() => SetAffinity(selected))));
                entries.Add(new("Analyze wait chain", async () => await AnalyzeWaitChain(selected), Enabled: () => selected.Id > 4 && !analyzingWaitChain));
            }
            entries.Add(MenuEntry.Separator);
            if (list != details)
                entries.Add(new("Go to details", () => { if (compactMode) ToggleDetails(); searchBox.Clear(); tabs.SelectedIndex = 5; details.SelectProcess(selected.Id); details.Focus(); }));
            entries.Add(new("Open file location", () => TryAction(() => Launch("explorer.exe", "/select,", ProcessActions.PathOf(selected)))));
            entries.Add(new("Search online", () => TryAction(() => Launch("https://www.bing.com/search?q=" + Uri.EscapeDataString(selected.Name)))));
            entries.Add(new("Properties", () => TryAction(() => ShowProperties(ProcessActions.PathOf(selected)))));
            NativeMenu.Show(this, screen, entries);
        }
        list.MouseUp += (_, args) => { if (args.Button == MouseButtons.Right) Show(list.PointToScreen(args.Location)); };
        list.KeyUp += (_, args) =>
        {
            if (args.KeyCode != Keys.Apps && !(args.Shift && args.KeyCode == Keys.F10)) return;
            var bounds = list.SelectedIndices.Count > 0 ? list.GetItemRect(list.SelectedIndices[0]) : Rectangle.Empty;
            Show(list.PointToScreen(new Point(bounds.Left + 40, bounds.Bottom)));
        };
    }

    private bool metadataIsApp(ProcessSample process) =>
        current?.Metadata.TryGetValue((process.Id, process.Created), out var information) == true && information.IsApp;

    private void SwitchTo(ProcessSample process)
    {
        TryAction(() => ProcessActions.SwitchTo(process));
        if (settings.MinimizeOnUse) WindowState = FormWindowState.Minimized;
    }

    private async void EndTree(ProcessSample root)
    {
        if (endingTasks) return;
        if (current == null || !Confirm($"Do you want to end the process tree of {root.Name}?\r\n\r\nThis will end the process and all processes started by it. Any unsaved work will be lost.")) return;
        var tree = new List<ProcessSample> { root };
        var children = current.Processes.ToLookup(process => process.ParentId);
        for (var index = 0; index < tree.Count; index++)
            tree.AddRange(children[tree[index].Id].Where(process => process.Created > tree[index].Created && process.Id > 4 && process.Id != Environment.ProcessId));
        await EndProcesses(Enumerable.Reverse(tree).ToArray());
    }

    private MenuEntry PriorityMenu(ProcessSample process)
    {
        var levels = new (string Name, ProcessPriorityClass Value)[] { ("Realtime", ProcessPriorityClass.RealTime), ("High", ProcessPriorityClass.High),
            ("Above normal", ProcessPriorityClass.AboveNormal), ("Normal", ProcessPriorityClass.Normal), ("Below normal", ProcessPriorityClass.BelowNormal), ("Low", ProcessPriorityClass.Idle) };
        var currentPriority = ProcessActions.CurrentPriority(process);
        return new("Set priority", Children: levels.Select(level => new MenuEntry(level.Name, () =>
        {
            if (!Confirm($"Do you want to change the priority of {process.Name}?\r\n\r\nChanging the priority of certain processes could cause system instability.")) return;
            TryAction(() => ProcessActions.Priority(process, level.Value));
        }, () => currentPriority == level.Value, () => level.Value != ProcessPriorityClass.RealTime, Radio: true)).ToArray());
    }

    private void SetSpeed(int value)
    {
        Volatile.Write(ref interval, value);
        if (value != 0) RequestRefresh();
        ConsumeSample();
    }

    private void RequestRefresh()
    {
        if (refreshSignal.CurrentCount == 0) refreshSignal.Release();
        inventoryUpdated = DateTime.MinValue;
        inventoryTab = -1;
    }

    private void ToggleDetails()
    {
        compactMode = !compactMode;
        tabs.Visible = !compactMode;
        searchPanel.Visible = !compactMode;
        content.Visible = !compactMode;
        compact.Visible = compactMode;
        status.Visible = !compactMode;
        // Like Windows, the compact view has no menu bar.
        NativeMenu.SetMenu(Handle, compactMode ? 0 : menuBar);
        if (compactMode)
        {
            detailedSize = ClientSize;
            MinimumSize = new Size(Scale(250), Scale(200));
            ClientSize = new Size(Scale(360), Scale(360));
        }
        else
        {
            MinimumSize = new Size(740, 480);
            ClientSize = detailedSize;
        }
        toggleDetails.Text = compactMode ? "More details" : "Fewer details";
        toggleDetails.Invalidate();
        TabChanged();
    }

    private void RunNewTask()
    {
        using var dialog = new Form
        {
            Text = "Create new task", Font = Font, FormBorderStyle = FormBorderStyle.FixedDialog, AutoScaleMode = AutoScaleMode.Dpi,
            StartPosition = FormStartPosition.CenterParent, ClientSize = new Size(Scale(400), Scale(200)), MaximizeBox = false, MinimizeBox = false, ShowInTaskbar = false
        };
        var icon = new PictureBox { Image = SystemIcons.Application.ToBitmap(), Location = new Point(Scale(18), Scale(18)), Size = new Size(Scale(32), Scale(32)), SizeMode = PictureBoxSizeMode.Zoom };
        var label = new Label { Text = "Type the name of a program, folder, document, or Internet resource, and Windows will open it for you.",
            Location = new Point(Scale(64), Scale(18)), Size = new Size(Scale(320), Scale(36)) };
        var openLabel = new Label { Text = "&Open:", AutoSize = true, Location = new Point(Scale(18), Scale(71)) };
        var executable = new ComboBox { Location = new Point(Scale(64), Scale(67)), Width = Scale(318) };
        var elevated = new CheckBox { Text = "Create this task with administrative privileges.", AutoSize = true, Location = new Point(Scale(64), Scale(102)),
            Visible = true };
        var shield = new PictureBox { Location = new Point(Scale(40), Scale(102)), Size = new Size(Scale(16), Scale(16)), SizeMode = PictureBoxSizeMode.Zoom };
        try { shield.Image = new Icon(SystemIcons.Shield, 16, 16).ToBitmap(); } catch (ArgumentException) { }
        var bottom = new Panel { Dock = DockStyle.Bottom, Height = Scale(46), BackColor = SystemColors.Control };
        var okay = new Button { Text = "OK", DialogResult = DialogResult.OK, Size = new Size(Scale(75), Scale(23)), Location = new Point(Scale(141), Scale(12)), UseVisualStyleBackColor = true };
        var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, Size = new Size(Scale(75), Scale(23)), Location = new Point(Scale(224), Scale(12)), UseVisualStyleBackColor = true };
        var browse = new Button { Text = "&Browse...", Size = new Size(Scale(75), Scale(23)), Location = new Point(Scale(307), Scale(12)), UseVisualStyleBackColor = true };
        bottom.Controls.AddRange([okay, cancel, browse]);
        browse.Click += (_, _) =>
        {
            using var picker = new OpenFileDialog { Title = "Browse", Filter = "Programs (*.exe;*.pif;*.com;*.bat;*.cmd)|*.exe;*.pif;*.com;*.bat;*.cmd|All files (*.*)|*.*" };
            if (picker.ShowDialog(dialog) == DialogResult.OK) executable.Text = picker.FileName;
        };
        dialog.Controls.AddRange([icon, label, openLabel, executable, elevated, shield, bottom]);
        dialog.AcceptButton = okay;
        dialog.CancelButton = cancel;
        if (dialog.ShowDialog(this) != DialogResult.OK || string.IsNullOrWhiteSpace(executable.Text)) return;
        var text = executable.Text.Trim();
        var (file, arguments) = SplitCommand(text);
        TryAction(() => Process.Start(new ProcessStartInfo(file) { Arguments = arguments, UseShellExecute = true,
            Verb = elevated.Checked ? "runas" : "open" })?.Dispose());
    }

    private static (string File, string Arguments) SplitCommand(string text)
    {
        if (text.StartsWith('"'))
        {
            var end = text.IndexOf('"', 1);
            return end > 0 ? (text[1..end], text[(end + 1)..].Trim()) : (text.Trim('"'), "");
        }
        if (File.Exists(text) || Directory.Exists(text)) return (text, "");
        var space = text.IndexOf(' ');
        return space > 0 ? (text[..space], text[(space + 1)..]) : (text, "");
    }

    private void TryAction(Action action)
    {
        try { action(); }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException or ArgumentException or NotSupportedException or IOException or UnauthorizedAccessException or System.Security.SecurityException)
        { MessageBox.Show(this, error.Message, "Task Manager", MessageBoxButtons.OK, MessageBoxIcon.Information); }
    }

    private static void Launch(string executable, params string[] arguments)
    {
        var start = new ProcessStartInfo(executable) { UseShellExecute = true };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        Process.Start(start)?.Dispose();
    }

    private static void ShowProperties(string path)
    {
        var information = new ShellExecuteInfo { Size = Marshal.SizeOf<ShellExecuteInfo>(), Mask = 0xC, Verb = "properties", File = path, Show = 5 };
        if (!ShellExecuteExW(ref information)) throw new Win32Exception();
    }

    protected override void OnFormClosing(FormClosingEventArgs args)
    {
        base.OnFormClosing(args);
        if (args.Cancel) return;
        cancellation.Cancel();
        refreshTimer.Stop();
        if (diagnosticMode) return;
        settings.Width = compactMode ? detailedSize.Width : ClientSize.Width;
        settings.Height = compactMode ? detailedSize.Height : ClientSize.Height;
        settings.SelectedTab = tabs.SelectedIndex;
        settings.UpdateInterval = interval;
        settings.AlwaysOnTop = TopMost;
        settings.GroupByType = processes.GroupByType;
        settings.ShowAllHistory = allHistory;
        settings.ProcessColumns = processes.CaptureColumns();
        settings.DetailColumns = details.CaptureColumns();
        try { settings.Save(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Debug.WriteLine(error.Message); }
        try { appHistory.Save(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Debug.WriteLine(error.Message); }
    }

    internal void RenderSample(SystemSample sample, int selectedTab, string path)
    {
        if (!diagnosticMode) throw new InvalidOperationException("Rendering is only available in diagnostic mode.");
        if (!Visible)
        {
            ShowInTaskbar = false;
            Opacity = 0;
            Show();
        }
        current = sample;
        performance.UpdateSample(sample);
        tabs.SelectedIndex = selectedTab;
        RefreshActiveView();
        CreateControl();
        PerformLayout();
        Application.DoEvents();
        using var bitmap = new Bitmap(Width, Height);
        DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
        bitmap.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }

    protected override bool ShowWithoutActivation => diagnosticMode;

    protected override void Dispose(bool disposing)
    {
        if (disposing && !resourcesDisposed)
        {
            resourcesDisposed = true;
            cancellation.Cancel();
            refreshTimer.Dispose();
            tray.ContextMenuStrip?.Dispose();
            tray.Dispose();
            if (samplingTask == null || samplingTask.IsCompleted) { cancellation.Dispose(); refreshSignal.Dispose(); }
            else _ = samplingTask.ContinueWith(_ => { cancellation.Dispose(); refreshSignal.Dispose(); }, TaskScheduler.Default);
        }
        base.Dispose(disposing);
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ShellExecuteInfo
    {
        public int Size; public uint Mask; public nint Window; public string? Verb; public string? File; public string? Parameters;
        public string? Directory; public int Show; public nint InstanceApp; public nint IdList; public string? Class; public nint KeyClass;
        public uint HotKey; public nint Icon; public nint Process;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool ShellExecuteExW(ref ShellExecuteInfo information);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern nint SendMessageW(nint window, uint message, nint wParam, nint lParam);
}
