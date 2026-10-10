[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$benchmarkScript = Join-Path (Split-Path -Parent $PSScriptRoot) 'benchmark.ps1'
$expectedNames = @('self-test', 'components', 'sampler', 'all-tabs', 'interaction', 'idle', 'process-sort-spam', 'details-sort-spam', 'tab-spam', 'dark-tab-spam', 'high-speed', 'low-speed', 'paused', 'minimized')
$expectedNames += 0..6 | ForEach-Object { "tab-$_" }

function Assert-Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-Scenario {
    param([string]$Name, [AllowNull()]$Payload, [string]$ExpectedError = '', [int]$ExitCode = 0, [switch]$Timeout)

    $output = Join-Path $fixtureRoot $Name
    New-Item -ItemType Directory -Path $output | Out-Null
    foreach ($caseName in @($expectedNames) + @('suite')) {
        [IO.File]::WriteAllText((Join-Path $output "$caseName.json"), '{"passed":true,"generation":"stale"}')
    }
    $mockState = @{
        Output = $output; Payload = $Payload; ExitCode = $ExitCode; Timeout = [bool]$Timeout
        Processes = [Collections.Generic.List[object]]::new()
    }

    # This function is visible only to this invocation and the real script's child scope.
    # Its process objects never launch, wait for or terminate operating-system processes.
    function Start-Process {
        param([string]$FilePath, [string]$ArgumentList, [string]$WindowStyle, [switch]$PassThru)
        Assert-Check ($FilePath -eq $fixtureExecutable -and $WindowStyle -eq 'Hidden' -and $PassThru) 'Unexpected diagnostic process request.'
        Assert-Check ($ArgumentList -match ' --output "([^"]+)"$') 'The benchmark output path must be quoted.'
        $reportPath = $Matches[1]
        Assert-Check ((Split-Path -Parent $reportPath) -eq $mockState.Output) 'The mock may write only its owned report directory.'
        $caseName = [IO.Path]::GetFileNameWithoutExtension($reportPath)
        Assert-Check ($caseName -eq $expectedNames[$mockState.Processes.Count]) 'Unexpected benchmark case or order.'
        if ($mockState.Processes.Count -eq 0) {
            foreach ($oldName in @($expectedNames) + @('suite')) {
                Assert-Check (-not (Test-Path -LiteralPath (Join-Path $mockState.Output "$oldName.json"))) 'Old reports and suite must be removed before starting a new attempt.'
            }
        }
        Assert-Check (-not (Test-Path -LiteralPath $reportPath)) 'A stale report was left in place.'
        if ($null -ne $mockState.Payload) { [IO.File]::WriteAllText($reportPath, $mockState.Payload, [Text.UTF8Encoding]::new($false)) }
        $process = [pscustomobject]@{
            ExitCode = $mockState.ExitCode; Timeout = $mockState.Timeout; DisposeCalls = 0; KillCalls = 0
            Events = [Collections.Generic.List[string]]::new()
        }
        $process | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {
            param([int]$Milliseconds = -1)
            $this.Events.Add("wait:$Milliseconds")
            if ($Milliseconds -ge 0) { return -not $this.Timeout }
        }
        $process | Add-Member -MemberType ScriptMethod -Name Kill -Value { $this.KillCalls++; $this.Events.Add('kill') }
        $process | Add-Member -MemberType ScriptMethod -Name Dispose -Value { $this.DisposeCalls++; $this.Events.Add('dispose') }
        $mockState.Processes.Add($process)
        return $process
    }

    $caught = $null
    try { & $benchmarkScript -Seconds 5 -Executable $fixtureExecutable -OutputDirectory $output 6>$null }
    catch { $caught = $_ }
    $suitePath = Join-Path $output 'suite.json'
    if ($ExpectedError) {
        Assert-Check ($null -ne $caught) "$Name was accepted unexpectedly."
        Assert-Check ($caught.Exception.Message -like "*$ExpectedError*") "$Name returned the wrong error: $caught"
        Assert-Check ($mockState.Processes.Count -eq 1) "$Name should stop at its first failing case."
        Assert-Check (-not (Test-Path -LiteralPath $suitePath)) "$Name left a stale successful suite."
    }
    else {
        if ($null -ne $caught) { throw $caught }
        Assert-Check ($mockState.Processes.Count -eq $expectedNames.Count) 'The valid suite did not execute all 21 cases.'
        $suite = @(Get-Content -LiteralPath $suitePath -Raw | ConvertFrom-Json)
        Assert-Check ($suite.Count -eq $expectedNames.Count) 'The valid suite did not report all 21 cases.'
        for ($index = 0; $index -lt $suite.Count; $index++) {
            $item = $suite[$index]
            Assert-Check ($item.name -eq $expectedNames[$index] -and $item.passed -is [bool] -and $item.passed) 'The suite contains an incorrect case result.'
            Assert-Check ($item.report -eq (Join-Path $output ($item.name + '.json'))) 'The suite references the wrong report.'
        }
    }
    foreach ($process in $mockState.Processes) {
        Assert-Check ($process.DisposeCalls -eq 1) "$Name did not dispose its owned process exactly once."
        $expectedEvents = if ($Timeout) { 'wait:65000,kill,wait:-1,dispose' } else { 'wait:65000,dispose' }
        Assert-Check (($process.Events -join ',') -eq $expectedEvents) "$Name did not complete the expected owned-process cleanup."
    }
    foreach ($file in Get-ChildItem -LiteralPath $output -Filter '*.json' -File) {
        Assert-Check (-not (Get-Content -LiteralPath $file.FullName -Raw).Contains('"generation":"stale"')) "$Name retained an old report."
    }
    Write-Output "PASS $Name"
}

$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('TaskManager Benchmark Tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
try {
    $fixtureRoot = (Resolve-Path -LiteralPath $fixtureRoot).Path
    $fixtureExecutable = Join-Path $fixtureRoot 'owned fixture.exe'
    [IO.File]::WriteAllBytes($fixtureExecutable, [byte[]]@())
    Invoke-Scenario -Name 'no-fresh-output' -Payload $null -ExpectedError 'produced no fresh report'
    Invoke-Scenario -Name 'missing-passed' -Payload '{}' -ExpectedError 'did not report success'
    Invoke-Scenario -Name 'false-passed' -Payload '{"passed":false}' -ExpectedError 'did not report success'
    Invoke-Scenario -Name 'string-passed' -Payload '{"passed":"true"}' -ExpectedError 'did not report success'
    Invoke-Scenario -Name 'numeric-passed' -Payload '{"passed":1}' -ExpectedError 'did not report success'
    Invoke-Scenario -Name 'non-object-report' -Payload 'true' -ExpectedError 'did not report success'
    Invoke-Scenario -Name 'array-report' -Payload '[{"passed":true}]' -ExpectedError 'did not report success'
    Invoke-Scenario -Name 'explicit-error' -Payload '{"passed":true,"error":"owned fixture failure"}' -ExpectedError 'did not report success'
    Invoke-Scenario -Name 'nonzero-exit' -Payload '{"passed":true}' -ExitCode 9 -ExpectedError 'failed with exit code 9'
    Invoke-Scenario -Name 'timeout' -Payload '{"passed":true}' -Timeout -ExpectedError 'exceeded its deadline'
    Invoke-Scenario -Name 'valid-21-cases' -Payload '{"passed":true,"generation":"fresh"}'
}
finally { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force }
