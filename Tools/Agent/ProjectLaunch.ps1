param([Parameter(Mandatory=$true)][string]$Project,
      [Parameter(Mandatory=$true)][string]$Log, [switch]$Check,
      [switch]$UseWarp, [ValidateRange(1,10000)][int]$CheckFrames = 2000,
      [ValidateRange(5,300)][int]$StartupTimeoutSeconds = 120)
$ErrorActionPreference = 'Stop'
if ($UseWarp -and !$Check) { throw 'UseWarp is reserved for an owned bounded Editor check.' }
. (Join-Path $PSScriptRoot 'Project.ps1')
. (Join-Path $PSScriptRoot 'Storage.ps1')
. (Join-Path $PSScriptRoot 'Process.ps1')
$repositoryRoot = Split-Path (Split-Path $PSScriptRoot)
$Log = [IO.Path]::GetFullPath($Log)
$null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Log))
[IO.File]::WriteAllText($Log, "[ProjectManager] Preparing $Project`r`n", [Text.UTF8Encoding]::new($true))
$storageRun = $null
$storageState = 'failed'
$editor = $null
$editorJob = $null
$released = $false
$launchLock = $null
$stdout = $Log + '.editor.stdout.log'
$stderr = $Log + '.editor.stderr.log'
Add-Type -Path (Join-Path $PSScriptRoot 'ProjectProcess.cs')
function Invoke-ProjectBuild([string]$scriptPath, [string[]]$parameters) {
    $quoted = @("'" + $scriptPath.Replace("'", "''") + "'") + @($parameters | ForEach-Object {
        if ($_ -match '^-[A-Za-z][A-Za-z0-9]*$') { $_ } else { "'" + $_.Replace("'", "''") + "'" }
    })
    $command = "[Console]::OutputEncoding = [Text.UTF8Encoding]::new(`$false); `$OutputEncoding = [Console]::OutputEncoding; `$ErrorActionPreference = 'Stop'; `$ProgressPreference = 'SilentlyContinue'; try { & " +
        ($quoted -join ' ') + '; exit $LASTEXITCODE } catch { [Console]::Error.WriteLine($_.ToString()); exit 1 }'
    # -EncodedCommand serializes Write-Host information records as CLIXML on
    # Windows PowerShell 5. A BOM-marked script keeps all output human-readable.
    $bootstrapPath = $Log + '.build.ps1'
    [IO.File]::WriteAllText($bootstrapPath, $command, [Text.UTF8Encoding]::new($true))
    return [HEProjectProcess]::Run('powershell.exe', "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$bootstrapPath`"", $repositoryRoot, $Log)
}
function Publish-State([string]$state, [string]$message = '') {
    $value = [ordered]@{SchemaVersion=1; State=$state; Message=$message; Project=$Project; Stdout=$stdout; Stderr=$stderr}
    if ($editor) {
        $value.ProcessId = $editor.Id
        $value.ProcessStartTicks = $editorStartTicks
    }
    Write-HEWorkspace ($Log + '.state.json') $value
}
try {
    Publish-State 'building'
    $info = Resolve-HEProject $Project $repositoryRoot
    $lockRoot = Join-Path $repositoryRoot 'out/agent/projects/launch-locks'
    $null = [IO.Directory]::CreateDirectory($lockRoot)
    try { $launchLock = [IO.File]::Open((Join-Path $lockRoot ($info.CacheKey + '.lock')), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { throw 'This project is already being opened by another Project Manager. Wait for that attempt to finish.' }
    $buildRoot = if ($info.Root -ieq (Join-Path $repositoryRoot 'Project')) {
        Join-Path $repositoryRoot 'out/agent/editor-relwithdebinfo'
    } else { Join-Path $repositoryRoot "out/agent/projects/$($info.CacheKey)/editor-relwithdebinfo" }
    $executable = Join-Path $buildRoot 'bin/HEngineEditor.exe'
    $portable = Test-Path -LiteralPath (Join-Path $repositoryRoot 'HEngine.distribution.json')
    if ($portable) { $executable = Join-Path $repositoryRoot 'HEngineEditor.exe' }
    $runningEditor = if (!$portable) { Get-AgentArtifactProcess -ArtifactPath $executable -ProcessName 'HEngineEditor' } else { $null }
    if ($runningEditor) {
        $runningId = $runningEditor.Id
        $runningEditor.Dispose()
        throw "This project's Editor is already running (pid=$runningId). Continue in that window, or close it before rebuilding."
    }
    $storageRun = Start-HEStorageRun -Project $info.Root -ProjectId $info.ProjectId -Workflow 'project-open' -Paths @($Log, $stdout, $stderr, ($Log + '.state.json'), ($Log + '.build.ps1')) -Outer
    # Native stderr warnings must not override the actual compiler exit code.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        if ($portable) {
            if (!(Test-Path -LiteralPath $executable)) { throw 'HEngineEditor.exe is missing from this installation.' }
            $managedProject = Join-Path $info.Root 'Scripts/CSharp/GameScripts.csproj'
            $managedOutput = Join-Path $info.Root 'Saved/Scripting/CSharp'
            $prebuilt = Join-Path $info.Root 'Scripts/CSharp/Prebuilt/build.json'
            if ((Test-Path -LiteralPath $managedProject) -and !(Test-Path -LiteralPath $prebuilt) -and
                @(Get-ChildItem -LiteralPath (Split-Path $managedProject) -Filter '*.cs' -Recurse -File | Where-Object FullName -NotMatch '[\\/](bin|obj)[\\/]').Count -gt 0) {
                $managedExit = Invoke-ProjectBuild "$repositoryRoot/Tools/Scripting/BuildManaged.ps1" @(
                    '-Project',$info.Root,'-WorkDirectory',(Join-Path $info.Root 'Saved/Scripting/Build'),'-OutputDirectory',$managedOutput,'-EditorHost')
                if ($managedExit -ne 0) { throw "C# compilation failed. Install .NET SDK 10.0.302 and this project's build dependencies; see $Log" }
            }
            $buildExit = 0
        } else {
            $buildExit = Invoke-ProjectBuild (Join-Path $PSScriptRoot 'hectl.ps1') @('editor','build','-Project',$info.Root)
        }
    }
    finally { $ErrorActionPreference = $previousPreference }
    if ($buildExit -ne 0) { throw "Editor build failed (exit $buildExit). See $Log" }
    Publish-State 'starting'
    $launchParameters = @{FilePath=$executable; WorkingDirectory=$info.Root; PassThru=$true; RedirectStandardOutput=$stdout; RedirectStandardError=$stderr; ArgumentList=@('--project="' + $info.Root + '"')}
    if ($UseWarp) { $launchParameters.ArgumentList += @('--graphics-api=dx12','--strict-graphics-api','--warp','--resolution=1280x720') }
    if ($Check) {
        $launchParameters.WindowStyle = 'Hidden'
        # Same readiness handshake, with an owned bounded Editor for validation.
        $editorJob = New-AgentProcessJob
        # Leave enough frames for the external 100 ms readiness poll, including
        # empty projects that can render thousands of frames per second.
        $launchParameters.ArgumentList += "--exit-after-frames=$CheckFrames"
    }
    $editor = Start-Process @launchParameters
    $editorStartTicks = $editor.StartTime.ToUniversalTime().Ticks
    if ($editorJob) { Add-AgentProcessToJob -Job $editorJob -Process $editor }
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    $ready = $false
    $windowReady = $false
    $closeRequested = $false
    do {
        $editor.Refresh()
        if (Test-Path -LiteralPath $stdout) {
            $tail = Get-Content -LiteralPath $stdout -Encoding UTF8 -Tail 256
            if ($tail | Select-String -SimpleMatch '[ProjectManager] Editor startup failed:') { throw "Editor could not load its initial scene. See $stdout" }
            $ready = $ready -or [bool]($tail | Select-String -SimpleMatch '[ProjectManager] Editor ready')
        }
        if ($Check -and $ready -and $editor.HasExited) {
            if ($editor.ExitCode -ne 0) { throw "Editor validation failed (exit $($editor.ExitCode)). See $stderr" }
            if (-not $windowReady) { throw "Editor exited without confirming a responsive window. See $stdout" }
            Publish-State 'validated'
            $storageState = 'success'
            exit 0
        }
        if ($editor.HasExited) { throw "Editor exited before becoming usable (exit $($editor.ExitCode)). See $stdout and $stderr" }
        $windowReady = $windowReady -or ($ready -and $editor.MainWindowHandle -ne [IntPtr]::Zero -and $editor.Responding)
        if ($Check -and $windowReady -and !$closeRequested) {
            # A WARP host need not render thousands of idle frames to prove startup.
            # Close only this owned Editor, then require its normal zero exit above.
            $closeRequested = $true
            try {
                if (!$editor.CloseMainWindow() -and !$editor.HasExited) { throw 'Owned Editor did not accept a normal close request.' }
            } catch {
                if (!$editor.HasExited) { throw }
            }
        }
        if (-not $Check -and $windowReady) {
            Publish-State 'ready'
            $released = $true
            $storageState = 'success'
            exit 0
        }
        if ([DateTime]::UtcNow -gt $deadline) { throw "Editor startup timed out after $StartupTimeoutSeconds seconds. See $stdout and $stderr" }
        Start-Sleep -Milliseconds 100
    } while ($true)
}
catch {
    $failureMessage = $_.Exception.Message
    $_ | Out-String | Add-Content -LiteralPath $Log -Encoding UTF8
    Publish-State 'failed' $failureMessage
    exit 1
}
finally {
    # Only the Editor created by this attempt is retired on startup failure.
    if ($editor -and -not $released) {
        if (-not $editor.HasExited) { Stop-AgentOwnedProcess -Process $editor -Description 'Project Manager startup' }
    }
    if ($editorJob) { Close-AgentProcessJob -Job $editorJob }
    if ($editor) { $editor.Dispose() }
    if ($launchLock) { $launchLock.Dispose() }
    if ($storageRun) { Complete-HEStorageRun -Run $storageRun -State $storageState -NoAuto }
}
