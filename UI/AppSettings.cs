using System.Text.Json;

namespace TaskManager.UI;

internal sealed class AppSettings
{
    public int Width { get; set; } = 1000;
    public int Height { get; set; } = 800;
    public int UpdateInterval { get; set; } = 1000;
    public bool AlwaysOnTop { get; set; }
    public int SelectedTab { get; set; }
    public bool GroupByType { get; set; }
    public bool ShowAllHistory { get; set; }
    public bool MinimizeOnUse { get; set; }
    public bool HideWhenMinimized { get; set; }
    public ColumnSettings? ProcessColumns { get; set; }
    public ColumnSettings? DetailColumns { get; set; }
    private static string SettingsPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TaskManagerClone", "settings.json");

    public static AppSettings Load()
    {
        try
        {
            var settings = File.Exists(SettingsPath) && new FileInfo(SettingsPath).Length <= 65536 ? JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(SettingsPath)) : null;
            if (settings == null) return new();
            settings.Width = Math.Clamp(settings.Width, 700, 2400);
            settings.Height = Math.Clamp(settings.Height, 450, 1600);
            settings.SelectedTab = Math.Clamp(settings.SelectedTab, 0, 6);
            if (settings.UpdateInterval is not (0 or 500 or 1000 or 4000)) settings.UpdateInterval = 1000;
            return settings;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return new(); }
    }

    public void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)!);
        var temporary = SettingsPath + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(this));
        File.Move(temporary, SettingsPath, true);
    }
}

internal sealed class ColumnSettings
{
    public int[] Widths { get; set; } = [];
    public int[] Order { get; set; } = [];
    public int SortColumn { get; set; }
    public bool Descending { get; set; }
    public bool MemoryPercent { get; set; }
    public bool NetworkPercent { get; set; }
}
