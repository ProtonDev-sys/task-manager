using System.Runtime.InteropServices;
using TaskManager.UI;

namespace TaskManager;

// Renders the real window off-screen (never activated, not in the taskbar) and saves each tab with PrintWindow.
internal static class Screenshots
{
    public static int Run(string[] arguments)
    {
        var directory = Option(arguments, "--output", "artifacts/screens");
        var wait = Math.Clamp(int.Parse(Option(arguments, "--wait", "6")), 1, 120);
        var width = int.Parse(Option(arguments, "--width", "1006"));
        var height = int.Parse(Option(arguments, "--height", "821"));
        var tabs = Option(arguments, "--tabs", "0,1,2,3,4,5,6").Split(',').Select(int.Parse).ToArray();
        var compact = arguments.Contains("--compact", StringComparer.Ordinal);
        var allResources = arguments.Contains("--resources", StringComparer.Ordinal);
        var group = arguments.Contains("--group", StringComparer.Ordinal);
        var expand = arguments.Contains("--expand", StringComparer.Ordinal);
        Directory.CreateDirectory(directory);
        var area = Screen.PrimaryScreen?.WorkingArea ?? SystemInformation.VirtualScreen;
        // On screen (DWM does not render off-screen windows) but invisible: 1% opacity, click-through, no taskbar/Alt+Tab entry,
        // bottom of the z-order and never activated.
        using var form = new MainForm(new AppSettings { Width = width, Height = height, UpdateInterval = 1000, GroupByType = group }, true)
        { ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(area.Left, area.Top), Opacity = 0.01 };
        form.HandleCreated += (_, _) =>
        {
            SetWindowLongPtrW(form.Handle, -20, GetWindowLongPtrW(form.Handle, -20) | 0x20 | 0x80 | 0x80000);
            SetWindowPos(form.Handle, 1, 0, 0, 0, 0, 0x13);
        };
        using var timer = new System.Windows.Forms.Timer { Interval = wait * 1000 };
        var failure = 0;
        timer.Tick += async (_, _) =>
        {
            timer.Stop();
            try
            {
                if (expand) form.ProcessView.ExpandAll(true);
                foreach (var tab in tabs)
                {
                    form.SelectedTab = tab;
                    await Task.Delay(tab is 3 or 6 ? 1500 : 400);
                    if (tab == 1 && allResources)
                        foreach (var key in form.Performance.ResourceKeys)
                        {
                            form.Performance.SelectedResource = key;
                            await Task.Delay(150);
                            Capture(form, Path.Combine(directory, $"tab1-{Sanitize(key)}.png"));
                        }
                    else Capture(form, Path.Combine(directory, $"tab{tab}.png"));
                }
                if (compact)
                {
                    form.ToggleDetailsForTest();
                    await Task.Delay(400);
                    Capture(form, Path.Combine(directory, "compact.png"));
                }
            }
            catch (Exception error)
            {
                File.WriteAllText(Path.Combine(directory, "error.txt"), error.ToString());
                failure = 1;
            }
            form.Close();
        };
        form.Shown += (_, _) => timer.Start();
        Application.Run(form);
        form.SamplingTask?.Wait(TimeSpan.FromSeconds(5));
        return failure;
    }

    private static string Sanitize(string key) => string.Concat(key.Select(character => char.IsLetterOrDigit(character) ? character : '-'));

    private static void Capture(Form form, string path)
    {
        form.Update();
        GetWindowRect(form.Handle, out var window);
        DwmGetWindowAttribute(form.Handle, 9, out var frame, 16);
        using var full = new Bitmap(window.Right - window.Left, window.Bottom - window.Top);
        using (var graphics = Graphics.FromImage(full))
        {
            var dc = graphics.GetHdc();
            try { PrintWindow(form.Handle, dc, 2); }
            finally { graphics.ReleaseHdc(dc); }
        }
        var crop = new Rectangle(frame.Left - window.Left, frame.Top - window.Top, frame.Right - frame.Left, frame.Bottom - frame.Top);
        using var image = full.Clone(crop, full.PixelFormat);
        image.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }

    private static string Option(string[] arguments, string name, string fallback)
    {
        var index = Array.IndexOf(arguments, name);
        return index >= 0 && index + 1 < arguments.Length ? arguments[index + 1] : fallback;
    }

    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint window, out Rect rectangle);
    [DllImport("user32.dll")] private static extern bool PrintWindow(nint window, nint dc, uint flags);
    [DllImport("user32.dll")] private static extern nint GetWindowLongPtrW(nint window, int index);
    [DllImport("user32.dll")] private static extern nint SetWindowLongPtrW(nint window, int index, nint value);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(nint window, int attribute, out Rect value, int size);
}
