param(
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$Profile = '',
    [ValidateSet('Build','Check')][string]$Action = 'Build'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'OutputDirectory.ps1')
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repo 'Tools/Dependencies/Environment.ps1')
Import-HEDependencyEnvironment
. (Join-Path $repo 'Tools/Agent/Storage.ps1')
$projectRoot = (Resolve-Path -LiteralPath $Project).Path
if ([IO.Path]::GetExtension($projectRoot) -eq '.heproject') { $projectRoot = Split-Path $projectRoot }
if (!(Test-Path -LiteralPath (Join-Path $projectRoot 'Project.heproject'))) { throw 'Project.heproject is missing.' }
$config = Get-Content -LiteralPath (Join-Path $projectRoot 'Config/BuildProfiles.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($config.schema -ne 1) { throw 'Unsupported build profile schema.' }
if (!$Profile) { $Profile = $config.activeProfile }
$profiles = @($config.profiles | Where-Object id -eq $Profile)
if ($profiles.Count -ne 1 -or $Profile -notmatch '^[a-zA-Z0-9_-]+$') { throw 'Choose one valid, unique build profile.' }
$selected = $profiles[0]
if ($selected.platform -eq 'macOS') {
    if (!$IsMacOS) { throw 'The macOS export backend requires an Apple Silicon macOS host.' }
    $python = if ($env:HE_BUILD_PYTHON) { $env:HE_BUILD_PYTHON } else { (Get-Command python3 -ErrorAction Stop).Source }
    & $python (Join-Path $repo 'Tools/macOS/Publish.py') --project $projectRoot --profile $Profile --action $Action
    exit $LASTEXITCODE
}
$sha = [Security.Cryptography.SHA256]::Create()
try { $key = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($projectRoot.ToLowerInvariant())))).Replace('-','').Substring(0,12) } finally { $sha.Dispose() }
$work = Join-Path $repo "out/agent/build-publish/$key/$Profile"
New-Item -ItemType Directory -Force $work | Out-Null
$log = Join-Path $work 'build.log'
$receipt = Join-Path $projectRoot "Saved/Build/$Profile.json"
New-Item -ItemType Directory -Force (Split-Path $receipt) | Out-Null
$result = [ordered]@{schema=1;profile=$Profile;platform=$selected.platform;action=$Action;success=$false;status='Running';output='';log=$log;error='';started=[DateTime]::UtcNow.ToString('o')}
function Save-Receipt { $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receipt -Encoding UTF8 }
function Resolve-ProjectFile([string]$relative) {
    if (!$relative -or [IO.Path]::IsPathRooted($relative)) { throw 'Project paths must be relative.' }
    $path = [IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
    if (!$path.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Project path escapes its root.' }
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing project file: $relative" }
    return $path
}
function Invoke-BuildTool([string]$tool, [string[]]$arguments) {
    $logWriter.WriteLine("Running: $tool")
    $previous = $ErrorActionPreference
    $previousEngine = $env:HE_ENGINE_ROOT
    try { $env:HE_ENGINE_ROOT = $repo; $ErrorActionPreference = 'Continue'; & $tool @arguments 2>&1 | ForEach-Object { $logWriter.WriteLine($_.ToString()) }; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $previous; $env:HE_ENGINE_ROOT = $previousEngine }
    if ($code -ne 0) { throw "Build tool failed ($code). See $log" }
}
function Read-SharedLog {
    $stream=[IO.File]::Open($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=New-Object IO.StreamReader($stream,[Text.Encoding]::UTF8)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Find-Python {
    if ($env:HE_BUILD_PYTHON -and (Test-Path -LiteralPath $env:HE_BUILD_PYTHON)) { return $env:HE_BUILD_PYTHON }
    if (Get-Command py.exe -ErrorAction SilentlyContinue) {
        $found = & py.exe -3.11 -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $found)) { return $found }
    }
    foreach ($candidate in @((Get-Command python.exe -ErrorAction SilentlyContinue).Source, (Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'))) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    throw 'Install Python 3.11+ or configure HE_BUILD_PYTHON.'
}
$lock = $null
$logWriter = $null
$storageRun = $null
try {
    $lock = [IO.File]::Open((Join-Path $work 'build.lock'), 'OpenOrCreate','ReadWrite','None')
    $logStream=[IO.File]::Open($log,[IO.FileMode]::Create,[IO.FileAccess]::Write,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $logWriter=New-Object IO.StreamWriter($logStream,(New-Object Text.UTF8Encoding($false)))
    $logWriter.AutoFlush=$true
    Save-Receipt
    $logWriter.WriteLine("Build profile: $Profile ($($selected.platform))")
    $binaryDistribution = Test-Path -LiteralPath (Join-Path $repo 'HEngine.distribution.json')
    $binaryWindows = $selected.platform -eq 'Windows' -and $binaryDistribution
    if ($selected.unavailableReason) { throw $selected.unavailableReason }
    if ($selected.platform -notin @('Web','Windows','WeChat','Android')) { throw "No registered build backend for $($selected.platform)." }
    if ($selected.configuration -notin @('Debug','Release')) { throw 'Configuration must be Debug or Release.' }
    Invoke-BuildTool (Find-Python) @('-X','utf8',(Join-Path $repo 'Tools/Build/Preflight.py'),'--project',$projectRoot,
        '--profile',$Profile,'--output',(Join-Path $projectRoot "Saved/Build/$Profile-preflight.json"))
    if (Test-Path -LiteralPath (Join-Path $projectRoot 'Config/SharedContent.json')) {
        Invoke-BuildTool (Find-Python) @((Join-Path $repo 'Tools/Build/SharedContent.py'),$projectRoot)
    }
    if (Test-Path -LiteralPath (Join-Path $projectRoot 'Config/SharedUI.json')) {
        $sharedUiNode = (Get-Command node.exe -ErrorAction SilentlyContinue).Source
        if (!$sharedUiNode) { throw 'Shared UI export requires Node.js.' }
        Invoke-BuildTool $sharedUiNode @((Join-Path $repo 'Tools/Build/CompileSharedUI.cjs'),$projectRoot,'--check')
        if ($Action -eq 'Build') { Invoke-BuildTool $sharedUiNode @((Join-Path $repo 'Tools/Build/CompileSharedUI.cjs'),$projectRoot) }
    }
    $quality = if ($selected.quality) { $selected.quality } elseif ($selected.platform -in @('Windows','Android')) { 'Desktop' } else { 'Portable' }
    if ($quality -notin @('Desktop','Portable')) { throw 'Quality must be Desktop or Portable.' }
    if ($selected.platform -eq 'Windows' -and $selected.configuration -ne 'Release') { throw 'Windows shipping packages require Release.' }
    $output = Resolve-HEBuildOutput $projectRoot $selected.output
    Assert-HEBuildOutputOwner $output $projectRoot $Profile
    $result.output = $output
    if ($selected.platform -in @('Web','WeChat')) {
        $managed = Resolve-ProjectFile $selected.managedProject
        $exporter = Resolve-ProjectFile $selected.contentExporter
        $python = Find-Python
        $contentExporterTool = $python
        switch ([IO.Path]::GetExtension($exporter).ToLowerInvariant()) {
            '.py' { }
            '.cjs' {
                $contentExporterTool = $env:HE_SCRIPT_NODE
                if (!$contentExporterTool) { $contentExporterTool = (Get-Command node.exe -ErrorAction SilentlyContinue).Source }
                if (!$contentExporterTool -or !(Test-Path -LiteralPath $contentExporterTool)) { throw 'Node.js content export requires Node.js or HE_SCRIPT_NODE.' }
            }
            default { throw 'Content exporters must be Python (.py) or Node.js (.cjs) scripts.' }
        }
        if (!(Get-Command dotnet.exe -ErrorAction SilentlyContinue)) { throw 'Install the .NET SDK listed in Engine/Managed/global.json and the wasm-tools workload.' }
        Push-Location (Join-Path $repo 'Engine/Managed')
        try {
            Invoke-BuildTool 'dotnet' @('--info'); Invoke-BuildTool 'dotnet' @('workload','list')
            if ((Read-SharedLog) -notmatch 'wasm-tools') { throw 'Install the .NET wasm-tools workload: dotnet workload install wasm-tools (from Engine/Managed).' }
            Invoke-BuildTool $python @('-c','import sys; assert sys.version_info >= (3,11), "Python 3.11+ required"')
        } finally { Pop-Location }
        if ($selected.platform -eq 'WeChat') {
            $platformExporter = Resolve-ProjectFile $selected.platformExporter
            $node = $env:HE_SCRIPT_NODE
            if (!$node) { $node = (Get-Command node.exe -ErrorAction SilentlyContinue).Source }
            if (!$node) { $node = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' }
            if (!(Test-Path -LiteralPath $node)) { throw 'Install Node.js or set HE_SCRIPT_NODE.' }
            $assetPython = $python
            & $assetPython -c "import importlib.util,sys;sys.exit(0 if importlib.util.find_spec('PIL') and importlib.util.find_spec('audioop') else 1)"
            if ($LASTEXITCODE -ne 0) {
                $assetPython = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
                if (!(Test-Path -LiteralPath $assetPython)) { throw 'Portable asset export requires Python 3.11/3.12 and Pillow: py -3.11 -m pip install Pillow' }
            }
            Invoke-BuildTool $assetPython @('-c','import PIL, audioop')
        }
    }
    elseif ($selected.platform -eq 'Android') {
        $androidConfig = if ($selected.androidConfig) { $selected.androidConfig } else { 'Config/Android.json' }
        $null = Resolve-ProjectFile $androidConfig
        Invoke-BuildTool 'powershell.exe' @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $repo 'Tools/Android/Build.ps1'),'-Project',$projectRoot,'-Profile',$Profile,'-Config',$androidConfig,'-Action','Check','-Quality',$quality)
    }
    elseif ($binaryWindows) {
        Invoke-BuildTool 'powershell.exe' @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$repo/Tools/Distribution/Export-Windows.ps1",'-Project',$projectRoot,'-Profile',$Profile,'-Action','Check')
    }
    else {
        . (Join-Path $repo 'Tools/Agent/Toolchain.ps1')
        $null = Get-HeVisualStudio
    }
    if ($Action -eq 'Check') { $result.success=$true; $result.status='Ready'; Save-Receipt; Write-Output "CHECK PASS: $Profile"; return }
    $run = Join-Path (Join-Path $work 'content') ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-ffff'))
    if ($binaryDistribution -and $selected.platform -in @('Windows','Android')) {
        # Native content paths include shader variants and managed intermediates.
        # Keep each project/run isolated without repeating the long log hierarchy.
        $run = Join-Path $repo ("out/agent/content/$key/" + [guid]::NewGuid().ToString('N').Substring(0,12))
    }
    $stage = Join-Path $run 'site'
    New-Item -ItemType Directory -Force $stage | Out-Null
    $storageRun = Start-HEStorageRun -Project $projectRoot -Workflow "publish-$Profile" -Kind evidence -Paths @($run) -Locks @((Join-Path $work 'build.lock'))
    if ($selected.platform -in @('Web','WeChat')) {
        Push-Location (Join-Path $repo 'Engine/Managed')
        $managedArguments = @('publish',$managed,'-c',$selected.configuration,"-p:HEngineRoot=$repo","-p:HEPlatform=$($selected.platform)","-p:HEQuality=$quality",'--artifacts-path',(Join-Path $work 'artifacts'),'-o',(Join-Path $run 'managed'))
        if ($selected.platform -eq 'WeChat') { $managedArguments += @('-p:PublishTrimmed=true','-p:InvariantGlobalization=true','-p:WasmEnableExceptionHandling=false','-p:WasmEnableSIMD=false','-p:WasmEnableThreads=false','-p:WasmBuildNative=true') }
        try { Invoke-BuildTool 'dotnet' $managedArguments } finally { Pop-Location }
        if ($selected.platform -eq 'Web') {
            Invoke-BuildTool $contentExporterTool @($exporter,'--output',$stage)
            Copy-Item -LiteralPath (Join-Path $run 'managed/wwwroot/_framework') -Destination $stage -Recurse
            Copy-Item -LiteralPath (Join-Path $repo 'Engine/Source/Runtime/Platform/Web') -Destination (Join-Path $stage 'engine') -Recurse
            foreach ($required in @('index.html','main.js','_framework/dotnet.js')) {
                if (!(Test-Path -LiteralPath (Join-Path $stage $required))) { throw "Web export is incomplete: $required" }
            }
        } else {
            Invoke-BuildTool $node @((Join-Path $repo 'Tools/Build/ManagedWeChatCompatibility.cjs'),(Join-Path $run 'managed/wwwroot/_framework'))
            $content = Join-Path $run 'content'
            Invoke-BuildTool $contentExporterTool @($exporter,'--output',$content)
            if ($quality -eq 'Portable') { Invoke-BuildTool $assetPython @((Join-Path $repo 'Tools/Build/OptimizePortableAssets.py'),$content) }
            Invoke-BuildTool $node @($platformExporter,$content,(Join-Path $run 'managed/wwwroot/_framework'),$stage)
            Invoke-BuildTool $node @((Join-Path $repo 'Tools/Build/WeChatJavaScript.cjs'),$stage)
            foreach ($required in @('game.js','game.json','project.config.json','game-manifest.json')) {
                if (!(Test-Path -LiteralPath (Join-Path $stage $required))) { throw "WeChat export is incomplete: $required" }
            }
        }
    }
    elseif ($selected.platform -eq 'Android') {
        $androidWork = if ($binaryDistribution) { Join-Path $run 'a' } else { Join-Path $work 'android' }
        Invoke-BuildTool 'powershell.exe' @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $repo 'Tools/Android/Build.ps1'),'-Project',$projectRoot,'-Profile',$Profile,'-Config',$androidConfig,'-Quality',$quality,'-WorkDirectory',$androidWork,'-OutputDirectory',$stage)
        if (!(Test-Path -LiteralPath (Join-Path $stage 'android-build.json'))) { throw 'Android build did not produce a verified artifact receipt.' }
    }
    elseif ($binaryWindows) {
        Invoke-BuildTool 'powershell.exe' @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$repo/Tools/Distribution/Export-Windows.ps1",'-Project',$projectRoot,'-Profile',$Profile,'-OutputDirectory',$stage,'-WorkDirectory',"$run/native")
    }
    else {
        # Same authoritative shipping workflow as Win-PackGame.bat / hectl package.
        $packageArguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $repo 'Tools/Agent/Package.ps1'),'-Project',$projectRoot,'-ScriptQuality',$quality)
        if ($selected.gameConfig) { $null = Resolve-ProjectFile $selected.gameConfig; $packageArguments += @('-GameConfig',$selected.gameConfig) }
        if ($selected.pluginConfig) { $packageArguments += @('-PluginConfig',(Resolve-ProjectFile $selected.pluginConfig)) }
        Invoke-BuildTool 'powershell.exe' $packageArguments
        $line = (Read-SharedLog) -split "`n" | Select-String '^Packaged game: (.+)$' | Select-Object -Last 1
        if (!$line) { throw 'Shipping workflow did not report a verified package.' }
        $package = Split-Path $line.Matches[0].Groups[1].Value.Trim()
        foreach ($required in @('Game.pak','Game.manifest.json')) { if (!(Test-Path -LiteralPath (Join-Path $package $required))) { throw "Shipping package incomplete: $required" } }
        Get-ChildItem -LiteralPath $package -Force | Copy-Item -Destination $stage -Recurse
    }
    [ordered]@{schema=1;project=$projectRoot;profile=$Profile;platform=$selected.platform;quality=$quality;configuration=$selected.configuration;built=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage '.he-build.json') -Encoding UTF8
    Publish-HEBuildOutput $stage $output $projectRoot $Profile
    $result.success=$true; $result.status='Succeeded'; Save-Receipt
    Write-Output "BUILD PASS: $output"
}
catch { $result.error=$_.Exception.Message; $result.status='Failed'; if ($lock) { Save-Receipt; if($logWriter){$logWriter.WriteLine($result.error)} }; Write-Error $result.error; exit 1 }
finally {
    if ($logWriter) { $logWriter.Dispose() }
    if ($lock) { $lock.Dispose() }
    $storageState = if ($result.success) { 'success' } else { 'failed' }
    Complete-HEStorageRun -Run $storageRun -State $storageState -NoAuto
}
