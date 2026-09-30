[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    cmake -S . -B artifacts/native-build -A x64
    if ($LASTEXITCODE -ne 0) { throw 'Native CMake configuration failed.' }
    cmake --build artifacts/native-build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Native release build failed.' }
    ctest --test-dir artifacts/native-build -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
    New-Item -ItemType Directory -Force artifacts/app | Out-Null
    Copy-Item -LiteralPath artifacts/native-build/Release/TaskManager.exe -Destination artifacts/app/TaskManager.exe
    $executable = Join-Path $PSScriptRoot 'artifacts\app\TaskManager.exe'
    $reportPath = Join-Path $PSScriptRoot 'artifacts\release-self-test.json'
    $test = Start-Process -FilePath $executable -ArgumentList "--self-test --output `"$reportPath`"" -WindowStyle Hidden -Wait -PassThru
    if ($test.ExitCode -ne 0) { throw "Published self-test failed with exit code $($test.ExitCode)." }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if (-not $report.passed) { throw 'Published self-test report did not pass.' }
    Write-Output "Published native C++ $executable; $($report.checks.Count) checks passed. No .NET runtime is required."
}
finally { Pop-Location }
