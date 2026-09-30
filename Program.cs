using TaskManager.UI;

namespace TaskManager;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.SetDefaultFont(new Font("Segoe UI", 9));
        if (args.Contains("--self-test", StringComparer.Ordinal))
        {
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);
            return Diagnostics.SelfTest(args);
        }
        if (args.Contains("--benchmark", StringComparer.Ordinal)) return Diagnostics.Benchmark(args);
        if (args.Contains("--component-benchmark", StringComparer.Ordinal)) return ComponentBenchmark.Run(args);
        if (args.Contains("--ui-benchmark", StringComparer.Ordinal))
        {
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);
            return UiBenchmark.Run(args);
        }
        if (args.Contains("--screenshots", StringComparer.Ordinal)) return Screenshots.Run(args);
        if (args.Contains("--test-child", StringComparer.Ordinal)) { Thread.Sleep(30_000); return 0; }
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        Application.ThreadException += (_, eventArgs) => MessageBox.Show(eventArgs.Exception.Message,
            "Task Manager", MessageBoxButtons.OK, MessageBoxIcon.Error);
        using var form = new MainForm();
        Application.Run(form);
        try { form.SamplingTask?.Wait(TimeSpan.FromSeconds(5)); }
        catch (AggregateException error) { System.Diagnostics.Debug.WriteLine(error.Message); }
        return 0;
    }
}
