[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable
)

$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$manifestTool = Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/mt.exe" -File | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $manifestTool) { throw 'Windows SDK manifest tool was not found.' }
$manifestPath = Join-Path ([System.IO.Path]::GetTempPath()) ('TaskManagerManifest-' + [guid]::NewGuid().ToString('N') + '.xml')
try {
    & $manifestTool.FullName -nologo "-inputresource:$Executable;#1" "-out:$manifestPath" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract the production execution manifest.' }
    [xml]$manifest = Get-Content -LiteralPath $manifestPath -Raw
    $execution = $manifest.SelectSingleNode("//*[local-name()='requestedExecutionLevel']")
    if (-not $execution -or $execution.level -ne 'requireAdministrator' -or $execution.uiAccess -ne 'false') { throw 'Production executable must require administrator elevation without UI-access privileges.' }
}
finally { if (Test-Path -LiteralPath $manifestPath) { Remove-Item -LiteralPath $manifestPath } }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable, '--self-test')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    try {
        $unexpected = [Diagnostics.Process]::Start($start)
        $unexpected.WaitForExit(10000) | Out-Null
        $unexpected.Dispose()
        throw 'Production executable started without administrator elevation.'
    }
    catch [System.ComponentModel.Win32Exception] {
        if ($_.Exception.NativeErrorCode -ne 740) { throw }
    }
    Write-Output 'Administrator manifest verified; Windows refused unelevated launch (740). Runtime command-rejection checks require an elevated test shell.'
}
$destination = Join-Path ([System.IO.Path]::GetTempPath()) ('TaskManagerProduction-' + [guid]::NewGuid().ToString('N'))
$commands = @('--self-test', '--test-child', '--component-benchmark', '--benchmark', '--ui-benchmark', '--startup-inventory', '--screenshots')
foreach ($command in $(if ($elevated) { $commands })) {
    $process = Start-Process -FilePath $Executable -ArgumentList "$command `"$destination`" --output `"$destination`"" -WindowStyle Hidden -PassThru
    try {
        if (-not $process.WaitForExit(10000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Production executable accepted $command or failed to exit promptly."
        }
        if ($process.ExitCode -ne 2) { throw "Production executable did not reject $command (exit $($process.ExitCode))." }
        if (Test-Path -LiteralPath $destination) { throw "Production executable wrote diagnostic output for $command." }
    }
    finally { $process.Dispose() }
}
$bytes = [System.IO.File]::ReadAllBytes($Executable)
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
$wideText = [System.Text.Encoding]::Unicode.GetString($bytes)
foreach ($marker in @('startup_fixture_created', 'cachedSort', 'keyboardPassed', 'tabSpamPassed', 'columnResize', 'scrollbar-rebuild-no-intermediate-paints', 'scrollbar-rebuild-explicit-capture', 'caption-immediate-theme-pixels', '--tab-spam', '--self-test', '--component-benchmark', '--ui-benchmark', '--screenshots', 'TaskManagerNativeTests')) {
    if ($text.Contains($marker) -or $wideText.Contains($marker)) { throw "Production executable still contains diagnostic marker: $marker" }
}
Write-Output "Production exclusion checks passed: diagnostic code markers absent; administrator manifest verified; runtime rejection checks executed: $elevated."
