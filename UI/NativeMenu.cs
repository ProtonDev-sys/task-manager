using System.Runtime.InteropServices;

namespace TaskManager.UI;

// Native Win32 menus, so the menu bar and context menus look exactly like the system's (as Task Manager's do).
internal sealed record MenuEntry(string Text, Action? Action = null, Func<bool>? Checked = null, Func<bool>? Enabled = null,
    MenuEntry[]? Children = null, bool Radio = false, bool Default = false)
{
    public static readonly MenuEntry Separator = new("-");
}

internal static class NativeMenu
{
    public static nint Build(IEnumerable<MenuEntry> entries, List<MenuEntry> commands, bool bar = false)
    {
        var menu = bar ? CreateMenu() : CreatePopupMenu();
        foreach (var entry in entries)
        {
            if (entry.Text == "-") { AppendMenuW(menu, 0x800, 0, null); continue; }
            if (entry.Children != null)
            {
                AppendMenuW(menu, 0x10 | State(entry), Build(entry.Children, commands), entry.Text);
                continue;
            }
            commands.Add(entry);
            AppendMenuW(menu, State(entry), commands.Count, entry.Text);
            if (entry.Radio && entry.Checked?.Invoke() == true)
            {
                var info = new MenuItemInfo { Size = Marshal.SizeOf<MenuItemInfo>(), Mask = 0x100 | 0x1, Type = 0x200, State = 0x8 };
                SetMenuItemInfoW(menu, (uint)commands.Count, false, ref info);
            }
            if (entry.Default) SetMenuDefaultItem(menu, (uint)commands.Count, 0);
        }
        return menu;
    }

    private static uint State(MenuEntry entry) =>
        (entry.Checked?.Invoke() == true ? 0x8u : 0) | (entry.Enabled?.Invoke() == false ? 0x3u : 0);

    // Refreshes check and enabled states of an existing menu before it opens.
    public static void Refresh(nint menu, List<MenuEntry> commands)
    {
        var count = GetMenuItemCount(menu);
        for (var index = 0; index < count; index++)
        {
            var id = GetMenuItemID(menu, index);
            if (id == uint.MaxValue || id == 0 || id > commands.Count) continue;
            var entry = commands[(int)id - 1];
            var info = new MenuItemInfo { Size = Marshal.SizeOf<MenuItemInfo>(), Mask = 0x1 | 0x100, Type = entry.Radio ? 0x200u : 0, State = State(entry) };
            SetMenuItemInfoW(menu, id, false, ref info);
        }
    }

    public static void Show(Control owner, Point screen, IEnumerable<MenuEntry> entries)
    {
        var commands = new List<MenuEntry>();
        var menu = Build(entries, commands);
        try
        {
            var chosen = TrackPopupMenuEx(menu, 0x100 | 0x2, screen.X, screen.Y, owner.Handle, 0);
            if (chosen > 0 && chosen <= commands.Count) commands[chosen - 1].Action?.Invoke();
        }
        finally { DestroyMenu(menu); }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MenuItemInfo
    {
        public int Size; public uint Mask; public uint Type; public uint State; public uint Id; public nint SubMenu;
        public nint Checked; public nint Unchecked; public nint Data; public nint TypeData; public uint Length; public nint Item;
    }

    [DllImport("user32.dll")] public static extern nint CreateMenu();
    [DllImport("user32.dll")] private static extern nint CreatePopupMenu();
    [DllImport("user32.dll")] public static extern bool DestroyMenu(nint menu);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool AppendMenuW(nint menu, uint flags, nint id, string? text);
    [DllImport("user32.dll")] private static extern int TrackPopupMenuEx(nint menu, uint flags, int x, int y, nint window, nint parameters);
    [DllImport("user32.dll")] private static extern bool SetMenuDefaultItem(nint menu, uint item, uint byPosition);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool SetMenuItemInfoW(nint menu, uint item, bool byPosition, ref MenuItemInfo info);
    [DllImport("user32.dll")] private static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll")] private static extern uint GetMenuItemID(nint menu, int position);
    [DllImport("user32.dll")] public static extern bool SetMenu(nint window, nint menu);
}
