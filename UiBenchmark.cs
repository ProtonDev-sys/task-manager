using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using TaskManager.UI;

namespace TaskManager;

internal static class UiBenchmark
{
    public static int Run(string[] arguments)
    {
        var seconds = Read(arguments, "--seconds", 30);
        seconds = Math.Clamp(seconds, 5, 600);
        var output = "artifacts/ui-benchmark.json";
        var live = arguments.Contains("--live", StringComparer.Ordinal);
        var fixedTab = Read(arguments, "--tab", -1);
        var outputIndex = Array.IndexOf(arguments, "--output");
        if (outputIndex >= 0 && outputIndex + 1 < arguments.Length) output = arguments[outputIndex + 1];
        var launch = Stopwatch.StartNew();
        var interval = Read(arguments, "--interval", 1000);
        if (interval is not (0 or 500 or 1000 or 4000)) throw new ArgumentException("Supported intervals are 0, 500, 1000 and 4000.");
        var exercise = arguments.Contains("--exercise", StringComparer.Ordinal);
        var minimized = arguments.Contains("--minimized", StringComparer.Ordinal);
        using var form = new MainForm(new AppSettings { UpdateInterval = interval }, true) { ShowInTaskbar = live, Opacity = live ? 1 : 0 };
        using var timer = new System.Windows.Forms.Timer { Interval = 100 };
        using var process = Process.GetCurrentProcess();
        var clock = Stopwatch.StartNew();
        var warmup = 20000;
        var measured = false;
        var baselineCpu = TimeSpan.Zero;
        long baselineAllocated = 0;
        var baselineHandles = 0;
        var baselineGdi = 0;
        var baselineUser = 0;
        int[] collections = [];
        var renders = 0;
        var paintMilliseconds = new List<double>();
        var sampleMilliseconds = new List<double>();
        var gpuMilliseconds = new List<double>();
        var metadataMilliseconds = new List<double>();
        var deliveryMilliseconds = new List<double>();
        var updateMilliseconds = new List<double>();
        var navigationMilliseconds = new List<double>();
        var searchMilliseconds = new List<double>();
        var heartbeatMilliseconds = new List<double>();
        var inputMilliseconds = new List<double>();
        var stageMilliseconds = new Dictionary<string, List<double>>
        {
            ["processInventory"] = [], ["devices"] = [], ["sessionsAndServices"] = [], ["attribution"] = []
        };
        var tabRenders = Enumerable.Range(0, 7).ToDictionary(index => index, _ => new List<double>());
        var previousTick = Stopwatch.GetTimestamp();
        var lastProbe = previousTick;
        double shownMilliseconds = 0;
        double firstSampleMilliseconds = 0;
        DateTime lastSample = default;
        var tabOrder = new[] { 0, 1, 5, 4, 2, 3, 6 };
        using var frame = new Bitmap(form.Width, form.Height);
        var failure = "";
        timer.Tick += (_, _) =>
        {
            try
            {
                var tick = Stopwatch.GetTimestamp();
                if (measured) heartbeatMilliseconds.Add(Math.Max(0, Stopwatch.GetElapsedTime(previousTick, tick).TotalMilliseconds - timer.Interval));
                previousTick = tick;
                if (measured && Stopwatch.GetElapsedTime(lastProbe, tick).TotalSeconds >= 1)
                {
                    lastProbe = tick;
                    _ = Task.Run(() =>
                    {
                        var posted = Stopwatch.GetTimestamp();
                        try { form.BeginInvoke(() => inputMilliseconds.Add(Stopwatch.GetElapsedTime(posted).TotalMilliseconds)); }
                        catch (InvalidOperationException) { }
                    });
                }
                if (form.SamplingError != null) throw new InvalidOperationException(form.SamplingError);
                if (!measured && clock.ElapsedMilliseconds >= warmup)
                {
                    process.Refresh();
                    baselineCpu = process.TotalProcessorTime;
                    baselineAllocated = GC.GetTotalAllocatedBytes();
                    baselineHandles = process.HandleCount;
                    baselineGdi = GetGuiResources(process.Handle, 0);
                    baselineUser = GetGuiResources(process.Handle, 1);
                    collections = Enumerable.Range(0, 3).Select(GC.CollectionCount).ToArray();
                    measured = true;
                    if (minimized) form.WindowState = FormWindowState.Minimized;
                }
                if (form.CurrentSample is { } sample && sample.Timestamp != lastSample)
                {
                    lastSample = sample.Timestamp;
                    if (firstSampleMilliseconds == 0) firstSampleMilliseconds = launch.Elapsed.TotalMilliseconds;
                    if (measured) { sampleMilliseconds.Add(sample.SampleMilliseconds); gpuMilliseconds.Add(sample.GpuMilliseconds); metadataMilliseconds.Add(sample.MetadataMilliseconds); }
                    if (measured)
                    {
                        deliveryMilliseconds.Add(form.LastDeliveryMilliseconds);
                        updateMilliseconds.Add(form.LastUiUpdateMilliseconds);
                        stageMilliseconds["processInventory"].Add(sample.ProcessMilliseconds);
                        stageMilliseconds["devices"].Add(sample.DeviceMilliseconds);
                        stageMilliseconds["sessionsAndServices"].Add(sample.InventoryMilliseconds);
                        stageMilliseconds["attribution"].Add(sample.AttributionMilliseconds);
                    }
                    var navigation = Stopwatch.GetTimestamp();
                    form.SelectedTab = fixedTab is >= 0 and <= 6 ? fixedTab : tabOrder[renders % tabOrder.Length];
                    if (measured) navigationMilliseconds.Add(Stopwatch.GetElapsedTime(navigation).TotalMilliseconds);
                    if (exercise)
                    {
                        var search = Stopwatch.GetTimestamp();
                        form.SearchText = sample.Processes.FirstOrDefault(process => process.Id > 4)?.Id.ToString() ?? "task";
                        form.SearchText = "";
                        if (measured) searchMilliseconds.Add(Stopwatch.GetElapsedTime(search).TotalMilliseconds);
                        if (form.SelectedTab == 1 && form.Performance.ResourceKeys.Count > 0)
                            form.Performance.SelectedResource = form.Performance.ResourceKeys[renders % form.Performance.ResourceKeys.Count];
                    }
                    var paint = Stopwatch.GetTimestamp();
                    if (!live) form.DrawToBitmap(frame, new Rectangle(Point.Empty, frame.Size));
                    if (measured && !live)
                    {
                        var duration = Stopwatch.GetElapsedTime(paint).TotalMilliseconds;
                        paintMilliseconds.Add(duration);
                        tabRenders[form.SelectedTab].Add(duration);
                    }
                    renders++;
                }
                if (clock.ElapsedMilliseconds < warmup + seconds * 1000) return;
                if (form.CurrentSample == null || (!minimized && interval != 0 && sampleMilliseconds.Count == 0))
                    throw new InvalidOperationException("No system samples reached the UI during the measurement.");
                timer.Stop();
                process.Refresh();
                var elapsed = (clock.ElapsedMilliseconds - warmup) / 1000d;
                var cpuMs = (process.TotalProcessorTime - baselineCpu).TotalMilliseconds;
                var ordered = paintMilliseconds.Order().ToArray();
                var result = new
                {
                    passed = true, durationSeconds = elapsed, warmupSeconds = warmup / 1000,
                    scope = live ? "Real visible window, sampling, metadata, ETW if permitted, native controls and normal paints. Desktop compositor excluded from own process CPU." : "Sampling, ETW if permitted, metadata, native controls and DrawToBitmap. Excludes desktop composition.",
                    fixedTab, live, interval, exercise, minimized, shownMilliseconds, firstSampleMilliseconds,
                    delivery = Summarize(deliveryMilliseconds), uiUpdate = Summarize(updateMilliseconds), navigation = Summarize(navigationMilliseconds),
                    search = Summarize(searchMilliseconds), messageLoopDelay = Summarize(heartbeatMilliseconds), inputQueue = Summarize(inputMilliseconds),
                    stages = stageMilliseconds.ToDictionary(entry => entry.Key, entry => Summarize(entry.Value)),
                    perTabRender = tabRenders.ToDictionary(entry => entry.Key, entry => Summarize(entry.Value)),
                    renderCount = ordered.Length, medianRenderMilliseconds = ordered.Length > 0 ? ordered[ordered.Length / 2] : 0,
                    p95RenderMilliseconds = ordered.Length > 0 ? ordered[(int)Math.Min(ordered.Length - 1, Math.Ceiling(ordered.Length * .95) - 1)] : 0,
                    cpuMilliseconds = cpuMs, cpuPercentOneCore = cpuMs / (elapsed * 10), cpuPercentMachine = cpuMs / (elapsed * 10 * Environment.ProcessorCount),
                    allocatedBytes = GC.GetTotalAllocatedBytes() - baselineAllocated, workingSetBytes = process.WorkingSet64, privateBytes = process.PrivateMemorySize64,
                    handleDelta = process.HandleCount - baselineHandles, gdiDelta = GetGuiResources(process.Handle, 0) - baselineGdi,
                    userDelta = GetGuiResources(process.Handle, 1) - baselineUser, collections = Enumerable.Range(0, 3).Select(index => GC.CollectionCount(index) - collections[index]).ToArray(),
                    processes = form.CurrentSample?.Processes.Length, gpus = form.CurrentSample?.Gpus.Select(gpu => new { gpu.Name, gpu.Usage, engineCount = gpu.Engines.Count }),
                    ioAttribution = form.CurrentSample?.IoError ?? "Available"
                    , averageSampleMilliseconds = sampleMilliseconds.DefaultIfEmpty().Average(), averageGpuMilliseconds = gpuMilliseconds.DefaultIfEmpty().Average(),
                    averageMetadataMilliseconds = metadataMilliseconds.DefaultIfEmpty().Average(), metadataCount = form.CurrentSample?.Metadata.Count
                };
                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
                File.WriteAllText(output, JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true }));
                form.Close();
            }
            catch (Exception error)
            {
                failure = error.ToString();
                timer.Stop();
                form.Close();
            }
        };
        form.Shown += (_, _) => { shownMilliseconds = launch.Elapsed.TotalMilliseconds; timer.Start(); };
        Application.Run(form);
        form.SamplingTask?.Wait(TimeSpan.FromSeconds(5));
        if (failure.Length == 0) return 0;
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
        File.WriteAllText(output, JsonSerializer.Serialize(new { passed = false, error = failure }));
        return 1;
    }

    private static int Read(string[] arguments, string name, int fallback)
    {
        var index = Array.IndexOf(arguments, name);
        return index >= 0 && index + 1 < arguments.Length && int.TryParse(arguments[index + 1], out var value) ? value : fallback;
    }

    internal static object Summarize(IEnumerable<double> values)
    {
        var ordered = values.Order().ToArray();
        return new
        {
            count = ordered.Length,
            averageMilliseconds = ordered.Length > 0 ? ordered.Average() : (double?)null,
            medianMilliseconds = ordered.Length > 0 ? ordered[ordered.Length / 2] : (double?)null,
            p95Milliseconds = ordered.Length > 0 ? ordered[Math.Max(0, (int)Math.Ceiling(ordered.Length * .95) - 1)] : (double?)null,
            maximumMilliseconds = ordered.Length > 0 ? ordered[^1] : (double?)null
        };
    }

    [DllImport("user32.dll")] private static extern int GetGuiResources(nint process, uint flags);
}
