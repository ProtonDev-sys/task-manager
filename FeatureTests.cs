using System.Diagnostics;
using TaskManager.Monitoring;
using TaskManager.UI;

namespace TaskManager;

internal static class FeatureTests
{
    public static void Run(SystemSample system, List<string> checks)
    {
        var bytes = new byte[86];
        BitConverter.GetBytes(78u).CopyTo(bytes, 4);
        bytes[8] = 17; bytes[9] = 34;
        BitConverter.GetBytes((ushort)16384).CopyTo(bytes, 20);
        bytes[22] = 9;
        BitConverter.GetBytes((ushort)6000).CopyTo(bytes, 40);
        bytes[44] = 17; bytes[45] = 34;
        bytes[80] = 127; bytes[81] = 4;
        var memory = HardwareInventory.ParseMemory(bytes, 32ul * 1073741824);
        Assert(memory.Slots == 2 && memory.UsedSlots == 1 && memory.Speed == 6000 && memory.FormFactor == "DIMM", "bounded SMBIOS memory decoding", checks);
        Assert(HardwareInventory.ParseMemory([1, 2], 0).Slots == 0, "truncated SMBIOS rejected", checks);
        var parent = new ProcessSample(200, 0, "app.exe", 100, 2, 1000, 0, 1, 0, 1, 0, 0) { CpuTime = 10000 };
        var child = parent with { Id = 201, ParentId = 200, Created = 110, Cpu = 3, WorkingSet = 2000 };
        var metadata = new Dictionary<(int, long), ProcessMetadata> { [(200, 100)] = new("App", "C:\\app.exe", "", null, true) };
        var rows = new ProcessRows();
        var collapsed = rows.Build([parent, child], metadata, (left, right) => left.Id.CompareTo(right.Id), process => process.Name);
        Assert(collapsed.Length == 1 && collapsed[0].Sample.Cpu == 5 && collapsed[0].Sample.WorkingSet == 3000, "process tree resource aggregation", checks);
        rows.Expanded.Add((200, 100));
        Assert(rows.Build([parent, child], metadata, (left, right) => left.Id.CompareTo(right.Id), process => process.Name).Length == 3, "process tree expansion lists every member", checks);
        var shell = parent with { Id = 150, ParentId = 0, Name = "explorer.exe", Created = 50 };
        metadata[(150, 50)] = new("Windows Explorer", "C:\\Windows\\explorer.exe", "", null, true);
        var launched = parent with { ParentId = 150 };
        var shellRows = rows.Build([shell, launched, child], metadata, (left, right) => left.Id.CompareTo(right.Id), process => process.Name);
        Assert(shellRows.Any(row => row.Sample.Id == 150 && row.Sample.Cpu == shell.Cpu) && shellRows.Any(row => row.Sample.Id == 200 && row.Sample.Cpu == 5), "separate applications are not grouped under Explorer", checks);
        Assert(rows.Build([shell, child with { ParentId = 150 }], metadata, (left, right) => left.Id.CompareTo(right.Id), process => process.Name).Length == 2,
            "unidentified Explorer children remain separate", checks);
        Assert(rows.Build([parent, child with { Created = 90 }], metadata, (left, right) => left.Id.CompareTo(right.Id), process => process.Name).Length == 2, "PID reuse does not create a false process tree", checks);
        rows.GroupByType = true;
        Assert(rows.Build([parent, child], metadata, (left, right) => left.Id.CompareTo(right.Id), process => process.Name).Any(row => row.Kind == RowKind.Heading && row.Name.StartsWith("Apps")), "process category headings", checks);
        var history = new AppHistory(false);
        history.Update(system with { Processes = [parent], Metadata = metadata });
        history.Update(system with { Processes = [parent with { CpuTime = 30000 }], Metadata = metadata });
        history.Update(system with { Processes = [parent with { CpuTime = 30000 }], Metadata = metadata });
        Assert(history.Entries(true).Single().CpuTicks == 20000, "app history records deltas without double counting", checks);
        history.Clear();
        Assert(history.Entries(true).Length == 0, "in-memory history reset", checks);
        using var inventory = new InventoryView();
        inventory.CreateControl();
        inventory.ReplaceRows([new ListViewItem("One") { Tag = 1 }, new ListViewItem("Two") { Tag = 2 }]);
        inventory.SelectedIndices.Add(1);
        inventory.ReplaceRows([new ListViewItem("Two") { Tag = 2 }, new ListViewItem("One") { Tag = 1 }]);
        Assert(inventory.SelectedItems.Single().Text == "Two", "virtual inventory preserves selection", checks);
        var selectionChanges = 0;
        inventory.SelectedIndexChanged += (_, _) => selectionChanges++;
        inventory.ReplaceRows([new ListViewItem("Two") { Tag = 2 }, new ListViewItem("One") { Tag = 1 }]);
        Assert(selectionChanges == 0, "unchanged inventory selection avoids native event churn", checks);
        inventory.Filter = "ONE";
        Assert(inventory.VirtualListSize == 1 && inventory.Items[0].Text == "One", "inventory search is case insensitive", checks);
        inventory.Filter = "not-present";
        Assert(inventory.VirtualListSize == 0 && inventory.SelectedItems.Count == 0, "empty filter results clear unsafe selection", checks);
        inventory.Filter = "";
        Assert(inventory.VirtualListSize == 2, "clearing search restores inventory", checks);
        var session = new UserSession(1, "Example", "Active");
        inventory.ReplaceRows([new ListViewItem("▸ Example") { Tag = session }]);
        inventory.SelectedIndices.Add(0);
        inventory.ReplaceRows([new ListViewItem("▾ Example") { Tag = session }]);
        Assert(inventory.SelectedItems.Single().Text == "▾ Example", "session selection survives expanding caption", checks);
        inventory.Columns.Add("Name");
        inventory.Columns.Add("PID");
        inventory.ReplaceRows([new ListViewItem(["Ten", "10"]), new ListViewItem(["Two", "2"])]);
        inventory.SortByColumn(1);
        Assert(inventory.Items[0].Text == "Two", "inventory numeric ascending sort", checks);
        inventory.ReplaceRows([new ListViewItem(["Ten", "10"]), new ListViewItem(["Two", "2"])]);
        Assert(inventory.Items[0].Text == "Two", "inventory sort survives refresh", checks);
        inventory.SortByColumn(1);
        Assert(inventory.Items[0].Text == "Ten", "inventory descending sort", checks);
        using var processList = new ProcessList(false);
        processList.CreateControl();
        processList.UpdateSample(system with { Processes = [parent, child], Metadata = metadata });
        processList.Filter = "200";
        Assert(processList.VisibleRowCount == 1, "process search matches PID", checks);
        processList.Filter = "does-not-exist";
        Assert(processList.VirtualListSize == 0 && processList.SelectedProcess == null, "process search clears hidden selection", checks);
        processList.Filter = "";
        Assert(processList.VisibleRowCount == 1, "clearing process search restores grouping", checks);
        Assert(SearchFilter.Matches(parent, new("Editor", null, "Example Publisher", null, true), "PUBLISHER"), "process search matches publisher", checks);
        processList.Columns[1].Width = 0;
        processList.Columns[10].DisplayIndex = 2;
        var preferences = processList.CaptureColumns();
        using var restored = new ProcessList(false);
        restored.CreateControl();
        restored.RestoreColumns(preferences);
        Assert(restored.Columns[1].Width == 0 && restored.Columns[10].DisplayIndex == 2, "process column visibility and order round trip", checks);
        restored.RestoreColumns(new ColumnSettings { Widths = [100], Order = [999], SortColumn = 999 });
        Assert(restored.Columns[0].Width > 0, "invalid column preferences leave required name column visible", checks);
        var idle = parent with { Id = 0, ParentId = 0, Name = "System Idle Process" };
        Assert(rows.Build([idle, parent], metadata, (left, right) => left.Id.CompareTo(right.Id), process => process.Name).All(row => row.Sample.Id != 0), "Processes excludes the idle counter", checks);
    }

    public static void Child(Process child, ProcessSample sample, List<string> checks)
    {
        try { ProcessActions.Priority(sample with { Created = sample.Created + 1 }, ProcessPriorityClass.BelowNormal); throw new InvalidOperationException("Stale identity was accepted."); }
        catch (InvalidOperationException error) when (error.Message.Contains("exited", StringComparison.Ordinal)) { checks.Add("process action rejects stale identity"); }
        var original = child.PriorityClass;
        try
        {
            ProcessActions.Priority(sample, ProcessPriorityClass.BelowNormal);
            child.Refresh();
            Assert(child.PriorityClass == ProcessPriorityClass.BelowNormal, "priority change on owned test child", checks);
        }
        finally { ProcessActions.Priority(sample, original); }
        var originalEfficiency = ProcessActions.EfficiencyMode(sample);
        if (originalEfficiency.HasValue)
        {
            try
            {
                ProcessActions.EfficiencyMode(sample, true);
                child.Refresh();
                Assert(ProcessActions.EfficiencyMode(sample) == true && child.PriorityClass == ProcessPriorityClass.Idle, "efficiency mode on owned child", checks);
                ProcessActions.EfficiencyMode(sample, false);
                Assert(ProcessActions.EfficiencyMode(sample) == false, "efficiency mode disables on owned child", checks);
            }
            finally
            {
                ProcessActions.EfficiencyMode(sample, originalEfficiency.Value);
                ProcessActions.Priority(sample, original);
            }
        }
        var masks = ProcessActions.Affinity(sample);
        Assert(WaitChains.Analyze(sample).Contains("Thread ", StringComparison.Ordinal), "owned child wait-chain analysis", checks);
        Assert(masks.Process != 0 && (masks.Process & ~masks.System) == 0, "owned child processor affinity", checks);
        ProcessActions.Affinity(sample, masks.Process);
        try { ProcessActions.Affinity(sample, 0); throw new InvalidOperationException("Empty affinity was accepted."); }
        catch (InvalidOperationException error) when (error.Message.Contains("at least one", StringComparison.Ordinal)) { checks.Add("empty affinity refused"); }
        var dump = Path.Combine(Path.GetTempPath(), "taskmanager-owned-child-" + Guid.NewGuid().ToString("N") + ".dmp");
        try
        {
            try { ProcessActions.Dump(sample with { Created = sample.Created + 1 }, dump); throw new InvalidOperationException("Stale dump identity was accepted."); }
            catch (InvalidOperationException error) when (error.Message.Contains("exited", StringComparison.Ordinal))
            { Assert(!File.Exists(dump), "stale dump identity does not create a file", checks); }
            ProcessActions.Dump(sample, dump);
            using var stream = File.OpenRead(dump);
            Span<byte> signature = stackalloc byte[4];
            stream.ReadExactly(signature);
            Assert(signature.SequenceEqual("MDMP"u8), "owned child dump has valid minidump signature", checks);
        }
        finally { if (File.Exists(dump)) File.Delete(dump); }
    }

    private static void Assert(bool condition, string name, List<string> checks)
    {
        if (!condition) throw new InvalidOperationException("Failed: " + name);
        checks.Add(name);
    }
}
