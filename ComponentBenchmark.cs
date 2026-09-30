using System.Diagnostics;
using System.Text.Json;
using TaskManager.Monitoring;
using TaskManager.UI;

namespace TaskManager;

internal static class ComponentBenchmark
{
    public static int Run(string[] arguments)
    {
        var outputIndex = Array.IndexOf(arguments, "--output");
        var output = outputIndex >= 0 && outputIndex + 1 < arguments.Length ? arguments[outputIndex + 1] : "artifacts/components.json";
        var reports = new Dictionary<string, object>();
        try
        {
            using var sampler = new SystemSampler(true);
            sampler.Sample();
            Thread.Sleep(250);
            var sample = sampler.Sample();
            using var host = new Form { ClientSize = new Size(1000, 700), ShowInTaskbar = false };
            using var processes = new ProcessList(false);
            using var details = new ProcessList(true);
            using var inventory = new InventoryView();
            using var performance = new PerformanceView();
            host.Controls.AddRange([processes, details, inventory, performance]);
            _ = host.Handle;
            foreach (Control control in host.Controls) _ = control.Handle;
            using var bitmap = new Bitmap(1000, 700);
            foreach (var count in new[] { 1000, 5000 })
            {
                var stress = Synthetic(sample, count);
                processes.GroupByType = false;
                processes.Filter = "";
                processes.UpdateSample(stress);
                processes.SelectProcess(stress.Processes[0].Id);
                reports[$"processes/{count}/update"] = Measure(() => processes.UpdateSample(stress));
                processes.GroupByType = true;
                reports[$"processes/{count}/categories"] = Measure(() => processes.UpdateSample(stress));
                processes.ExpandAll(true);
                reports[$"processes/{count}/expanded"] = Measure(() => processes.UpdateSample(stress));
                reports[$"processes/{count}/search"] = Measure(() => { processes.Filter = "publisher 2"; processes.Filter = ""; });
                reports[$"processes/{count}/paint"] = Measure(() => processes.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size)));
                details.UpdateSample(stress);
                reports[$"details/{count}/update"] = Measure(() => details.UpdateSample(stress));
                reports[$"details/{count}/search"] = Measure(() => { details.Filter = "10042"; details.Filter = ""; });
                reports[$"details/{count}/paint"] = Measure(() => details.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size)));
                var items = stress.Processes.Select(process => new ListViewItem([process.Name, process.Id.ToString(), process.Cpu.ToString("0.0")]) { Tag = process }).ToArray();
                if (inventory.Columns.Count == 0)
                {
                    inventory.Columns.Add("Name", 350);
                    inventory.Columns.Add("PID", 100);
                    inventory.Columns.Add("CPU", 100);
                }
                inventory.ReplaceRows(items);
                reports[$"inventory/{count}/update"] = Measure(() => inventory.ReplaceRows(items));
                reports[$"inventory/{count}/sort"] = Measure(() => inventory.SortByColumn(1));
                reports[$"inventory/{count}/search"] = Measure(() => { inventory.Filter = "10042"; inventory.Filter = ""; });
                reports[$"grouping/{count}"] = Measure(() => new ProcessRows { GroupByType = true }.Build(stress.Processes, stress.Metadata,
                    (left, right) => left.Id.CompareTo(right.Id), process => process.Name));
                var history = new AppHistory(false);
                reports[$"history/{count}/update"] = Measure(() => history.Update(stress));
            }
            performance.UpdateSample(sample);
            reports["performance/history-update"] = Measure(() => performance.UpdateSample(sample with { MonotonicSeconds = sample.MonotonicSeconds + 1 }));
            var resourceIndex = 0;
            foreach (var resource in performance.ResourceKeys)
            {
                performance.SelectedResource = resource;
                reports["performance/paint/" + resource.Split(':')[0] + "/" + resourceIndex++] = Measure(() => performance.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size)));
            }
            Write(output, new
            {
                passed = true, timestamp = DateTimeOffset.Now,
                scope = "Headless native controls; deterministic 1,000/5,000-process fixtures; 10 warmups and 30 measurements per operation. Paint excludes desktop composition. No live process mutations.",
                reports
            });
            return 0;
        }
        catch (Exception error) { Write(output, new { passed = false, error = error.ToString() }); return 1; }
    }

    private static object Measure(Action action)
    {
        for (var index = 0; index < 10; index++) action();
        var before = GC.GetAllocatedBytesForCurrentThread();
        var durations = new double[30];
        for (var index = 0; index < durations.Length; index++)
        {
            var started = Stopwatch.GetTimestamp();
            action();
            durations[index] = Stopwatch.GetElapsedTime(started).TotalMilliseconds;
        }
        return new { timing = UiBenchmark.Summarize(durations), allocatedBytesPerOperation = (GC.GetAllocatedBytesForCurrentThread() - before) / durations.Length };
    }

    private static SystemSample Synthetic(SystemSample sample, int count)
    {
        var processes = new ProcessSample[count];
        var metadata = new Dictionary<(int, long), ProcessMetadata>(count);
        for (var index = 0; index < count; index++)
        {
            var process = new ProcessSample(10000 + index, index % 10 == 0 ? 0 : 10000 + index / 10 * 10,
                "Application " + index / 10 + ".exe", 10000 + index, index % 100 / 10d, 1048576 * (index % 100 + 1L), 0, 10, 20, 1, 0, 0)
            { DiskRate = index * 100, NetworkRate = index * 10, Gpu = index % 10, CpuTime = index * 1000 };
            processes[index] = process;
            metadata[(process.Id, process.Created)] = new(process.Name, "C:\\Example\\" + process.Name, "Publisher " + index % 7, null, index % 10 == 0);
        }
        return sample with { Processes = processes, Metadata = metadata, ProcessCount = count, Services = new Dictionary<int, ServiceEntry[]>() };
    }

    private static void Write(string path, object result)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        File.WriteAllText(path, JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true }));
    }
}
