[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'artifacts/compiler-study'),
    [ValidateRange(2, 10)][int]$Repetitions = 3,
    [ValidateRange(100, 1000)][int]$Samples = 500
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$variants = @(
    @{ Name = 'balanced'; Size = 'ON'; Lto = 'ON'; Flags = '/O2 /Ob2 /DNDEBUG' },
    @{ Name = 'speed'; Size = 'OFF'; Lto = 'ON'; Flags = '/O2 /Ob2 /DNDEBUG' },
    @{ Name = 'no-lto'; Size = 'ON'; Lto = 'OFF'; Flags = '/O2 /Ob2 /DNDEBUG' },
    @{ Name = 'inline3'; Size = 'ON'; Lto = 'ON'; Flags = '/O2 /Ob3 /DNDEBUG' }
)
$binaries = foreach ($variant in $variants) {
    foreach ($kind in 'production', 'diagnostic') {
        $directory = Join-Path $OutputDirectory "$($variant.Name)-$kind"
        $diagnostics = if ($kind -eq 'diagnostic') { 'ON' } else { 'OFF' }
        & cmake -S $PSScriptRoot -B $directory "-DTASKMGR_FAVOR_SIZE=$($variant.Size)" "-DTASKMGR_LTO=$($variant.Lto)" "-DTASKMGR_DIAGNOSTICS=$diagnostics" "-DCMAKE_CXX_FLAGS_RELEASE=$($variant.Flags)" *> "$directory-configure.log"
        if ($LASTEXITCODE -ne 0) { throw "Configuration failed: $directory-configure.log" }
        & cmake --build $directory --config Release --parallel 4 *> "$directory-build.log"
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $directory-build.log" }
        $executable = Join-Path $directory 'Release/TaskManager.exe'
        [pscustomobject]@{ variant = $variant.Name; kind = $kind; path = $executable; bytes = (Get-Item -LiteralPath $executable).Length; sha256 = (Get-FileHash -LiteralPath $executable).Hash; flags = $variant.Flags; favorSize = $variant.Size; lto = $variant.Lto }
        Write-Host "Built $($variant.Name) $kind"
    }
}
$binaries | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'binaries.json') -Encoding utf8
$runs = foreach ($scenario in 'self-test', 'components', 'sampler') {
    $arguments = switch ($scenario) { 'self-test' { '--self-test' }; 'components' { '--component-benchmark' }; 'sampler' { "--benchmark --samples $Samples --interval 0 --warmup-samples 10" } }
    foreach ($repetition in 1..$Repetitions) {
        $order = @($binaries | Where-Object kind -EQ diagnostic)
        if ($repetition % 2 -eq 0) { [array]::Reverse($order) }
        foreach ($binary in $order) {
            $reportPath = Join-Path $OutputDirectory "$scenario-$($binary.variant)-$repetition.json"
            if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
            $process = Start-Process -FilePath $binary.path -ArgumentList "$arguments --output `"$reportPath`"" -WindowStyle Hidden -PassThru
            try {
                if (-not $process.WaitForExit(120000)) { $process.Kill(); $process.WaitForExit(); throw "Owned benchmark exceeded deadline: $reportPath" }
                if ($process.ExitCode -ne 0) { throw "Benchmark failed: $reportPath" }
            } finally { $process.Dispose() }
            $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
            if (-not $report.passed) { throw "Validation failed: $reportPath" }
            [pscustomobject]@{ scenario = $scenario; variant = $binary.variant; repetition = $repetition; reportPath = $reportPath; measurements = $report }
            Write-Host "Passed $scenario $($binary.variant) repetition $repetition"
        }
    }
}
[pscustomobject]@{ timestampUtc = [DateTime]::UtcNow.ToString('o'); operatingSystem = [Environment]::OSVersion.VersionString; repetitions = $Repetitions; samples = $Samples; binaries = @($binaries); runs = @($runs) } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'study.json') -Encoding utf8
