[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$TestConfiguration = 'Debug'
)

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    cmake -S . -B artifacts/native-build -A x64 -DTASKMGR_DIAGNOSTICS=OFF
    if ($LASTEXITCODE -ne 0) { throw 'Native CMake configuration failed.' }
    cmake --build artifacts/native-build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Native release build failed.' }
    cmake -S . -B artifacts/native-tests -A x64 -DTASKMGR_DIAGNOSTICS=ON
    if ($LASTEXITCODE -ne 0) { throw 'Native developer CMake configuration failed.' }
    cmake --build artifacts/native-tests --config $TestConfiguration --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Native developer build failed.' }
    ctest --test-dir artifacts/native-tests -C $TestConfiguration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
    & ./tests/production.ps1 -Executable (Join-Path $PSScriptRoot 'artifacts\native-build\Release\TaskManager.exe')
    New-Item -ItemType Directory -Force artifacts/app | Out-Null
    Copy-Item -LiteralPath artifacts/native-build/Release/TaskManager.exe -Destination artifacts/app/TaskManager.exe
    $executable = Join-Path $PSScriptRoot 'artifacts\app\TaskManager.exe'
    if ((Get-FileHash -LiteralPath $executable).Hash -ne (Get-FileHash -LiteralPath 'artifacts/native-build/Release/TaskManager.exe').Hash) { throw 'Published executable differs from the verified production build.' }
    Write-Output "Published native C++ $executable; separate $TestConfiguration developer tests and production exclusion checks passed. No .NET runtime is required."
}
finally { Pop-Location }
