param(
    [ValidateSet('all','none','python','csharp','both')][string]$Profile = 'all'
)
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$project = Join-Path $repository 'Project'
$work = Join-Path $repository ('out/agent/scripting/matrix-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force $work | Out-Null
. (Join-Path $repository 'Tools/Agent/Storage.ps1')
$run = Start-HEStorageRun -Project $project -Workflow 'script-plugin-matrix-pc' -Paths @($work) -Outer
$state = 'failed'
$results = @()
function Get-MatrixFileHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $hasher.Dispose(); $stream.Dispose() }
}
try {
    $profiles = if ($Profile -eq 'all') { @('none','python','csharp','both') } else { @($Profile) }
    foreach ($name in $profiles) {
        $python = $name -in @('python','both')
        $csharp = $name -in @('csharp','both')
        $configuration = Get-Content (Join-Path $project 'Config/Plugins.json') -Raw | ConvertFrom-Json
        foreach ($hostName in @('Editor','DevGame','Game','Tests','Tools','RenderSmoke')) {
            $configuration.'he.script.python'.$hostName = $python -and $hostName -in @('Editor','DevGame','Game','Tests')
            $configuration.'he.script.csharp'.$hostName = $csharp -and $hostName -in @('Editor','DevGame','Game','Tests')
        }
        $config = Join-Path $work "$name.json"
        $configuration | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $config -Encoding UTF8
        $scene = switch ($name) {
            none { 'ScriptNative' } python { 'ScriptPython' } csharp { 'ScriptTouch' } both { 'ScriptCoexist' }
        }
        # One dedicated cache, separate immutable output directory per profile/run.
        # Switching never modifies the user's Project/Config/Plugins.json and never
        # mistakes old DLLs in a previous profile's output for current deployment.
        $artifactVariant = "$name-" + (Split-Path $work -Leaf).Substring(7)
        $log = Join-Path $work "$name.log"
        $capture = Join-Path $work "$name.png"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repository 'Tools/Agent/Build.ps1') `
            -Target HGame -Configuration RelWithDebInfo -BuildVariant scripting-matrix `
            -ArtifactVariant $artifactVariant -PluginConfig $config -Run -Wait -TimeoutSeconds 60 `
            -ExitAfterFrames 30 -FixedDeltaSeconds 0.0166667 -Resolution 1280x720 -NoCursorCapture `
            -StartupScene "Assets/WeChatSample/$scene.he" -CaptureFrame 30 -CapturePath $capture *> $log
        if ($LASTEXITCODE -ne 0) { throw "PC matrix '$name' build/run failed; see $log" }
        $build = Join-Path $repository 'out/agent/game-scripting-matrix-relwithdebinfo'
        $output = Join-Path $build "bin/$artifactVariant"
        $commands = Get-Content (Join-Path $build 'compile_commands.json') -Raw | ConvertFrom-Json
        $nativeCommands = [string]::Join("`n", @($commands | ForEach-Object { $_.command }))
        $ninja = Get-Content (Join-Path $build 'build.ninja') -Raw
        if (-not $python -and ($nativeCommands -match '[/\\](pybind11|Python)[/\\](include|libs)' -or $ninja -match 'python311\.lib')) {
            throw "Disabled Python leaked into '$name' compile/link inputs."
        }
        if (-not $csharp -and ($nativeCommands -match '(CSharpBackend|CoreClrHost)\.cpp' -or $ninja -match 'HEngineCSharpManaged')) {
            throw "Disabled C# leaked into '$name' native/managed build inputs."
        }
        if ((Test-Path (Join-Path $output 'python311.dll')) -ne $python) { throw "Python deployment boundary differs in '$name'." }
        if ((Test-Path (Join-Path $output 'Scripting/CSharp/HEngine.ScriptRuntime.dll')) -ne $csharp) { throw "C# deployment boundary differs in '$name'." }
        $receipt = Get-Content $log -Raw
        $stdoutMatch = [regex]::Match($receipt, 'stdout: ([^;\r\n]+)')
        if (-not $stdoutMatch.Success) { throw "Missing owned runtime log for '$name'." }
        $stdout = Get-Content $stdoutMatch.Groups[1].Value -Raw
        Copy-Item -LiteralPath $stdoutMatch.Groups[1].Value -Destination (Join-Path $work "$name.stdout.log")
        if ($csharp -and $stdout -notmatch '\[C#\].*Initialize entity=') { throw "No real C# initialization in '$name'." }
        if ($python -and $stdout -notmatch '\[Python\] observer Initialize') { throw "No real Python initialization in '$name'." }
        if ($name -eq 'none' -and $stdout -notmatch '\[NativeScript\] TouchSample created speed=3 score=7') { throw 'Native gameplay did not initialize with both plugins off.' }
        if ($stdout -match '\[Script\].*(kind=|failed)|Fatal|Unhandled exception') { throw "Runtime diagnostic in '$name'; inspect the retained log." }
        if ($name -eq 'both' -and $stdout -notmatch '\[Python\] observed C# score=') { throw 'The two real runtimes did not cooperate in the game scene.' }
        $result = [ordered]@{profile=$name; python=$python; csharp=$csharp; build='PASS'; ownedRuntime='PASS'; dependencyBoundary='PASS';
            visualReview='NOT_PERFORMED_BY_THIS_SCRIPT'; shipping='NOT_TESTED'; artifact=$output; capture=$capture;
            executableSha256=(Get-MatrixFileHash (Join-Path $output 'StarbloomSurvivor.exe'));
            compileUnits=@($commands).Count}
        if ($csharp) { $result.managed = Get-Content (Join-Path $output 'Scripting/CSharp/build.json') -Raw | ConvertFrom-Json }
        $results += [pscustomobject]$result
        $results | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $work 'results.json') -Encoding UTF8
        Write-Host "PC matrix $name`: build-only PASS; owned runtime PASS; dependency boundary PASS; visual review pending."
    }
    $state = 'success'
    Write-Host "Evidence: $work"
}
finally { Complete-HEStorageRun -Run $run -State $state }
