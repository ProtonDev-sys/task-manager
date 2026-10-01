[CmdletBinding()]
param(
    [ValidateRange(5, 600)][int]$Seconds = 15,
    [string]$Executable = (Join-Path $PSScriptRoot 'artifacts/native-tests/Release/TaskManager.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'artifacts/benchmark-suite'),
    [switch]$EnforceBudgets
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw 'Build the optimized developer executable with ./build.ps1 -TestConfiguration Release before benchmarking.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$cases = @(
    @{ Name = 'self-test'; Arguments = '--self-test' },
    @{ Name = 'components'; Arguments = '--component-benchmark' },
    @{ Name = 'sampler'; Arguments = '--benchmark --samples 30 --interval 100' },
    @{ Name = 'all-tabs'; Arguments = "--ui-benchmark --seconds $Seconds" },
    @{ Name = 'interaction'; Arguments = "--ui-benchmark --seconds $Seconds" },
    @{ Name = 'idle'; Arguments = "--ui-benchmark --idle --seconds $Seconds" },
    @{ Name = 'process-sort-spam'; Arguments = "--ui-benchmark --idle --sort-spam --tab 0 --seconds $Seconds" },
    @{ Name = 'details-sort-spam'; Arguments = "--ui-benchmark --idle --sort-spam --tab 5 --seconds $Seconds" },
    @{ Name = 'tab-spam'; Arguments = "--ui-benchmark --idle --tab-spam --seconds $Seconds" },
    @{ Name = 'dark-tab-spam'; Arguments = "--ui-benchmark --dark --idle --tab-spam --seconds $Seconds" },
    @{ Name = 'high-speed'; Arguments = "--ui-benchmark --interval 500 --seconds $Seconds" },
    @{ Name = 'low-speed'; Arguments = "--ui-benchmark --interval 4000 --seconds $Seconds" },
    @{ Name = 'paused'; Arguments = "--ui-benchmark --idle --interval 0 --seconds $Seconds" }
    @{ Name = 'minimized'; Arguments = "--ui-benchmark --minimized --seconds $Seconds" }
)
foreach ($tab in 0..6) { $cases += @{ Name = "tab-$tab"; Arguments = "--ui-benchmark --tab $tab --seconds $Seconds" } }
$results = foreach ($case in $cases) {
    $reportPath = Join-Path $OutputDirectory ($case.Name + '.json')
    $process = Start-Process -FilePath $Executable -ArgumentList "$($case.Arguments) --output `"$reportPath`"" -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(($Seconds + 60) * 1000)) {
        $process.Kill()
        throw "Benchmark $($case.Name) exceeded its deadline. Only its owned diagnostic process was stopped."
    }
    if ($process.ExitCode -ne 0) { throw "Benchmark $($case.Name) failed with exit code $($process.ExitCode). See $reportPath" }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.passed -eq $false -or $report.error) { throw "Benchmark $($case.Name) failed. See $reportPath" }
    if ($EnforceBudgets -and $report.uiUpdate.count -gt 0) {
        if ($report.uiUpdate.p95Milliseconds -gt 50 -or $report.cachedSort.p95Milliseconds -gt 50 -or $report.tabSwitch.p95Milliseconds -gt 50 -or $report.messageQueue.p95Milliseconds -gt 100 -or $report.letterNavigation.p95Milliseconds -gt 100 -or $report.firstSampleMilliseconds -gt 2000) {
            throw "Responsiveness budget exceeded for $($case.Name). See $reportPath"
        }
    }
    Write-Host "$($case.Name): passed"
    [pscustomobject]@{ name = $case.Name; passed = $true; report = $reportPath }
}
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'suite.json') -Encoding utf8
