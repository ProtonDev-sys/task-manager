using TaskManager.Monitoring;

namespace TaskManager.UI;

internal sealed class InventoryView : ListView
{
    private ListViewItem[] rows = [];
    private ListViewItem[] source = [];
    private string filter = "";
    private int sortColumn = -1;
    private bool descending;
    internal string Filter
    {
        get => filter;
        set
        {
            var next = value.Trim();
            if (filter == next) return;
            filter = next;
            ApplyRows();
        }
    }
    internal int VisibleRowCount => rows.Length;
    public new IReadOnlyList<ListViewItem> SelectedItems => SelectedIndices.Cast<int>().Where(index => index >= 0 && index < rows.Length).Select(index => rows[index]).ToArray();

    public InventoryView()
    {
        Dock = DockStyle.Fill;
        View = View.Details;
        FullRowSelect = true;
        MultiSelect = false;
        HideSelection = false;
        BorderStyle = BorderStyle.None;
        Font = new Font("Segoe UI", 9);
        VirtualMode = true;
        DoubleBuffered = true;
        RetrieveVirtualItem += (_, args) => args.Item = args.ItemIndex >= 0 && args.ItemIndex < rows.Length ? rows[args.ItemIndex] : new ListViewItem("");
        ColumnClick += (_, args) =>
        {
            SortByColumn(args.Column);
        };
        KeyDown += (_, args) =>
        {
            if (!args.Control || args.KeyCode != Keys.C || SelectedItems.FirstOrDefault() is not { } selected) return;
            Clipboard.SetText(string.Join('\t', selected.SubItems.Cast<ListViewItem.ListViewSubItem>().Select(cell => cell.Text)));
            args.SuppressKeyPress = true;
        };
    }

    internal void SortByColumn(int column)
    {
        if (column < 0 || column >= Columns.Count) return;
        descending = sortColumn == column && !descending;
        sortColumn = column;
        ApplyRows();
    }

    protected override void OnHandleCreated(EventArgs args)
    {
        base.OnHandleCreated(args);
        NativeMethods.SetWindowTheme(Handle, "Explorer", null);
    }

    public void ReplaceRows(IEnumerable<ListViewItem> items)
    {
        source = items.ToArray();
        ApplyRows();
    }

    private void ApplyRows()
    {
        var selected = SelectedItems.FirstOrDefault();
        var identity = selected?.Tag;
        var text = selected?.Text;
        var top = TopItem?.Index ?? 0;
        rows = filter.Length == 0 ? sortColumn < 0 ? source : (ListViewItem[])source.Clone() : source.Where(item =>
            item.SubItems.Cast<ListViewItem.ListViewSubItem>().Any(cell => SearchFilter.Contains(cell.Text, filter))).ToArray();
        if (sortColumn >= 0) Array.Sort(rows, CompareRows);
        BeginUpdate();
        if (VirtualListSize != rows.Length) VirtualListSize = rows.Length;
        if (text != null)
        {
            var index = Array.FindIndex(rows, item => SameIdentity(identity, item.Tag) && (identity != null || item.Text == text));
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

    private int CompareRows(ListViewItem left, ListViewItem right)
    {
        var leftText = left.SubItems.Count > sortColumn ? left.SubItems[sortColumn].Text : "";
        var rightText = right.SubItems.Count > sortColumn ? right.SubItems[sortColumn].Text : "";
        var comparison = double.TryParse(leftText, out var leftNumber) && double.TryParse(rightText, out var rightNumber)
            ? leftNumber.CompareTo(rightNumber) : StringComparer.CurrentCultureIgnoreCase.Compare(leftText, rightText);
        if (comparison == 0) comparison = StringComparer.CurrentCultureIgnoreCase.Compare(left.Text, right.Text);
        return descending ? -Math.Sign(comparison) : Math.Sign(comparison);
    }

    // The Explorer list theme draws faint column dividers below the last row; Task Manager's lists are plain white there.
    protected override void WndProc(ref Message message)
    {
        base.WndProc(ref message);
        if (message.Msg != 0x000F || !IsHandleCreated || View != View.Details) return;
        var top = 0;
        if (HeaderStyle != ColumnHeaderStyle.None && GetWindowRect(SendMessageW(Handle, 0x101F, 0, 0), out var header)) top = header.Bottom - header.Top;
        if (rows.Length > 0) top = Math.Max(top, GetItemRect(rows.Length - 1).Bottom);
        if (top >= ClientSize.Height) return;
        using var graphics = Graphics.FromHwnd(Handle);
        graphics.FillRectangle(SystemBrushes.Window, 0, top, ClientSize.Width, ClientSize.Height - top);
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern bool GetWindowRect(nint window, out Rect rectangle);
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern nint SendMessageW(nint window, uint message, nint wParam, nint lParam);

    private static bool SameIdentity(object? left, object? right) => (left, right) switch
    {
        (ProcessSample before, ProcessSample after) => before.Id == after.Id && before.Created == after.Created,
        (ServiceEntry before, ServiceEntry after) => before.Name == after.Name,
        (UserSession before, UserSession after) => before.Id == after.Id,
        (StartupEntry before, StartupEntry after) => before.Name == after.Name && before.Location == after.Location,
        _ => left == null || left.Equals(right)
    };
}
