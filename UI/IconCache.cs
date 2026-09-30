using System.Runtime.InteropServices;

namespace TaskManager.UI;

// One bounded, shared small-icon list for the native list views (Details, Startup, Fewer details).
internal static class IconCache
{
    private const int Limit = 1024;
    private static readonly Dictionary<Icon, int> icons = new(ReferenceEqualityComparer.Instance);
    private static readonly Dictionary<string, int> paths = new(StringComparer.OrdinalIgnoreCase);
    private static ImageList? images;

    public static ImageList Images
    {
        get
        {
            if (images != null) return images;
            images = new ImageList { ColorDepth = ColorDepth.Depth32Bit, ImageSize = new Size(16, 16) };
            images.Images.Add(SystemIcons.Application);
            return images;
        }
    }

    public static int Index(Icon? icon)
    {
        if (icon == null) return 0;
        if (icons.TryGetValue(icon, out var index)) return index;
        if (Images.Images.Count >= Limit) return 0;
        // Process icons belong to the sampler's cache and are disposed when it shuts down.
        try { Images.Images.Add(icon); }
        catch (Exception error) when (error is ObjectDisposedException or ArgumentException) { return 0; }
        return icons[icon] = Images.Images.Count - 1;
    }

    public static int Index(string? path)
    {
        if (string.IsNullOrEmpty(path)) return 0;
        if (paths.TryGetValue(path, out var index)) return index;
        if (Images.Images.Count >= Limit || paths.Count >= Limit) return 0;
        var information = new ShellFileInfo();
        index = 0;
        if (SHGetFileInfoW(path, 0, ref information, (uint)Marshal.SizeOf<ShellFileInfo>(), 0x101) != 0 && information.Icon != 0)
        {
            try
            {
                using var icon = Icon.FromHandle(information.Icon);
                Images.Images.Add(icon);
                index = Images.Images.Count - 1;
            }
            finally { DestroyIcon(information.Icon); }
        }
        return paths[path] = index;
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

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern nint SHGetFileInfoW(string path, uint attributes, ref ShellFileInfo information, uint size, uint flags);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(nint icon);
}
