using System.ComponentModel;
using System.Diagnostics;
using TaskManager.Monitoring;

namespace TaskManager.UI;

internal sealed partial class MainForm
{
    private readonly InventoryView historyList = InventoryList(("Name", 280, false), ("CPU time", 100, true), ("Network", 100, true), ("Metered network", 120, true), ("Tile updates", 100, true));
    private readonly Label historySince = new() { AutoSize = true };
    private readonly AppHistory appHistory;
    private bool allHistory;
    private bool analyzingWaitChain;

    private async Task AnalyzeWaitChain(ProcessSample selected)
    {
        if (analyzingWaitChain) return;
        analyzingWaitChain = true;
        try
        {
            var report = await Task.Run(() => WaitChains.Analyze(selected));
            if (IsDisposed || Disposing) return;
            using var dialog = new Form { Text = "Wait chain: " + selected.Name, ClientSize = new Size(700, 420), StartPosition = FormStartPosition.CenterParent, ShowInTaskbar = false };
            dialog.Controls.Add(new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, Dock = DockStyle.Fill, Text = report, WordWrap = false });
            dialog.ShowDialog(this);
        }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException or ArgumentException)
        { if (!IsDisposed) MessageBox.Show(this, error.Message, "Task Manager", MessageBoxButtons.OK, MessageBoxIcon.Information); }
        finally { analyzingWaitChain = false; }
    }

    private void RestartExplorer(ProcessSample selected)
    {
        if (!Confirm("Restart Windows Explorer? Open Explorer windows may close and the desktop and taskbar will briefly disappear.")) return;
        TryAction(() =>
        {
            var path = ProcessActions.PathOf(selected);
            using var own = Process.GetCurrentProcess();
            if (selected.Session != own.SessionId || !path.Equals(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe"), StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Only the installed Windows Explorer in your own session can be restarted.");
            ProcessActions.End(selected);
            Launch(path);
        });
        RequestRefresh();
    }

    private async Task DumpSelected(ProcessSample selected)
    {
        if (!Confirm("A full memory dump can contain passwords, personal data and other secrets from " + selected.Name + ". Save it only to a trusted location. Continue?")) return;
        using var dialog = new SaveFileDialog { Filter = "Memory dumps (*.dmp)|*.dmp", FileName = selected.Name + "." + selected.Id + ".dmp", OverwritePrompt = true };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        try
        {
            await Task.Run(() => ProcessActions.Dump(selected, dialog.FileName));
            if (!IsDisposed) MessageBox.Show(this, "Memory dump saved to " + dialog.FileName, "Task Manager", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException or IOException or UnauthorizedAccessException)
        {
            if (!IsDisposed) MessageBox.Show(this, error.Message, "Task Manager", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
    }

    private async void ChangeSelectedStartup()
    {
        if (startup.SelectedItems.Count == 0 || startup.SelectedItems[0].Tag is not StartupEntry entry) return;
        await ChangeStartup(entry, entry.Status == "Disabled");
    }

    private async Task ChangeStartup(StartupEntry entry, bool enable)
    {
        TryAction(() => Management.Startup(entry, enable));
        if (!inventoryLoading) await RefreshInventory(3);
    }

    private void DisconnectSelectedUser()
    {
        if (users.SelectedItems.Count == 0 || users.SelectedItems[0].Tag is not UserSession session ||
            !Confirm($"Disconnect {session.Name}? Their applications will continue running.")) return;
        TryAction(() => Management.Disconnect(session.Id));
    }

    private Panel CreateHistoryView()
    {
        var panel = new Panel { Dock = DockStyle.Fill, BackColor = Color.White };
        var top = new Panel { Dock = DockStyle.Top, Height = 44, BackColor = Color.White };
        var delete = new LinkLabel { Text = "Delete usage history", AutoSize = true, LinkColor = Color.FromArgb(0, 102, 204), ActiveLinkColor = Color.FromArgb(0, 102, 204) };
        delete.LinkClicked += (_, _) =>
        {
            if (!Confirm("Delete this application's recorded usage history?")) return;
            TryAction(() => { appHistory.Clear(); RefreshHistory(); });
        };
        top.Controls.Add(historySince);
        top.Controls.Add(delete);
        top.Resize += (_, _) =>
        {
            historySince.Location = new Point(Scale(8), Scale(2));
            delete.Location = new Point(Scale(8), historySince.Bottom + Scale(2));
            top.Height = delete.Bottom + Scale(8);
        };
        panel.Controls.Add(historyList);
        panel.Controls.Add(top);
        return panel;
    }

    private void RefreshHistory()
    {
        historySince.Text = $"Resource usage since {appHistory.Started:d} for current user account.";
        historyList.ReplaceRows(appHistory.Entries(allHistory).Select(entry => new ListViewItem([entry.Name, Metrics.CpuTime(entry.CpuTicks),
                current?.IoError == null ? Metrics.Megabytes(entry.NetworkBytes) : "—", "—", "—"])));
    }

    private bool Confirm(string question) => MessageBox.Show(this, question, "Task Manager", MessageBoxButtons.YesNo,
        MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) == DialogResult.Yes;

    private static void AttachMenu(ListView list, Func<IEnumerable<MenuEntry>?> build, Control owner)
    {
        void Show(Point screen) { if (build() is { } entries) NativeMenu.Show(owner, screen, entries); }
        list.MouseUp += (_, args) => { if (args.Button == MouseButtons.Right) Show(list.PointToScreen(args.Location)); };
        list.KeyUp += (_, args) =>
        {
            if (args.KeyCode != Keys.Apps && !(args.Shift && args.KeyCode == Keys.F10)) return;
            var bounds = list.SelectedIndices.Count > 0 ? list.GetItemRect(list.SelectedIndices[0]) : Rectangle.Empty;
            Show(list.PointToScreen(new Point(bounds.Left + 40, bounds.Bottom)));
        };
    }

    private void AddManagementMenus()
    {
        AttachMenu(startup, () =>
        {
            if (startup.SelectedItems.Count == 0 || startup.SelectedItems[0].Tag is not StartupEntry entry) return null;
            var enable = entry.Status == "Disabled";
            return
            [
                new(enable ? "Enable" : "Disable", async () => await ChangeStartup(entry, enable), Default: true),
                new("Open file location", () => { if (entry.ExecutablePath is { } path) TryAction(() => Launch("explorer.exe", "/select,", path)); },
                    Enabled: () => entry.ExecutablePath != null),
                new("Search online", () => TryAction(() => Launch("https://www.bing.com/search?q=" + Uri.EscapeDataString(entry.Name)))),
                new("Properties", () => { if (entry.ExecutablePath is { } path) TryAction(() => ShowProperties(path)); }, Enabled: () => entry.ExecutablePath != null)
            ];
        }, this);

        AttachMenu(services, () =>
        {
            if (services.SelectedItems.Count == 0 || services.SelectedItems[0].Tag is not ServiceEntry service) return null;
            async Task Run(string action)
            {
                try { await Task.Run(() => Management.Service(service.Name, action)); }
                catch (Exception error) when (error is Win32Exception or InvalidOperationException or UnauthorizedAccessException)
                { if (!IsDisposed) MessageBox.Show(this, error.Message, "Task Manager", MessageBoxButtons.OK, MessageBoxIcon.Information); }
                if (!IsDisposed && !inventoryLoading) await RefreshInventory(6);
            }
            var running = service.Status == "Running";
            return
            [
                new("Start", async () => await Run("Start"), Enabled: () => !running),
                new("Stop", async () => await Run("Stop"), Enabled: () => running),
                new("Restart", async () => await Run("Restart"), Enabled: () => running),
                MenuEntry.Separator,
                new("Open Services", () => TryAction(() => Launch("services.msc"))),
                new("Search online", () => TryAction(() => Launch("https://www.bing.com/search?q=" + Uri.EscapeDataString(service.Name)))),
                new("Go to details", () => { searchBox.Clear(); tabs.SelectedIndex = 5; details.SelectProcess(service.ProcessId); details.Focus(); }, Enabled: () => service.ProcessId > 0)
            ];
        }, this);

        AttachMenu(users, () =>
        {
            if (users.SelectedItems.FirstOrDefault()?.Tag is ProcessSample process)
                return [new("End task", EndSelected, Enabled: () => process.Id > 4 && process.Id != Environment.ProcessId),
                    new("Go to details", () => { searchBox.Clear(); tabs.SelectedIndex = 5; details.SelectProcess(process.Id); })];
            if (users.SelectedItems.Count == 0 || users.SelectedItems[0].Tag is not UserSession session) return null;
            return [new(expandedSessions.Contains(session.Id) ? "Collapse" : "Expand", ToggleUserProcesses),
                new("Disconnect", DisconnectSelectedUser),
                new("Sign off", async () =>
                {
                    if (!Confirm("Sign off " + session.Name + "? All their applications will close and unsaved work will be lost.")) return;
                    try { await Task.Run(() => Management.SignOff(session)); }
                    catch (Exception error) when (error is Win32Exception or InvalidOperationException)
                    { if (!IsDisposed) MessageBox.Show(this, error.Message, "Task Manager", MessageBoxButtons.OK, MessageBoxIcon.Information); }
                    if (!IsDisposed && !Disposing) RequestRefresh();
                })];
        }, this);
    }

    private void SetAffinity(ProcessSample process)
    {
        var masks = ProcessActions.Affinity(process);
        using var dialog = new Form { Text = "Processor affinity", Font = Font, ClientSize = new Size(Scale(300), Scale(330)), AutoScaleMode = AutoScaleMode.Dpi,
            FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent, MinimizeBox = false, MaximizeBox = false, ShowInTaskbar = false };
        var prompt = new Label { Text = "Which processors are allowed to run “" + process.Name + "”?", Dock = DockStyle.Top, Height = Scale(34), Padding = new Padding(Scale(10), Scale(10), Scale(10), 0) };
        var list = new CheckedListBox { Dock = DockStyle.Fill, CheckOnClick = true, IntegralHeight = false };
        var processors = new List<int>();
        list.Items.Add("<All Processors>", masks.Process == masks.System);
        for (var index = 0; index < 64; index++)
        {
            if ((masks.System & (1ul << index)) == 0) continue;
            processors.Add(index);
            list.Items.Add("CPU " + index, (masks.Process & (1ul << index)) != 0);
        }
        var updating = false;
        list.ItemCheck += (_, args) =>
        {
            if (updating) return;
            updating = true;
            if (args.Index == 0) for (var index = 1; index < list.Items.Count; index++) list.SetItemChecked(index, args.NewValue == CheckState.Checked);
            else list.SetItemChecked(0, Enumerable.Range(1, list.Items.Count - 1).All(index => index == args.Index ? args.NewValue == CheckState.Checked : list.GetItemChecked(index)));
            updating = false;
        };
        var host = new Panel { Dock = DockStyle.Fill, Padding = new Padding(Scale(10), 0, Scale(10), 0) };
        host.Controls.Add(list);
        var footer = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = Scale(42), FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(Scale(6)) };
        var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, Size = new Size(Scale(75), Scale(23)), UseVisualStyleBackColor = true };
        var okay = new Button { Text = "OK", DialogResult = DialogResult.OK, Size = new Size(Scale(75), Scale(23)), UseVisualStyleBackColor = true };
        footer.Controls.AddRange([cancel, okay]);
        dialog.Controls.Add(host);
        dialog.Controls.Add(prompt);
        dialog.Controls.Add(footer);
        dialog.AcceptButton = okay;
        dialog.CancelButton = cancel;
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        ulong mask = 0;
        foreach (int selected in list.CheckedIndices) if (selected > 0) mask |= 1ul << processors[selected - 1];
        ProcessActions.Affinity(process, mask);
    }
}
