[CmdletBinding()]
param(
    [string[]]$Scenes = @(
        'Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he',
        'Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he'),
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi = 'DX12',
    [ValidatePattern('^[1-9][0-9]{0,3}x[1-9][0-9]{0,3}$')][string]$Resolution = '3840x2160',
    [ValidateRange(1,30)][int]$SwitchCycles = 3,
    [ValidateRange(1,120)][int]$SettleFrames = 30,
    [ValidateRange(30,300)][int]$TimeoutSeconds = 180,
    [switch]$IncludeHardwareRTGI,
    [switch]$IncludeParticles,
    [switch]$InjectCandidateFailure,
    [switch]$TransientTextureReuse,
    [switch]$SampleMemory,
    [string]$RenderSettingsPath = '',
    [ValidatePattern('^$|^[0-9a-fA-F]{64}$')][string]$ExistingBuildSha256 = '',
    [string]$ShaderCompilerPath = '',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$hectl = Join-Path $PSScriptRoot 'hectl.ps1'
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot ('out\agent\render-validation\{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'),$PID)
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory to retain each validation run.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$ownerTag = [guid]::NewGuid().ToString('N')
$results = [Collections.Generic.List[object]]::new()
$commands = [Collections.Generic.List[object]]::new()
$sequence = 0
$extraViewLive = $false
$memorySampler = $null
$sceneDirectory = $OutputDirectory
$expectedSize = $Resolution.Split('x') | ForEach-Object { [int]$_ }
$savedParticleEnvironment = @{}
$configurationPatch = @{}
if ($RenderSettingsPath) {
    $RenderSettingsPath = [IO.Path]::GetFullPath($RenderSettingsPath)
    $inputSettings = Get-Content -LiteralPath $RenderSettingsPath -Raw | ConvertFrom-Json
    foreach ($property in $inputSettings.PSObject.Properties) { $configurationPatch[$property.Name] = $property.Value }
}
$savedFailureEnvironment = [Environment]::GetEnvironmentVariable('HE_DEV_RENDER_FAIL_CANDIDATE_ONCE','Process')
$particleEnvironment = @{ HE_VFX_STRESS_MODE='1'; HE_VFX_STRESS_SCENARIO='gpu20k'; HE_VFX_STRESS_SEED='1806' }

function Write-Json([string]$path, $value) {
    [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 40), [Text.UTF8Encoding]::new($false))
}
function Invoke-HeCtl([string[]]$arguments, [string]$label, [bool]$parseJson = $true) {
    $script:sequence++
    $prefix = Join-Path $script:sceneDirectory ('{0:d4}-{1}' -f $script:sequence,$label)
    $fullArguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$hectl) + $arguments +
        @('-Configuration','RelWithDebInfo','-AgentOwnerTag',$ownerTag,'-TimeoutSeconds',"$TimeoutSeconds")
    $startedUtc = [DateTime]::UtcNow.ToString('o')
    $clock = [Diagnostics.Stopwatch]::StartNew()
    # A separate shell confines hectl's explicit exit to the child, allowing
    # this script's finally to always reclaim its owner-tagged HGame session.
    # Redirect to files before launching. Capturing a native shell through a
    # PowerShell pipeline can retain inherited pipe handles in the live game
    # and wait for that game to exit even after `agent start` has returned.
    $quotedArguments = @($fullArguments | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace([string]$_,
            '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' '
    $helper = Start-Process -FilePath powershell.exe -ArgumentList $quotedArguments `
        -RedirectStandardOutput ($prefix + '.stdout.log') `
        -RedirectStandardError ($prefix + '.stderr.log') -WindowStyle Hidden -PassThru
    try {
        # Cache the native process handle before it exits; Windows PowerShell's
        # Start-Process object otherwise can lose its queryable exit code.
        $null = $helper.Handle
        $helper.WaitForExit()
        $code = $helper.ExitCode
    }
    finally { $helper.Dispose() }
    $clock.Stop()
    $outputStream = [IO.File]::Open($prefix + '.stdout.log', [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $outputReader = [IO.StreamReader]::new($outputStream)
    try { $raw = $outputReader.ReadToEnd() }
    finally { $outputReader.Dispose() }
    $commands.Add(@{ program='powershell.exe'; arguments=$fullArguments; exitCode=$code;
        label=$label; startedUtc=$startedUtc; finishedUtc=[DateTime]::UtcNow.ToString('o');
        wallMs=$clock.Elapsed.TotalMilliseconds; output=$prefix+'.stdout.log' })
    if ($code -ne 0) { throw "hectl $label failed with exit $code; see $prefix.stdout.log" }
    if ($SampleMemory -and $label -eq 'start' -and $raw -match 'agent session: READY pid=(\d+)') {
        $ownedGame = [Diagnostics.Process]::GetProcessById([int]$Matches[1])
        try {
            Write-Json (Join-Path $OutputDirectory 'memory-sampler-game.json') @{
                processId=$ownedGame.Id; startTicks=$ownedGame.StartTime.ToUniversalTime().Ticks;
                executablePath=$ownedGame.MainModule.FileName
            }
        } finally { $ownedGame.Dispose() }
    }
    if (!$parseJson) { return }
    # agent calls emit route text followed by one JSON response; keep raw
    # output as evidence and require a parseable complete response.
    $jsonStart = $raw.IndexOf('{')
    if ($jsonStart -lt 0) { throw "No JSON response from $label" }
    $response = $raw.Substring($jsonStart) | ConvertFrom-Json
    if ($null -ne $response.ok -and !$response.ok) { throw "Agent $label rejected: $($response.error | ConvertTo-Json -Compress)" }
    if ($null -ne $response.result) {
        if ($null -ne $response.result.ok -and !$response.result.ok) { throw "Scene $label rejected: $($response.result.error | ConvertTo-Json -Compress)" }
        return $response.result
    }
    return $response
}
function Call-Agent([string]$method, [hashtable]$parameters = @{}) {
    $request = Join-Path $script:sceneDirectory ('request-{0:d4}.json' -f ($script:sequence + 1))
    Write-Json $request $parameters
    return Invoke-HeCtl @('agent','call','-Method',$method,'-RequestFile',$request) ($method.Replace('.','-'))
}
function Add-Result([string]$name,[string]$status,[string]$detail,$evidence = $null) {
    $results.Add(@{ scene=$script:currentScene; name=$name; status=$status;
        evidenceType='runtime-manual'; detail=$detail; evidence=$evidence })
}
function Save-Capture([string]$name, [int[]]$size = $expectedSize, $completedCapture = $null) {
    $capture = $(if ($null -ne $completedCapture) { $completedCapture } else { Call-Agent 'capture.frame' @{ filename = $name + '.png' } })
    $source = [string]$capture.path
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw 'Capture completion has no PNG file.' }
    $stream = [IO.File]::OpenRead($source)
    try { $header = [byte[]]::new(24); if ($stream.Read($header,0,24) -ne 24) { throw 'PNG header truncated.' } }
    finally { $stream.Dispose() }
    if ($header[0] -ne 137 -or $header[1] -ne 80 -or $header[2] -ne 78 -or $header[3] -ne 71) { throw 'Capture is not PNG.' }
    $width = [int]($header[16]*16777216 + $header[17]*65536 + $header[18]*256 + $header[19])
    $height = [int]($header[20]*16777216 + $header[21]*65536 + $header[22]*256 + $header[23])
    if ($width -ne $size[0] -or $height -ne $size[1]) { throw "Capture is ${width}x${height}, expected $($size[0])x$($size[1])." }
    $destination = Join-Path $script:sceneDirectory ($name + '.png')
    Copy-Item -LiteralPath $source -Destination $destination
    return @{ path=$destination; width=$width; height=$height; renderFrame=$capture.renderFrame;
        sha256=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash }
}
function Get-GraphPasses {
    $items = @(); $offset = 0; $snapshot = ''
    do {
        $parameters = @{ section='passes'; offset=$offset; limit=100 }
        if ($snapshot) { $parameters.snapshotId=$snapshot }
        $page = Call-Agent 'render.graph.snapshot' $parameters
        $snapshot = [string]$page.snapshotId
        $items += $page.items
        $offset = $page.nextOffset
    } while ($null -ne $offset)
    return @($items | Where-Object { !$_.culled })
}
function Apply-Configuration([string]$path,[string]$gi,[string]$name) {
    $settings = Call-Agent 'render.settings.get'
    $patch = $configurationPatch.Clone()
    $patch.Remove('Rendering.Presentation.FramesInFlight')
    $required = @{ 'Rendering.Path'=$path; 'Rendering.AntiAliasing.Mode'='TAA'; 'Rendering.GI.Technique'=$gi;
        'Rendering.GI.Denoiser'=$(if ($gi -eq 'Off') {'None'} else {'SVGF'});
        'Rendering.GI.RenderScale'=0.5; 'Rendering.Presentation.VSync'=$false }
    foreach ($id in $required.Keys) { $patch[$id] = $required[$id] }
    if ($TransientTextureReuse) { $patch['Rendering.Memory.TransientTextureReuse'] = $true }
    elseif (-not $patch.ContainsKey('Rendering.Memory.TransientTextureReuse')) { $patch['Rendering.Memory.TransientTextureReuse'] = $false }
    $applied = Call-Agent 'render.settings.apply' @{ layer='Session'; baseRevision=$settings.revision;
        baseHash=$settings.hash; patch=$patch; persist=$false }
    if (!$applied.success) {
        Write-Json (Join-Path $sceneDirectory ($name + '-rejected-receipt.json')) $applied
        $failure = [InvalidOperationException]::new("Render settings transaction rejected: $($applied.errorCode) $($applied.message)")
        $failure.Data['receipt'] = $applied
        throw $failure
    }
    $null = Call-Agent 'runtime.step' @{ frames=$SettleFrames }
    $effective = Call-Agent 'render.settings.get'
    foreach ($id in $patch.Keys) {
        $property = $effective.effective.PSObject.Properties[$id]
        $expected = $patch[$id]
        $matches = $null -ne $property
        if ($matches -and ($expected -is [bool] -or $expected -is [string])) {
            $matches = $property.Value -eq $expected
        } elseif ($matches) {
            $matches = [Math]::Abs([double]$property.Value - [double]$expected) -le
                [Math]::Max(0.000001, [Math]::Abs([double]$expected) * 0.00001)
        }
        if (-not $matches) { throw "Effective $id=$($property.Value) differs from requested $expected." }
    }
    $passes = @(Get-GraphPasses)
    if ($path -eq 'Deferred' -and !($passes.name -contains 'DeferredLightingHDR')) { throw 'Deferred settings has no executing DeferredLightingHDR pass.' }
    if ($path -eq 'Forward' -and !($passes.name -contains 'OpaqueForwardHDR')) { throw 'Forward settings has no executing OpaqueForwardHDR pass.' }
    if ($path -eq 'Forward' -and ($passes.name -contains 'DeferredLightingHDR')) { throw 'Forward settings still executes deferred lighting.' }
    Write-Json (Join-Path $script:sceneDirectory ($name + '-settings.json')) $effective
    Write-Json (Join-Path $script:sceneDirectory ($name + '-passes.json')) $passes
    $resources = @(); $offset = 0; $snapshot = ''
    do {
        $parameters = @{ section='resources'; offset=$offset; limit=100 }
        if ($snapshot) { $parameters.snapshotId=$snapshot }
        $page = Call-Agent 'render.graph.snapshot' $parameters
        $snapshot = [string]$page.snapshotId
        $resources += $page.items
        $offset = $page.nextOffset
    } while ($null -ne $offset)
    Write-Json (Join-Path $script:sceneDirectory ($name + '-resources.json')) $resources
    $capture = Save-Capture $name
    Add-Result $name 'PASS' 'Transaction, effective settings, executing main graph path, and native-size capture verified. Visual material correctness remains unreviewed.' $capture
    Add-Result ($name + '-visual') 'NOT_REVIEWED' 'Review static material coverage and temporal artifacts in the saved image; file existence is not visual PASS.' $capture.path
    if ($script:extraViewLive) { Test-ExtraViewFrame $path ($name + '-extra-view') }
}

function Test-ExtraViewFrame([string]$path,[string]$name) {
    $null = Call-Agent 'render.view.capture'
    $view = $null
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        $null = Call-Agent 'runtime.step' @{ frames=2 }
        $view = Call-Agent 'render.view.status'
        if ($view.capture.status -eq 'failed') { throw "Independent view readback failed: $($view.capture.error)" }
        if ($view.capture.status -eq 'complete') { break }
    }
    if (!$view.available -or $view.capture.status -ne 'complete') { throw 'Independent view did not complete its native readback.' }
    $requiredPass = $(if ($path -eq 'Deferred') {'DeferredLightingHDR'} else {'OpaqueForwardHDR'})
    if (!($view.passes -contains $requiredPass)) { throw 'Independent view did not execute the requested main rendering path.' }
    $world = Call-Agent 'world.summary'
    if ($view.sceneGeneration -ne $world.sceneGeneration) { throw 'Independent view is rendering a stale scene generation.' }
    $capture = Save-Capture $name @([int]$view.width,[int]$view.height) $view.capture
    Write-Json (Join-Path $script:sceneDirectory ($name + '-status.json')) $view
    Add-Result $name 'PASS' 'The separately owned view executed the requested path with the current scene generation and completed its own native-size GPU readback. Its 960x540 resolution is correctness coverage only, never the native-4K performance baseline.' @{ status=$view; capture=$capture }
}

function Invoke-MaterialCompiler($metadata,[string]$label) {
    $compiler = $ShaderCompilerPath
    if (!$compiler) { $compiler = Join-Path $repositoryRoot 'out/agent/game-relwithdebinfo/bin/Programs/HShaderCompiler/HShaderCompiler.exe' }
    if (!(Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'Existing isolated HShaderCompiler is required for material publication testing.' }
    $prefix = Join-Path $script:sceneDirectory $label
    $arguments = @('--rootPath',[string]$metadata.shaderRoot,'--includePath',[string]$metadata.includeRoot,
        '--outputPath',[string]$metadata.artifactOutput,'--shader',[string]$metadata.sourceFile,
        '--binary-format','all','--diagnosticsOutput',($prefix + '.diagnostics.log'))
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $compiler @arguments 1> ($prefix + '.stdout.log') 2> ($prefix + '.stderr.log')
    $code = $LASTEXITCODE; $timer.Stop()
    $commands.Add(@{ program=$compiler; arguments=$arguments; exitCode=$code; wallMs=$timer.Elapsed.TotalMilliseconds;
        output=$prefix+'.stdout.log'; evidenceType='build-only' })
    if ($code -ne 0) { throw "Official HShaderCompiler failed for $label; see disk diagnostics." }
}

function Test-MaterialHotUpdate {
    $sourcePath = 'Shaders/FresnelRim.hlsl'
    $before = Call-Agent 'render.material.refresh' @{ path=$sourcePath }
    if (!$before.succeeded) { throw 'Baseline Fresnel Surface material has no valid compiled program.' }
    $sourceFile = [IO.Path]::GetFullPath([string]$before.sourceFile)
    $expectedSource = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'Project/Shaders/FresnelRim.hlsl'))
    if ($sourceFile -ne $expectedSource) { throw 'The hot-update fixture must resolve to the existing Project Fresnel shader.' }
    $original = [IO.File]::ReadAllBytes($sourceFile)
    $originalTime = [IO.File]::GetLastWriteTimeUtc($sourceFile)
    [IO.File]::WriteAllBytes((Join-Path $script:sceneDirectory 'FresnelRim.original.hlsl'),$original)
    $text = [Text.Encoding]::UTF8.GetString($original)
    $marker = 'surface.Emissive += g_RimColor.rgb * rim * g_RimIntensity;'
    if (($text.Split(@($marker),[StringSplitOptions]::None)).Length -ne 2) { throw 'Fresnel hot-update marker is not unique; do not rewrite an unknown shader.' }
    try {
        $modified = $text.Replace($marker,'surface.Emissive += float3(1.0, 0.04, 0.01) * rim * g_RimIntensity;')
        [IO.File]::WriteAllText($sourceFile,$modified,[Text.UTF8Encoding]::new($false))
        Invoke-MaterialCompiler $before 'hot-material-compile'
        $published = Call-Agent 'render.material.refresh' @{ path=$sourcePath }
        if (!$published.succeeded -or $published.generation -le $before.generation -or $published.sourceHash -eq $before.sourceHash -or $published.artifactHash -eq $before.artifactHash) {
            throw 'The modified shader did not publish a distinct valid material generation and artifact set.'
        }
        Write-Json (Join-Path $script:sceneDirectory 'hot-material-publication.json') @{ before=$before; after=$published }
        foreach ($path in @('Forward','Deferred')) {
            Apply-Configuration $path 'Off' ('hot-material-' + $path)
            if ($script:extraViewLive) {
                $view = Call-Agent 'render.view.status'
                if ($view.materialGenerationMax -lt $published.generation -or $view.customGpuDraws -lt 1 -or $view.customGpuObjects -lt 1) {
                    throw 'The independent view did not report the published material generation in its rendered custom draws.'
                }
            }
        }
        Add-Result 'material-hot-update' 'PASS' 'Official compilation produced a changed material artifact and generation; both paths rendered afterward, including the independent view. Captures require visual review of the intentionally changed Fresnel rim.' $published
    } finally {
        [IO.File]::WriteAllBytes($sourceFile,$original)
        try {
            Invoke-MaterialCompiler $before 'hot-material-restore-compile'
            $restored = Call-Agent 'render.material.refresh' @{ path=$sourcePath }
            if (!$restored.succeeded -or $restored.sourceHash -ne $before.sourceHash -or $restored.artifactHash -ne $before.artifactHash) { throw 'Restored original shader did not republish its original source/artifact identity.' }
            $null = Call-Agent 'runtime.step' @{ frames=$SettleFrames }
            Add-Result 'material-hot-update-restoration' 'PASS' 'Original source bytes and compiled material identity were restored through the normal compiler and publication path.' $restored
        } finally { [IO.File]::SetLastWriteTimeUtc($sourceFile,$originalTime) }
    }
}

function Test-CandidateFailure {
    # First complete a real frame. Startup selection has no allocated old graph
    # and intentionally does not consume the renderer's one-shot fault.
    $null = Call-Agent 'runtime.step' @{ frames=$SettleFrames }
    $before = Call-Agent 'render.settings.get'
    $oldPath = [string]$before.effective.'Rendering.Path'
    $newPath = $(if ($oldPath -eq 'Deferred') { 'Forward' } else { 'Deferred' })
    $oldPasses = @(Get-GraphPasses)
    $oldNames = $oldPasses.name -join '|'
    $request = @{ layer='Session'; baseRevision=$before.revision; baseHash=$before.hash; persist=$false;
        patch=@{ 'Rendering.Path'=$newPath; 'Rendering.AntiAliasing.Mode'='TAA';
            'Rendering.GI.Technique'='ScreenSpaceGI'; 'Rendering.GI.Denoiser'='SVGF';
            'Rendering.GI.RenderScale'=0.5; 'Rendering.Presentation.VSync'=$false } }
    Write-Json (Join-Path $script:sceneDirectory 'failure-before-settings.json') $before
    Write-Json (Join-Path $script:sceneDirectory 'failure-before-passes.json') $oldPasses
    $rejection = $null; $rejected = $false; $diagnostic = ''
    try {
        $rejection = Call-Agent 'render.settings.apply' $request
        $rejected = !$rejection.success
        $diagnostic = $rejection | ConvertTo-Json -Depth 30 -Compress
    } catch {
        $rejected = $true
        $diagnostic = $_.Exception.Message
        if ($commands.Count -gt 0) {
            $diagnostic += [IO.File]::ReadAllText([string]$commands[$commands.Count-1].output)
        }
    }
    if (!$rejected -or $diagnostic -notmatch 'Injected candidate failure after candidate resource allocation') {
        throw 'Expected the dev-only post-allocation injected failure; a validation rejection or successful apply does not verify rollback.'
    }
    $after = Call-Agent 'render.settings.get'
    if ($after.revision -ne $before.revision -or $after.hash -ne $before.hash -or
        $after.effective.'Rendering.Path' -ne $oldPath) { throw 'Rejected candidate changed active revision, hash, or path.' }
    $null = Call-Agent 'runtime.step' @{ frames=$SettleFrames }
    $afterPasses = @(Get-GraphPasses)
    if (($afterPasses.name -join '|') -ne $oldNames) { throw 'Rejected candidate changed the executing graph pass sequence.' }
    $requiredOldPass = $(if ($oldPath -eq 'Deferred') {'DeferredLightingHDR'} else {'OpaqueForwardHDR'})
    if (!($afterPasses.name -contains $requiredOldPass)) { throw 'Old main path did not execute after candidate failure.' }
    $oldCapture = Save-Capture 'injected-failure-old-path'
    Write-Json (Join-Path $script:sceneDirectory 'failure-after-settings.json') $after
    Write-Json (Join-Path $script:sceneDirectory 'failure-after-passes.json') $afterPasses
    Add-Result 'candidate-allocation-rollback' 'PASS' 'The named post-allocation fault was rejected; revision, hash, effective path, and executing pass sequence remained unchanged. The old graph rendered additional frames and a native-size capture.' @{ rejection=$rejection; capture=$oldCapture }
    $retry = Call-Agent 'render.settings.apply' $request
    if (!$retry.success) { throw 'Retry of the identical configuration patch failed after the one-shot fault was consumed.' }
    $null = Call-Agent 'runtime.step' @{ frames=$SettleFrames }
    $committed = Call-Agent 'render.settings.get'
    $newPasses = @(Get-GraphPasses)
    $requiredNewPass = $(if ($newPath -eq 'Deferred') {'DeferredLightingHDR'} else {'OpaqueForwardHDR'})
    if ($committed.effective.'Rendering.Path' -ne $newPath -or !($newPasses.name -contains $requiredNewPass)) {
        throw 'Retried candidate did not become the effective executing path.'
    }
    Write-Json (Join-Path $script:sceneDirectory 'failure-retry-settings.json') $committed
    Write-Json (Join-Path $script:sceneDirectory 'failure-retry-passes.json') $newPasses
    $newCapture = Save-Capture 'injected-failure-retry-path'
    Add-Result 'candidate-allocation-retry' 'PASS' 'The identical patch succeeded after one fault, and the requested main path executed and produced a native-size capture. This does not validate static image quality or performance.' $newCapture
}

try {
    if ($SampleMemory) {
        $samplerScript=Join-Path $PSScriptRoot 'Rendering-MemorySampler.ps1'
        $memorySampler=Start-Process powershell.exe -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass',
            '-File',('"'+$samplerScript+'"'),'-OutputDirectory',('"'+$OutputDirectory+'"')) `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $OutputDirectory 'memory-sampler.stdout.log') `
            -RedirectStandardError (Join-Path $OutputDirectory 'memory-sampler.stderr.log')
        $null=$memorySampler.Handle
    }
    if ($IncludeParticles) { $Scenes += 'Assets/Scenes/VFXStressTest.he' }
    foreach ($currentScene in $Scenes) {
        if ($currentScene -notmatch '^Assets/[^"\r\n]+\.he$' -or @($currentScene.Split('/')) -contains '..') { throw 'Use safe Assets-relative .he paths.' }
        $source = Join-Path (Join-Path $repositoryRoot 'Project') $currentScene
        if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing scene $currentScene" }
        $sceneDirectory = Join-Path $OutputDirectory ([IO.Path]::GetFileNameWithoutExtension($currentScene))
        New-Item -ItemType Directory -Path $sceneDirectory | Out-Null
        $started = $false
        $extraViewLive = $false
        try {
            [Environment]::SetEnvironmentVariable('HE_DEV_RENDER_FAIL_CANDIDATE_ONCE',
                $(if ($InjectCandidateFailure) {'1'} else {$null}),'Process')
            if ($currentScene -eq 'Assets/Scenes/VFXStressTest.he') {
                foreach ($key in $particleEnvironment.Keys) {
                    $savedParticleEnvironment[$key] = [Environment]::GetEnvironmentVariable($key,'Process')
                    [Environment]::SetEnvironmentVariable($key,$particleEnvironment[$key],'Process')
                }
            }
            # Start is always paired with owner-tagged Stop, including failed starts.
            $started = $true
            $startArguments = @('agent','start','-StartupScene',$currentScene,'-Resolution',$Resolution,
                '-RenderPath','Deferred','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-RenderValidation','-RenderStats')
            if ($ExistingBuildSha256) { $startArguments += @('-ExistingBuildSha256',$ExistingBuildSha256) }
            if ($configurationPatch.ContainsKey('Rendering.Presentation.FramesInFlight')) {
                # Startup-only values need command-line precedence; live test
                # values must remain overridable by the Session transactions.
                $startupSettingsPath = Join-Path $sceneDirectory 'startup-settings.json'
                Write-Json $startupSettingsPath @{ 'Rendering.Presentation.FramesInFlight'=$configurationPatch['Rendering.Presentation.FramesInFlight'] }
                $startArguments += @('-RenderSettingsPath',$startupSettingsPath)
            }
            Invoke-HeCtl $startArguments 'start' $false
            $discovery = Call-Agent 'api.discover'
            Write-Json (Join-Path $sceneDirectory 'discovery.json') $discovery
            $methodNames = @($discovery.commands.name)
            $settings = Call-Agent 'render.settings.get'
            if ($configurationPatch.ContainsKey('Rendering.Presentation.FramesInFlight') -and
                $settings.effective.'Rendering.Presentation.FramesInFlight' -ne $configurationPatch['Rendering.Presentation.FramesInFlight']) {
                throw 'Startup frames-in-flight configuration did not take effect.'
            }
            if ($settings.effective.'Rendering.Backend' -ne $GraphicsApi) { throw "Session backend is $($settings.effective.'Rendering.Backend'), expected $GraphicsApi; verify Agent start API forwarding." }
            $queuePlan = Call-Agent 'render.settings.plan' @{ layer='Session'; baseRevision=$settings.revision;
                baseHash=$settings.hash; patch=@{ 'Rendering.Presentation.FramesInFlight'=$(if ($settings.effective.'Rendering.Presentation.FramesInFlight' -eq 2) {3} else {2}) } }
            $afterQueuePlan = Call-Agent 'render.settings.get'
            if ($queuePlan.success -or $afterQueuePlan.hash -ne $settings.hash -or $afterQueuePlan.revision -ne $settings.revision) {
                throw 'Restart-only frame count was accepted as a live session change.'
            }
            Add-Result 'frame-count-session-rejected' 'PASS' 'Restart-only frame count is unavailable to live Session overrides; snapshot unchanged.' $queuePlan
            $world = Call-Agent 'world.summary'
            Write-Json (Join-Path $sceneDirectory 'initial-world.json') $world
            if (!$world.available -or $world.entityCount -lt 1) { throw 'Startup scene has no runtime world.' }
            if ($InjectCandidateFailure) {
                Test-CandidateFailure
                continue
            }
            if ($methodNames -contains 'render.view.create') {
                $created = Call-Agent 'render.view.create' @{ width=960; height=540 }
                if (!$created.available) { throw 'Independent offscreen view creation failed.' }
                $extraViewLive = $true
                Write-Json (Join-Path $sceneDirectory 'extra-view-created.json') $created
            } else { Add-Result 'multi-view' 'FAIL' 'The required independent view API is absent from this binary.' }
            for ($cycle = 0; $cycle -lt $SwitchCycles; $cycle++) {
                foreach ($path in @('Forward','Deferred')) {
                    try { Apply-Configuration $path 'Off' ('switch-{0}-{1}' -f $cycle,$path) }
                    catch { Add-Result ('switch-{0}-{1}' -f $cycle,$path) 'FAIL' $_.Exception.Message }
                }
            }
            foreach ($path in @('Forward','Deferred')) {
                try { Apply-Configuration $path 'ScreenSpaceGI' ('ssgi-' + $path) }
                catch { Add-Result ('ssgi-' + $path) 'FAIL' $_.Exception.Message }
            }
            if ($IncludeHardwareRTGI) {
                if ($settings.capabilities.hardwareRayTracing) {
                    foreach ($path in @('Forward','Deferred')) {
                        try { Apply-Configuration $path 'HardwareRTGI' ('rtgi-' + $path) }
                        catch {
                            $failure = $_.Exception
                            $receipt = $failure.Data['receipt']
                            $view = Call-Agent 'render.view.status'
                            if ($null -ne $receipt -and $receipt.errorCode -eq 'transaction_failed' -and
                                $receipt.message -match 'Real-time RTGI requires a prepared RayTracingScene' -and
                                $view.available -and $view.PSObject.Properties['rayTracingInstanceCount'] -and
                                $view.rayTracingInstanceCount -eq 0) {
                                Add-Result ('rtgi-' + $path) 'NOT_APPLICABLE' 'RTGI preparation explicitly rejected a missing prepared RayTracingScene and live view reports zero RT instances; no RTGI coverage claim.' @{receipt=$receipt;view=$view}
                            } else { Add-Result ('rtgi-' + $path) 'FAIL' $failure.Message $receipt }
                        }
                    }
                } else { Add-Result 'HardwareRTGI' 'UNSUPPORTED' 'Effective hardware RT capability is unavailable; no RT coverage claim.' }
            }
            try {
                $before = Save-Capture 'motion-before'
                $checkBones = $currentScene -eq 'Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he'
                $boneObservations = @()
                if ($checkBones) {
                    if (!($methodNames -contains 'render.animation.status')) { throw 'The required submitted-bone diagnostic API is absent from this binary.' }
                    $boneObservations += Call-Agent 'render.animation.status'
                }
                try {
                    $null = Call-Agent 'input.set' @{ held=@('w') }
                    foreach ($interval in @(20,40)) {
                        $null = Call-Agent 'runtime.step' @{ frames=$interval }
                        if ($checkBones) { $boneObservations += Call-Agent 'render.animation.status' }
                    }
                } finally { $null = Call-Agent 'input.clear' }
                $after = Save-Capture 'motion-after'
                $afterWorld = Call-Agent 'world.summary'
                Write-Json (Join-Path $sceneDirectory 'motion-world.json') $afterWorld
                Add-Result 'motion-sequence' 'CAPTURED' '60 fixed simulation steps with existing movement script and skeletal animation. Requires motion/occlusion review; changed image hash alone is not correctness proof.' @($before,$after)
                if ($checkBones) {
                    $boneEvidence = Join-Path $sceneDirectory 'motion-submitted-bones.json'
                    Write-Json $boneEvidence $boneObservations
                    foreach ($sample in $boneObservations) {
                        if (!$sample.available -or $sample.source -ne 'lastSuccessfullySubmittedMainRenderWorld' -or $sample.skinnedObjectCount -lt 1) {
                            throw 'SurfaceCoverage has no skinned objects in the actual submitted RenderWorld.'
                        }
                        if ($sample.sceneGeneration -ne $boneObservations[0].sceneGeneration) { throw 'Bone comparison crossed a scene reload.' }
                    }
                    for ($index = 1; $index -lt $boneObservations.Count; $index++) {
                        $sample = $boneObservations[$index]
                        $previous = $boneObservations[$index-1]
                        if ($sample.renderWorldFrame -le $previous.renderWorldFrame -or $sample.previousObservationFrame -ne $previous.renderWorldFrame) {
                            throw 'The submitted bone observations did not advance along the sampled frame sequence.'
                        }
                        $changed = @($sample.objects | Where-Object {
                            $_.currentPaletteCount -gt 0 -and $_.observationComparisonAvailable -and
                            $_.observationPaletteCount -eq $_.currentPaletteCount -and $_.observationComparisonComplete -and
                            $_.observationChangedMatrixCount -gt 0 -and $_.observationMaxElementDelta -gt 0.000001 -and
                            $_.observationNonFiniteComparisons -eq 0 -and $_.hashCoversFullPalettes -and
                            $_.weights.inspectedVertices -gt 0 -and $_.weights.invalidWeights -eq 0 -and
                            $_.weights.positiveInfluencesOutsidePalette -eq 0 -and $_.weights.verticesWithoutValidWeight -eq 0
                        })
                        if ($changed.Count -lt 1) { throw 'No finite submitted bone-palette motion with valid inspected skin weights was observed over the 20/40-frame intervals.' }
                    }
                    Add-Result 'submitted-bone-motion' 'PASS' 'Actual submitted poses changed numerically over separate 20/40-frame intervals; inspected positive skin weights address valid palette entries. This is CPU snapshot evidence, not visual skinning or motion-vector PASS.' $boneEvidence
                }
            } catch { Add-Result 'motion-sequence' 'FAIL' $_.Exception.Message }
            if ($methodNames -contains 'window.resize') {
                try {
                    foreach ($dimensions in @(@(1279,719),$expectedSize)) {
                        $null = Call-Agent 'window.resize' @{ width=$dimensions[0]; height=$dimensions[1] }
                        $matched = $false
                        for ($attempt = 0; $attempt -lt 30; $attempt++) {
                            $null = Call-Agent 'runtime.step' @{ frames=1 }
                            $state = Call-Agent 'session.status'
                            if ($state.renderWidth -eq $dimensions[0] -and $state.renderHeight -eq $dimensions[1]) { $matched=$true; break }
                        }
                        if (!$matched) { throw 'Resize did not reach the requested render extent.' }
                        $image = Save-Capture ('resize-{0}x{1}' -f $dimensions[0],$dimensions[1]) $dimensions
                        Add-Result ('resize-{0}x{1}' -f $dimensions[0],$dimensions[1]) 'PASS' 'Reported render extent and completed PNG size agree. Odd-size intermediate is a correctness case, never performance evidence.' $image
                        if ($extraViewLive) {
                            $active = Call-Agent 'render.settings.get'
                            Test-ExtraViewFrame ([string]$active.effective.'Rendering.Path') ('resize-{0}x{1}-extra-view' -f $dimensions[0],$dimensions[1])
                        }
                    }
                } catch { Add-Result 'window.resize' 'FAIL' $_.Exception.Message }
            } else { Add-Result 'window.resize' 'FAIL' 'The required resize method is absent from this binary.' }
            if ($methodNames -contains 'camera.cut') {
                try {
                    $cut = Call-Agent 'camera.cut'
                    $null = Call-Agent 'runtime.step' @{ frames=2 }
                    $image = Save-Capture 'camera-cut'
                    Add-Result 'camera.cut' 'CAPTURED' 'Explicit cut was requested and subsequent rendering completed. History rejection and first-frame ghosting still require visual/diagnostic review.' @{ receipt=$cut; capture=$image }
                } catch { Add-Result 'camera.cut' 'FAIL' $_.Exception.Message }
            } else { Add-Result 'camera.cut' 'FAIL' 'The required camera-cut method is absent from this binary.' }
            if ($methodNames -contains 'runtime.scene.load') {
                try {
                    $beforeWorld = Call-Agent 'world.summary'
                    $load = Call-Agent 'runtime.scene.load' @{ path=$currentScene; forceReload=$true }
                    $reloaded = $false
                    for ($attempt = 0; $attempt -lt 30; $attempt++) {
                        $null = Call-Agent 'runtime.step' @{ frames=1 }
                        $loadedWorld = Call-Agent 'world.summary'
                        if ($loadedWorld.sceneGeneration -ne $beforeWorld.sceneGeneration) { $reloaded=$true; break }
                    }
                    if (!$reloaded) { throw 'Force reload did not produce a new scene generation.' }
                    $image = Save-Capture 'runtime-reload'
                    Add-Result 'runtime.scene.load' 'PASS' 'Same-scene forced reload produced a new runtime scene generation and completed a native-size rendered capture; visual coverage remains unreviewed.' @{ receipt=$load; world=$loadedWorld; capture=$image }
                    if ($extraViewLive) {
                        $active = Call-Agent 'render.settings.get'
                        Test-ExtraViewFrame ([string]$active.effective.'Rendering.Path') 'runtime-reload-extra-view'
                    }
                } catch { Add-Result 'runtime.scene.load' 'FAIL' $_.Exception.Message }
            } else { Add-Result 'runtime.scene.load' 'FAIL' 'The required scene-load method is absent from this binary.' }
            if ($currentScene -eq 'Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he') {
                try { Test-MaterialHotUpdate }
                catch { Add-Result 'material-hot-update' 'FAIL' $_.Exception.Message }
            } else { Add-Result 'material-hot-update' 'NOT_APPLICABLE' 'The directed SurfaceCoverage fixture owns this material publication regression.' }
            if ($extraViewLive) {
                $null = Call-Agent 'render.view.destroy'
                $extraViewLive = $false
                if ((Call-Agent 'render.view.status').available) { throw 'The owned offscreen view was not destroyed.' }
                Add-Result 'multi-view-cleanup' 'PASS' 'The session-owned offscreen view was destroyed before author preview.'
            }
            # Preview is deliberately last: it replaces rendering with a
            # non-simulated author document and cannot stand in for reload.
            try {
                $author = Call-Agent 'scene.open' @{ path=$currentScene }
                $preview = Call-Agent 'scene.preview' @{ sceneId=$author.sceneId; revision=$author.revision }
                $image = Save-Capture 'author-preview'
                Add-Result 'author-preview' 'CAPTURED' 'Render-only author preview. Scripts/animation are deliberately inactive; this is not a full runtime reload or equivalent scene-coverage test.' $image
            } catch { Add-Result 'author-preview' 'FAIL' $_.Exception.Message }
        } catch { Add-Result 'session' 'FAIL' $_.Exception.Message }
        finally {
            if ($extraViewLive) {
                try { $null = Call-Agent 'render.view.destroy' }
                catch { Add-Result 'multi-view-cleanup' 'FAIL' $_.Exception.Message }
                $extraViewLive = $false
            }
            if ($started) {
                try { Invoke-HeCtl @('agent','stop') 'stop' $false }
                catch { Add-Result 'session-stop' 'FAIL' $_.Exception.Message }
            }
            foreach ($key in @($savedParticleEnvironment.Keys)) { [Environment]::SetEnvironmentVariable($key,$savedParticleEnvironment[$key],'Process') }
            $savedParticleEnvironment.Clear()
            Write-Json (Join-Path $OutputDirectory 'results.json') $results.ToArray()
            Write-Json (Join-Path $OutputDirectory 'commands.json') $commands.ToArray()
        }
    }
} finally {
    if ($memorySampler) {
        try {
            [IO.File]::WriteAllText((Join-Path $OutputDirectory 'memory-sampler.stop'),'stop')
            if (!$memorySampler.WaitForExit(5000)) { $memorySampler.Kill(); $memorySampler.WaitForExit(); }
            $memorySummaryPath=Join-Path $OutputDirectory 'memory-summary.json'
            if ($memorySampler.ExitCode -ne 0 -or !(Test-Path -LiteralPath $memorySummaryPath)) {
                Add-Result 'memory-sampling' 'FAIL' 'Owned memory sampler did not finish cleanly.'
            } else {
                $memorySummary=Get-Content -LiteralPath $memorySummaryPath -Raw | ConvertFrom-Json
                $memoryStatus=if($memorySummary.gpuSamples -gt 0 -and $memorySummary.gameSamples -gt 0 -and $memorySummary.stoppedByOwner){'PASS'}else{'FAIL'}
                Add-Result 'memory-sampling' $memoryStatus 'Device-wide sampled memory and verified owned-process memory; does not prove an exact allocation peak.' $memorySummaryPath
            }
        } finally { $memorySampler.Dispose() }
    }
    Write-Json (Join-Path $OutputDirectory 'results.json') $results.ToArray()
    [Environment]::SetEnvironmentVariable('HE_DEV_RENDER_FAIL_CANDIDATE_ONCE',$savedFailureEnvironment,'Process')
    Write-Json (Join-Path $OutputDirectory 'configuration.json') @{
        requestedResolution=$Resolution; graphicsApi=$GraphicsApi; scenes=$Scenes; fixedDeltaSeconds=0.0166667;
        switchCycles=$SwitchCycles; settleFrames=$SettleFrames; hardwareRTGI=$IncludeHardwareRTGI.IsPresent;
        includeParticles=$IncludeParticles.IsPresent; injectCandidateFailure=$InjectCandidateFailure.IsPresent;
        transientTextureReuse=$TransientTextureReuse.IsPresent;
        sampleMemory=$SampleMemory.IsPresent;
        renderSettingsPath=$RenderSettingsPath;
        configuredSettings=$configurationPatch;
        pinnedExistingBuildSha256=$ExistingBuildSha256;
        commandTiming='End-to-end request wall time including CLI and completion barriers, not GPU frame or stable switch cost.';
        capturePolicy='Native PNGs retained on disk; no images automatically inspected.';
        giScale=0.5; taa='native main resolution'; visualPass=$false; performancePass=$false
    }
}
$failed = @($results | Where-Object status -eq 'FAIL').Count
Write-Host "Rendering path regression finished: $failed failures; evidence: $OutputDirectory"
if ($failed -gt 0) { exit 1 }
