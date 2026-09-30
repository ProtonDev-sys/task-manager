using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using TaskManager.Monitoring;
using TaskManager.UI;

namespace TaskManager;

internal static class Diagnostics
{
    public static int SelfTest(string[] args)
    {
        var output = Option(args, "--output", "artifacts/self-test.json");
        var checks = new List<string>();
        try
        {
            Check(Marshal.SizeOf<NativeMethods.ProcessInformation>() == 256, "x64 native process structure", checks);
            Check(Marshal.OffsetOf<NativeMethods.ProcessInformation>(nameof(NativeMethods.ProcessInformation.UniqueProcessId)).ToInt32() == 80,
                "native PID offset", checks);
            Check(Metrics.CpuPercent(0, 10_000_000, 1, 4) == 25, "CPU normalization", checks);
            Check(Metrics.CpuPercent(20, 10, 1, 4) == 0, "CPU counter reset", checks);
            Check(Metrics.CpuPercent(0, 10, 0, 4) == 0, "zero interval", checks);
            Check(Metrics.Rate(100, 300, 2) == 100 && Metrics.Rate(300, 100, 2) == 0, "I/O rate and counter reset", checks);
            Check(Metrics.CpuTime(TimeSpan.FromHours(25).Ticks) == "25:00:00" && Metrics.CpuTime(-1) == "0:00:00", "CPU time format preserves accumulated hours", checks);
            using var sampler = new SystemSampler(true);
            var first = sampler.Sample();
            Thread.Sleep(250);
            var second = sampler.Sample();
            Check(second.Processes.Any(process => process.Id == Environment.ProcessId), "current process present", checks);
            Check(second.Processes.All(process => process.Cpu is >= 0 and <= 100 && process.WorkingSet >= 0), "process metric ranges", checks);
            Check(second.Processes.Select(process => process.Id).Distinct().Count() == second.Processes.Length, "unique process identities", checks);
            Check(second.MonotonicSeconds > first.MonotonicSeconds, "graph clock is monotonic", checks);
            Check(second.Processes.Any(process => process.Id == 0 && process.Name == "System Idle Process"), "system idle process in native details inventory", checks);
            Check(second.Processes.Any(process => process.Id == -1 && process.Name == "System interrupts" && double.IsFinite(process.Cpu)), "interrupt activity pseudo-process", checks);
            Check(second.TotalMemory > 0 && second.AvailableMemory <= second.TotalMemory, "physical memory range", checks);
            Check(second.Cpu is >= 0 and <= 100 && double.IsFinite(second.Cpu), "system CPU range", checks);
            Check(second.CpuCores.Length > 0 && second.CpuCores.All(value => value is >= 0 and <= 100), "per-core CPU ranges", checks);
            Check(second.Gpus.All(gpu => gpu.Usage is >= 0 and <= 100 && gpu.Dedicated >= 0 && gpu.Shared >= 0), "GPU counter ranges", checks);
            Check(second.Sessions.All(session => session.Id >= 0 && session.Name.Length > 0), "named Windows user sessions", checks);
            Check(second.CommitLimit >= second.CommitBytes && second.ThreadCount > 0 && second.HandleCount > 0, "system counters", checks);
            Check(second.Networks.All(network => network.SendRate >= 0 && network.ReceiveRate >= 0), "network rate ranges", checks);
            Check(second.Disks.All(disk => disk.Active == null || disk.Active is >= 0 and <= 100), "disk range or explicit unavailable", checks);
            var ownName = second.Processes.First(process => process.Id == Environment.ProcessId).Name;
            Check(ownName == "TaskManager.exe", "native image name (" + ownName + ")", checks);

            var executable = Environment.ProcessPath ?? throw new InvalidOperationException("No executable path.");
            using (var child = Process.Start(new ProcessStartInfo(executable, "--test-child") { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden })
                ?? throw new InvalidOperationException("Could not start test child."))
            {
                try
                {
                    Thread.Sleep(150);
                    var childSample = sampler.Sample().Processes.FirstOrDefault(process => process.Id == child.Id);
                    Check(childSample != null && childSample.Cpu == 0, "new process has a fresh CPU baseline", checks);
                    FeatureTests.Child(child, childSample!, checks);
                    ProcessActions.End(childSample!);
                    Check(child.WaitForExit(4000), "safe termination of owned test child", checks);
                    Check(!sampler.Sample().Processes.Any(process => process.Id == child.Id), "exited process removed", checks);
                }
                finally { if (!child.HasExited) { child.Kill(); child.WaitForExit(4000); } }
            }
            try { ProcessActions.End(second.Processes.First(process => process.Id == Environment.ProcessId)); throw new InvalidOperationException("Self-termination was not refused."); }
            catch (InvalidOperationException error) when (error.Message.Contains("cannot be ended", StringComparison.Ordinal))
            { checks.Add("self-termination refused"); }
            var serviceEntries = SystemInventory.Services();
            Check(serviceEntries.Length > 0 && serviceEntries.Any(service => service.Status == "Running"), "live service inventory", checks);
            var startupEntries = SystemInventory.Startup();
            Check(startupEntries.All(entry => !string.IsNullOrEmpty(entry.Command)), "startup inventory", checks);
            var cpu = SystemInventory.Cpu();
            Check(cpu.Cores > 0 && cpu.Sockets > 0 && !string.IsNullOrEmpty(cpu.Name), "CPU topology", checks);
            Check(cpu.Caches.Length == 3 && cpu.Caches.All(bytes => bytes >= 0), "bounded native CPU cache inventory", checks);
            FeatureTests.Run(second, checks);

            var directory = Path.GetDirectoryName(Path.GetFullPath(output))!;
            Directory.CreateDirectory(directory);
            for (var index = 0; index < 14; index++) second = sampler.Sample();
            Check(second.Metadata.Values.Any(metadata => metadata.Icon != null), "cached executable icons", checks);
            using (var form = new MainForm(new AppSettings()))
            {
                form.RenderSample(first, 1, Path.Combine(directory, "performance.png"));
                form.RenderSample(second, 0, Path.Combine(directory, "processes.png"));
                form.RenderSample(second, 1, Path.Combine(directory, "performance.png"));
                form.RenderSample(second, 5, Path.Combine(directory, "details.png"));
                foreach (var adapter in second.Gpus)
                {
                    var graph = form.Performance;
                    graph.SelectedResource = "gpu:" + adapter.Id;
                    form.RenderSample(second, 1, Path.Combine(directory, $"gpu-{adapter.Index}.png"));
                    graph.SelectedResource = "cpu";
                }
                form.RenderSample(second, 0, Path.Combine(directory, "processes.png"));
                var processList = form.ProcessView;
                processList.SelectProcess(Environment.ProcessId);
                Check(processList.SelectedProcess?.Id == Environment.ProcessId, "virtual row selection", checks);
                processList.UpdateSample(second);
                Check(processList.SelectedProcess?.Id == Environment.ProcessId, "selection survives refresh", checks);
                form.Close();
            }
            checks.Add("offscreen Processes / Performance / Details rendering");
            SmokeTest(checks);
            Write(output, new { passed = true, checks, processes = second.Processes.Length, services = serviceEntries.Length,
                startup = startupEntries.Length, disks = second.Disks.Length, networks = second.Networks.Length, cpu,
                gpus = second.Gpus, metadataCount = second.Metadata.Count, sessions = second.Sessions, firstSampleMilliseconds = first.SampleMilliseconds });
            return 0;
        }
        catch (Exception error)
        {
            Write(output, new { passed = false, checks, error = error.ToString() });
            return 1;
        }
    }

    private static void SmokeTest(List<string> checks)
    {
        using var form = new MainForm(new AppSettings { UpdateInterval = 500 }, true) { ShowInTaskbar = false, Opacity = 0 };
        using var timer = new System.Windows.Forms.Timer { Interval = 100 };
        var clock = Stopwatch.StartNew();
        var phase = 0;
        DateTime pausedTimestamp = default;
        Exception? failure = null;
        timer.Tick += (_, _) =>
        {
            try
            {
                if (form.SamplingError != null) throw new InvalidOperationException(form.SamplingError);
                var elapsed = clock.ElapsedMilliseconds;
                if (phase == 0 && elapsed >= 600)
                {
                    Check(form.CurrentSample != null, "background sampler publishes to UI", checks);
                    form.SearchText = Environment.ProcessId.ToString();
                    Check(form.ProcessView.VisibleRowCount == 1, "live search narrows Processes by PID", checks);
                    form.SelectedTab = 5;
                    Check(form.DetailsView.VisibleRowCount == 1, "search follows navigation to Details", checks);
                    form.SearchText = "missing-process-name-for-test";
                    Check(form.DetailsView.VirtualListSize == 0, "live unmatched search returns no rows", checks);
                    form.SearchText = "";
                    Check(form.DetailsView.VisibleRowCount > 1, "clearing live search restores processes", checks);
                    form.SelectedTab = 1;
                    phase++;
                }
                else if (phase == 1 && elapsed >= 900) { form.SelectedTab = 3; phase++; }
                else if (phase == 2 && elapsed >= 1200) { form.SelectedTab = 6; phase++; }
                else if (phase == 3 && elapsed >= 1600) { form.UpdateInterval = 0; phase++; }
                else if (phase == 4 && elapsed >= 2100) { pausedTimestamp = form.CurrentSample!.Timestamp; phase++; }
                else if (phase == 5 && elapsed >= 2600)
                {
                    Check(form.CurrentSample!.Timestamp == pausedTimestamp, "pause stops automatic snapshots", checks);
                    form.RefreshNow();
                    phase++;
                }
                else if (phase == 6 && elapsed >= 3100)
                {
                    Check(form.CurrentSample!.Timestamp > pausedTimestamp, "manual refresh works while paused", checks);
                    form.UpdateInterval = 500;
                    form.SelectedTab = 5;
                    phase++;
                }
                else if (phase == 7 && elapsed >= 3400) { form.SelectedTab = 4; phase++; }
                else if (phase == 8 && elapsed >= 3600) { timer.Stop(); form.Close(); }
            }
            catch (Exception error) { failure = error; timer.Stop(); form.Close(); }
        };
        form.Shown += (_, _) => timer.Start();
        Application.Run(form);
        if (failure != null) throw new InvalidOperationException("UI smoke test failed.", failure);
        Check(form.SamplingTask?.Wait(2000) == true, "sampler cancels cleanly on window close", checks);
        checks.Add("live UI tab changes and async inventory loading");
    }

    public static int Benchmark(string[] args)
    {
        var output = Option(args, "--output", "artifacts/sampler-benchmark.json");
        try
        {
            var count = Math.Clamp(int.Parse(Option(args, "--samples", "30")), 5, 3600);
            var interval = Math.Clamp(int.Parse(Option(args, "--interval", "1000")), 100, 10_000);
            using var sampler = new SystemSampler(args.Contains("--enriched", StringComparer.Ordinal));
            using var ownProcess = Process.GetCurrentProcess();
            for (var index = 0; index < 3; index++) { sampler.Sample(); Thread.Sleep(100); }
            ownProcess.Refresh();
            var beforeCpu = ownProcess.TotalProcessorTime;
            var beforeHandles = ownProcess.HandleCount;
            var beforeAllocation = GC.GetTotalAllocatedBytes(true);
            var beforeCollections = Enumerable.Range(0, 3).Select(GC.CollectionCount).ToArray();
            var durations = new double[count];
            var clock = Stopwatch.StartNew();
            SystemSample? last = null;
            for (var index = 0; index < count; index++)
            {
                var started = Stopwatch.GetTimestamp();
                last = sampler.Sample();
                durations[index] = Stopwatch.GetElapsedTime(started).TotalMilliseconds;
                Thread.Sleep(Math.Max(0, interval - (int)Math.Ceiling(durations[index])));
            }
            clock.Stop();
            var allocated = GC.GetTotalAllocatedBytes(true) - beforeAllocation;
            ownProcess.Refresh();
            var cpuMilliseconds = (ownProcess.TotalProcessorTime - beforeCpu).TotalMilliseconds;
            Array.Sort(durations);
            Write(output, new
            {
                scope = "Sampler only; excludes UI painting. Warmed up with three samples. No built-in Task Manager comparison.",
                timestamp = DateTimeOffset.Now, samples = count, intervalMilliseconds = interval, elapsedSeconds = clock.Elapsed.TotalSeconds,
                averageMilliseconds = durations.Average(), medianMilliseconds = durations[count / 2], p95Milliseconds = durations[(int)Math.Ceiling(count * .95) - 1],
                maximumMilliseconds = durations[^1], allocatedBytesPerSample = allocated / count,
                processCpuMilliseconds = cpuMilliseconds, cpuPercentOfOneCore = 100 * cpuMilliseconds / clock.Elapsed.TotalMilliseconds,
                cpuPercentOfMachine = 100 * cpuMilliseconds / clock.Elapsed.TotalMilliseconds / Environment.ProcessorCount,
                workingSetBytes = ownProcess.WorkingSet64, privateBytes = ownProcess.PrivateMemorySize64,
                handleDelta = ownProcess.HandleCount - beforeHandles,
                collections = Enumerable.Range(0, 3).Select(index => GC.CollectionCount(index) - beforeCollections[index]).ToArray(),
                processCount = last?.Processes.Length, diskCount = last?.Disks.Length, networkCount = last?.Networks.Length
            });
            return 0;
        }
        catch (Exception error) { Write(output, new { error = error.ToString() }); return 1; }
    }

    private static string Option(string[] args, string name, string fallback)
    {
        var index = Array.IndexOf(args, name);
        return index >= 0 && index + 1 < args.Length ? args[index + 1] : fallback;
    }

    private static void Check(bool condition, string name, List<string> checks)
    {
        if (!condition) throw new InvalidOperationException("Failed: " + name);
        checks.Add(name);
    }

    private static void Write(string path, object value)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        File.WriteAllText(path, JsonSerializer.Serialize(value, new JsonSerializerOptions { WriteIndented = true }));
    }
}
