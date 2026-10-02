[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BaselineExecutable,
    [string]$Executable = (Join-Path $PSScriptRoot 'artifacts/native-tests/Release/TaskManager.exe'),
    [string]$BaselineProductionExecutable,
    [string]$ProductionExecutable = (Join-Path $PSScriptRoot 'artifacts/native-build/Release/TaskManager.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'artifacts/performance-comparison'),
    [ValidateRange(2, 10)][int]$Repetitions = 5,
    [ValidateRange(100, 1000)][int]$Samples = 500,
    [ValidateRange(5, 600)][int]$Seconds = 30,
    [ValidateRange(0, 60)][int]$WarmupSeconds = 20,
    [switch]$ThemeComparison
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$executables = @{ baseline = (Resolve-Path -LiteralPath $BaselineExecutable).Path; candidate = (Resolve-Path -LiteralPath $Executable).Path }
$identities = foreach ($variant in 'baseline', 'candidate') {
    $path = $executables[$variant]
    [pscustomobject]@{ variant = $variant; path = $path; bytes = (Get-Item -LiteralPath $path).Length; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
}
$production = foreach ($item in @(@{ Variant = 'baseline'; Path = $BaselineProductionExecutable }, @{ Variant = 'candidate'; Path = $ProductionExecutable })) {
    if ($item.Path) {
        $path = (Resolve-Path -LiteralPath $item.Path).Path
        [pscustomobject]@{ variant = $item.Variant; bytes = (Get-Item -LiteralPath $path).Length; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    }
}
$cases = @(
    @{ Name = 'sampler'; Arguments = "--benchmark --samples $Samples --interval 0 --warmup-samples 10" },
    @{ Name = 'idle'; Arguments = "--ui-benchmark --idle --tab 0 --warmup $WarmupSeconds --seconds $Seconds" },
    @{ Name = 'components'; Arguments = '--component-benchmark' }
)
if ($ThemeComparison) {
    $cases += @(
        @{ Name = 'dark-idle'; Arguments = "--ui-benchmark --dark --idle --tab 0 --warmup $WarmupSeconds --seconds $Seconds" },
        @{ Name = 'light-performance'; Arguments = "--ui-benchmark --idle --tab 1 --warmup $WarmupSeconds --seconds $Seconds" },
        @{ Name = 'dark-performance'; Arguments = "--ui-benchmark --dark --idle --tab 1 --warmup $WarmupSeconds --seconds $Seconds" },
        @{ Name = 'light-tabs'; Arguments = "--ui-benchmark --idle --tab-spam --warmup $WarmupSeconds --seconds $Seconds" },
        @{ Name = 'dark-tabs'; Arguments = "--ui-benchmark --dark --idle --tab-spam --warmup $WarmupSeconds --seconds $Seconds" }
    )
}
$runs = foreach ($case in $cases) {
    foreach ($repetition in 1..$Repetitions) {
        $order = if ($repetition % 2) { @('baseline', 'candidate') } else { @('candidate', 'baseline') }
        foreach ($variant in $order) {
            $reportPath = Join-Path $OutputDirectory "$($case.Name)-$variant-$repetition.json"
            if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
            $process = Start-Process -FilePath $executables[$variant] -ArgumentList "$($case.Arguments) --output `"$reportPath`"" -WindowStyle Hidden -PassThru
            try {
                if (-not $process.WaitForExit(($Seconds + 90) * 1000)) {
                    $process.Kill()
                    $process.WaitForExit()
                    throw "$($case.Name) $variant exceeded its deadline; only the owned benchmark was stopped."
                }
                if ($process.ExitCode -ne 0) { throw "$($case.Name) $variant failed with exit code $($process.ExitCode). See $reportPath" }
            } finally { $process.Dispose() }
            $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
            if (-not $report.passed) { throw "Benchmark validation failed. See $reportPath" }
            $metrics = [ordered]@{}
            foreach ($property in $report.PSObject.Properties) {
                if ($property.Name -in @('processCpuPercentOneCore', 'cpuMillisecondsPerSample', 'privateBytes', 'workingSetBytes', 'peakWorkingSetBytes', 'handles', 'minimumProcesses', 'maximumProcesses', 'firstSampleMilliseconds')) {
                    $metrics[$property.Name] = [double]$property.Value
                } elseif ($null -ne $property.Value -and $null -ne $property.Value.PSObject.Properties['meanMilliseconds']) {
                    $metrics["$($property.Name).meanMilliseconds"] = [double]$property.Value.meanMilliseconds
                    $metrics["$($property.Name).p95Milliseconds"] = [double]$property.Value.p95Milliseconds
                }
            }
            if ($case.Name -eq 'sampler' -and -not $metrics.Contains('cpuMillisecondsPerSample')) {
                throw 'Both executables need the instrumented headless sampler benchmark; timing-only historical reports cannot measure CPU reliably.'
            }
            Write-Host "$($case.Name) $variant repetition ${repetition}: passed"
            [pscustomobject]@{ scenario = $case.Name; variant = $variant; repetition = $repetition; report = $reportPath; metrics = $metrics }
        }
    }
}
function Get-Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2
}
$summary = foreach ($case in $cases) {
    $selected = @($runs | Where-Object scenario -EQ $case.Name)
    foreach ($name in $selected[0].metrics.Keys) {
        $baseline = @($selected | Where-Object variant -EQ baseline | ForEach-Object { $_.metrics[$name] })
        $candidate = @($selected | Where-Object variant -EQ candidate | ForEach-Object { $_.metrics[$name] })
        $before = Get-Median $baseline; $after = Get-Median $candidate
        [pscustomobject]@{
            scenario = $case.Name; metric = $name; baselineMedian = $before; candidateMedian = $after
            changePercent = if ($before -ne 0) { ($after - $before) * 100 / $before } else { $null }
            baselineMin = ($baseline | Measure-Object -Minimum).Minimum; baselineMax = ($baseline | Measure-Object -Maximum).Maximum
            candidateMin = ($candidate | Measure-Object -Minimum).Minimum; candidateMax = ($candidate | Measure-Object -Maximum).Maximum
        }
    }
}
[pscustomobject]@{
    timestampUtc = [DateTime]::UtcNow.ToString('o'); operatingSystem = [Environment]::OSVersion.VersionString
    logicalProcessors = [Environment]::ProcessorCount; repetitions = $Repetitions; samples = $Samples; seconds = $Seconds; warmupSeconds = $WarmupSeconds
    diagnosticExecutables = @($identities); productionExecutables = @($production); summary = @($summary); runs = @($runs)
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'comparison.json') -Encoding utf8
$summary | Where-Object { $_.metric -in @('cpuMillisecondsPerSample', 'privateBytes', 'processCpuPercentOneCore', 'sampler.meanMilliseconds', 'uiUpdate.meanMilliseconds') } | Format-Table -AutoSize
