param(
    [Parameter(Mandatory=$true)][string]$PackageDir,
    [string]$ExecutableName = 'StarbloomSurvivor',
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory,
    [switch]$UseWarp
)
# This acceptance harness is external to the player package. PowerShell is a
# Windows test harness dependency, never a gameplay dependency.
$ErrorActionPreference = 'Stop'
$result = [ordered]@{schema=1;status='FAIL';environment='clean Windows guest';checks=@();runs=@()}
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$process = $null
try {
    if (@(Get-NetAdapter | Where-Object Status -eq 'Up').Count) { throw 'Guest must have no active network adapter. Disable networking in the VM/Sandbox configuration.' }
    foreach ($tool in @('dotnet.exe','python.exe','python3.exe','cmake.exe','cl.exe','msbuild.exe','pwsh.exe')) {
        if (Get-Command $tool -ErrorAction SilentlyContinue) { throw "Development dependency exists in guest: $tool" }
    }
    foreach ($directory in @("$env:ProgramFiles/dotnet", "$env:ProgramFiles/Microsoft Visual Studio", "$env:USERPROFILE/.nuget/packages", 'C:/VulkanSDK')) {
        if (Test-Path -LiteralPath $directory) { throw "Guest is not clean: $directory" }
    }
    $result.checks += 'Offline and no SDK/build tools'
    $local = Join-Path $env:TEMP ('HEngine 中文 空格 ' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $local | Out-Null
    Copy-Item -Path (Join-Path $PackageDir '*') -Destination $local -Recurse
    $forbidden = @(Get-ChildItem -LiteralPath $local -File -Recurse | Where-Object {
        $_.Extension -in @('.cs','.csproj','.ps1','.pdb') -or $_.Name -match 'ScriptHost|RegistryGenerator|Microsoft.CodeAnalysis|\.Tests\.'
    })
    if ($forbidden.Count) { throw ('Development artifacts leaked into player: ' + ($forbidden.Name -join ', ')) }
    $result.checks += 'Unicode and space path; no compiler/editor/source artifacts'
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $env:DOTNET_MULTILEVEL_LOOKUP = '0'
    Remove-Item Env:DOTNET_ROOT,Env:PYTHONHOME,Env:PYTHONPATH -ErrorAction SilentlyContinue
    for ($round=0; $round -lt 2; ++$round) {
        $stdout = Join-Path $evidence "run-$round.stdout.log"
        $stderr = Join-Path $evidence "run-$round.stderr.log"
        $arguments = @('--graphics-api=dx12','--strict-graphics-api','--exit-after-frames=180','--fixed-delta=0.0166667')
        if ($UseWarp) { $arguments += '--warp' }
        $process = Start-Process -FilePath (Join-Path $local ($ExecutableName + '.exe')) -ArgumentList $arguments `
            -WorkingDirectory $local -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null = $process.Handle
        $modules = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $timer = [Diagnostics.Stopwatch]::StartNew()
        do {
            try { $process.Refresh(); foreach ($module in $process.Modules) { $null = $modules.Add($module.FileName) } }
            catch [InvalidOperationException] { } # Exit may race a module snapshot.
            $exited = $process.WaitForExit(20)
            if ($timer.Elapsed.TotalSeconds -gt 180) { throw 'Player timeout.' }
        } while (!$exited)
        $process.WaitForExit(); $process.Refresh()
        $modulePaths = @($modules | Sort-Object)
        $modulePaths | ConvertTo-Json | Set-Content (Join-Path $evidence "run-$round.modules.json") -Encoding UTF8
        if ($process.ExitCode -ne 0) { throw "Player failed with exit code $($process.ExitCode)" }
        foreach ($path in $modulePaths) {
            if (!$path.StartsWith($local + '\',[StringComparison]::OrdinalIgnoreCase) -and
                !$path.StartsWith($env:SystemRoot + '\',[StringComparison]::OrdinalIgnoreCase)) { throw "External loaded module: $path" }
        }
        if (!@($modulePaths | Where-Object { $_ -like "$local\*\coreclr.dll" }).Count) { throw 'No app-local CoreCLR observed; runtime closure unproven.' }
        $log = Get-Content -LiteralPath $stdout -Raw
        if ($log -notmatch 'Workflow Initialize' -or $log -notmatch 'Workflow save written=' -or
            ($round -eq 1 -and $log -notmatch 'Workflow save restored=') -or $log -match '\[Script\].*(kind=|failed)|Fatal|Unhandled exception') {
            throw 'Workflow execution/save/restart contract failed; inspect stdout.'
        }
        if ((Get-Item -LiteralPath $stderr).Length) { throw 'Unexpected player stderr.' }
        $result.runs += [ordered]@{round=$round;exitCode=$process.ExitCode;modules=$modulePaths;status='PASS'}
        $process.Dispose(); $process = $null
    }
    $result.checks += 'C# sample, app-local runtime, save and restart'
    $result.status = 'PASS'
} catch { $result.error = $_.Exception.Message }
finally {
    if ($process) {
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
    $result | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'result.json') -Encoding UTF8
}
if ($result.status -ne 'PASS') { throw $result.error }
