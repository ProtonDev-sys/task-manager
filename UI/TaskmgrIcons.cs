using System.Runtime.InteropServices;

namespace TaskManager.UI;

// Icons borrowed at runtime from the installed Taskmgr.exe so generic rows look the same as Windows' own.
internal static class TaskmgrIcons
{
    private static readonly Dictionary<int, Icon?> cache = new();
    public static Icon Process => Load(1) ?? SystemIcons.Application;
    public static Icon ServiceHost => Load(3) ?? Process;
    public static Icon Service => Load(2) ?? Process;

    private static Icon? Load(int index)
    {
        if (cache.TryGetValue(index, out var icon)) return icon;
        var small = new nint[1];
        try
        {
            var path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "Taskmgr.exe");
            if (ExtractIconExW(path, index, null, small, 1) > 0 && small[0] != 0)
            {
                using var borrowed = Icon.FromHandle(small[0]);
                icon = (Icon)borrowed.Clone();
            }
        }
        finally { if (small[0] != 0) DestroyIcon(small[0]); }
        return cache[index] = icon;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern uint ExtractIconExW(string file, int index, nint[]? large, nint[]? small, uint count);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(nint icon);
}
