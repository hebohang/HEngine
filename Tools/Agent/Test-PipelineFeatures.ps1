[CmdletBinding()]
param([ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12', [string]$OutputDirectory='', [int]$TimeoutSeconds=180, [string]$ExistingBuildSha256='')
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$hectl=Join-Path $PSScriptRoot 'hectl.ps1'
if (!$OutputDirectory) { $OutputDirectory=Join-Path $repositoryRoot ('out/agent/pipeline-features/'+(Get-Date -Format yyyyMMdd-HHmmss)) }
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new evidence directory.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$ownerTag=[guid]::NewGuid().ToString('N'); $sequence=0; $sceneDirectory=$OutputDirectory
$currentScene='Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he'
$results=[Collections.Generic.List[object]]::new(); $commands=[Collections.Generic.List[object]]::new()
$SampleMemory=$false; $expectedSize=@(1280,720); $temporaryScenePath=$null
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

function Apply-Patch([hashtable]$patch) {
    $current=Call-Agent 'render.settings.get'
    $receipt=Call-Agent 'render.settings.apply' @{layer='Session';baseRevision=$current.revision;baseHash=$current.hash;patch=$patch;persist=$false}
    if (!$receipt.success) { throw ($receipt | ConvertTo-Json -Depth 10 -Compress) }
    $null=Call-Agent 'runtime.step' @{frames=30}
    return $receipt
}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$run=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'pipeline-features' -Paths @($OutputDirectory) -Outer
$state='failed'
try {
    $startArguments=@('agent','start','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-RenderValidation','-RenderStats','-Resolution','1280x720','-StartupScene',$currentScene)
    if($ExistingBuildSha256){$startArguments+=@('-ExistingBuildSha256',$ExistingBuildSha256)}
    Invoke-HeCtl $startArguments 'start' $false
    $null=Apply-Patch @{'Rendering.Path'='Forward';'Rendering.GI.Technique'='Off';'Rendering.PostProcess.Bloom.Intensity'=0.4}
    $enabled=Get-GraphPasses
    if (!($enabled.name -contains 'PostProcessComposite')) { throw 'Enabled postprocess did not execute.' }
    $null=Save-Capture 'features-enabled'
    $null=Apply-Patch @{'Rendering.Pipeline.Features'=@()}
    $disabled=Get-GraphPasses
    if ($disabled.name -contains 'PostProcessComposite' -or $disabled.name -contains 'EditorOutline') { throw 'Explicit empty Feature list still executes an optional feature.' }
    Add-Result 'feature-empty-disables' 'PASS' 'Production graph removed optional outline and HDR postprocess.'
    $null=Save-Capture 'features-disabled'
    $feature=@{type='builtin.postprocess';version=1;instance='postprocess';enabled=$true;parameters=@{}}
    $null=Apply-Patch @{'Rendering.Pipeline.Features'=@($feature);'Rendering.Views.Overrides'=@{preview=@{'Rendering.Path'='Deferred';'Rendering.Pipeline.Features'=@()}}}
    $null=Call-Agent 'render.view.create' @{width=640;height=360}
    $null=Call-Agent 'render.view.capture'
    $view=$null
    for ($i=0;$i -lt 60;$i++) {
        $null=Call-Agent 'runtime.step' @{frames=2}
        $view=Call-Agent 'render.view.status'
        if ($view.capture.status -eq 'complete') {break}
        if ($view.capture.status -eq 'failed') {throw 'Preview capture failed.'}
    }
    if ($view.capture.status -ne 'complete' -or !($view.passes -contains 'DeferredLightingHDR') -or $view.passes -contains 'PostProcessComposite') { throw 'Preview configuration did not execute independently.' }
    $main=Get-GraphPasses
    if (!($main.name -contains 'OpaqueForwardHDR') -or !($main.name -contains 'PostProcessComposite')) { throw 'Preview override contaminated the main view.' }
    Write-Json (Join-Path $OutputDirectory 'preview-status.json') $view
    $null=Save-Capture 'preview-independent' @(640,360) $view.capture
    Add-Result 'independent-view-features' 'PASS' 'Main Forward with postprocess; preview Deferred without optional features; independent readback completed.'
    $missing=@{type='missing.plugin';version=7;instance='missing';enabled=$true;parameters=@{retained=42}}
    $null=Apply-Patch @{'Rendering.Pipeline.Features'=@($missing,$feature)}
    $settings=Call-Agent 'render.settings.get'
    Write-Json (Join-Path $OutputDirectory 'missing-feature.json') $settings
    if ($settings.requested.'Rendering.Pipeline.Features'[0].parameters.retained -ne 42 -or $settings.effective.'Rendering.Pipeline.Features'[0].enabled) {throw 'Missing feature was destroyed or falsely enabled.'}
    Add-Result 'missing-feature-preserved' 'PASS' 'Requested payload retained; unavailable feature disabled in Effective.'
    # Prove persistent ECS overrides in a second owned session, using a new scene.
    Invoke-HeCtl @('agent','stop') 'stop-before-component-overrides' $false
    $sourceScene=Join-Path $repositoryRoot ('Project/'+$currentScene)
    $sceneDocument=Get-Content -LiteralPath $sourceScene -Raw | ConvertFrom-Json
    $cameras=@($sceneDocument.PSObject.Properties | Where-Object {
        ($_.Value.PSObject.Properties.Name -contains 'CameraComponent') -and $_.Value.CameraComponent.Primary
    })
    if($cameras.Count -ne 1){throw 'Component override fixture requires exactly one primary camera.'}
    $cameras[0].Value.CameraComponent | Add-Member -NotePropertyName RenderOverrides -NotePropertyValue @{'Rendering.Path'='Forward'} -Force
    $sceneDocument | Add-Member -NotePropertyName ([guid]::NewGuid().ToString()) -NotePropertyValue @{
        TagComponent=@{Tag='Temporary rendering override validation'}
        RenderSettingsComponent=@{Overrides=@{'Rendering.Path'='Deferred';'Rendering.Pipeline.Features'=@()}}
    }
    $temporarySceneRelative='Assets/RenderingBenchmarks/Validation/PipelineOverrides-'+[guid]::NewGuid().ToString('N')+'.he'
    $temporaryScenePath=[IO.Path]::GetFullPath((Join-Path $repositoryRoot ('Project/'+$temporarySceneRelative)))
    if(Test-Path -LiteralPath $temporaryScenePath){throw 'Temporary override scene unexpectedly exists.'}
    Write-Json $temporaryScenePath $sceneDocument
    Copy-Item -LiteralPath $temporaryScenePath -Destination (Join-Path $OutputDirectory 'component-overrides-scene.he')
    $script:currentScene=$temporarySceneRelative
    $startArguments=@('agent','start','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-RenderValidation','-RenderStats','-Resolution','1280x720','-StartupScene',$temporarySceneRelative)
    if($ExistingBuildSha256){$startArguments+=@('-ExistingBuildSha256',$ExistingBuildSha256)}
    Invoke-HeCtl $startArguments 'start-component-overrides' $false
    $null=Apply-Patch @{'Rendering.Path'='Deferred';'Rendering.Pipeline.Features'=@($feature)}
    $componentPasses=Get-GraphPasses
    Write-Json (Join-Path $OutputDirectory 'component-overrides-passes.json') $componentPasses
    if(!($componentPasses.name -contains 'OpaqueForwardHDR') -or $componentPasses.name -contains 'DeferredLightingHDR' -or
        $componentPasses.name -contains 'PostProcessComposite'){
        throw 'Persisted camera path or scene Feature override did not reach the production graph.'
    }
    $null=Save-Capture 'component-overrides'
    Add-Result 'persisted-world-camera-overrides' 'PASS' 'CameraComponent selected Forward over scene Deferred; RenderSettingsComponent disabled postprocess despite Session requesting it.'
    $state='success'
} finally {
    try { Invoke-HeCtl @('agent','stop') 'stop' $false }
    finally {
        if($temporaryScenePath -and (Test-Path -LiteralPath $temporaryScenePath)) {
            $temporaryRoot=[IO.Path]::GetFullPath((Join-Path $repositoryRoot 'Project/Assets/RenderingBenchmarks/Validation'))+[IO.Path]::DirectorySeparatorChar
            if(!$temporaryScenePath.StartsWith($temporaryRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Temporary scene cleanup escaped its directory.'}
            Remove-Item -LiteralPath $temporaryScenePath -Force
        }
        Write-Json (Join-Path $OutputDirectory 'results.json') $results
        Write-Json (Join-Path $OutputDirectory 'commands.json') $commands
        Complete-HEStorageRun -Run $run -State $state
    }
}
Write-Host "Pipeline Feature validation: $state; evidence: $OutputDirectory"
