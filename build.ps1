[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    dotnet restore --locked-mode
    if ($LASTEXITCODE -ne 0) { throw 'Dependency restore failed.' }
    dotnet build -c Release --no-restore
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
    dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -o artifacts/app
    if ($LASTEXITCODE -ne 0) { throw 'Publishing failed.' }
    $executable = Join-Path $PSScriptRoot 'artifacts\app\TaskManager.exe'
    $reportPath = Join-Path $PSScriptRoot 'artifacts\release-self-test.json'
    $test = Start-Process -FilePath $executable -ArgumentList "--self-test --output `"$reportPath`"" -WindowStyle Hidden -Wait -PassThru
    if ($test.ExitCode -ne 0) { throw "Published self-test failed with exit code $($test.ExitCode)." }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if (-not $report.passed) { throw 'Published self-test report did not pass.' }
    Write-Output "Published $executable; $($report.checks.Count) checks passed."
}
finally { Pop-Location }
