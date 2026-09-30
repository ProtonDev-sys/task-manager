[CmdletBinding()]
param(
    [ValidateRange(5, 600)][int]$Seconds = 15,
    [string]$Executable = (Join-Path $PSScriptRoot 'artifacts/app/TaskManager.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'artifacts/benchmark-suite'),
    [switch]$EnforceBudgets
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$cases = @(
    @{ Name = 'self-test'; Arguments = '--self-test' },
    @{ Name = 'components'; Arguments = '--component-benchmark' },
    @{ Name = 'sampler'; Arguments = '--benchmark --samples 30 --interval 100' },
    @{ Name = 'enriched-sampler'; Arguments = '--benchmark --enriched --samples 30 --interval 100' },
    @{ Name = 'all-tabs'; Arguments = "--ui-benchmark --seconds $Seconds" },
    @{ Name = 'interaction'; Arguments = "--ui-benchmark --exercise --seconds $Seconds" },
    @{ Name = 'high-speed'; Arguments = "--ui-benchmark --interval 500 --seconds $Seconds" },
    @{ Name = 'low-speed'; Arguments = "--ui-benchmark --interval 4000 --seconds $Seconds" },
    @{ Name = 'paused'; Arguments = "--ui-benchmark --interval 0 --seconds $Seconds" }
    @{ Name = 'minimized'; Arguments = "--ui-benchmark --minimized --seconds $Seconds" }
)
foreach ($tab in 0..6) { $cases += @{ Name = "tab-$tab"; Arguments = "--ui-benchmark --tab $tab --seconds $Seconds" } }
$results = foreach ($case in $cases) {
    $reportPath = Join-Path $OutputDirectory ($case.Name + '.json')
    $process = Start-Process -FilePath $Executable -ArgumentList "$($case.Arguments) --output `"$reportPath`"" -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Benchmark $($case.Name) failed with exit code $($process.ExitCode). See $reportPath" }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.passed -eq $false -or $report.error) { throw "Benchmark $($case.Name) failed. See $reportPath" }
    if ($EnforceBudgets -and $report.uiUpdate.count -gt 0) {
        if ($report.uiUpdate.p95Milliseconds -gt 50 -or $report.inputQueue.p95Milliseconds -gt 100 -or $report.navigation.p95Milliseconds -gt 100 -or $report.firstSampleMilliseconds -gt 2000) {
            throw "Responsiveness budget exceeded for $($case.Name). See $reportPath"
        }
    }
    Write-Host "$($case.Name): passed"
    [pscustomobject]@{ name = $case.Name; passed = $true; report = $reportPath }
}
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'suite.json') -Encoding utf8
