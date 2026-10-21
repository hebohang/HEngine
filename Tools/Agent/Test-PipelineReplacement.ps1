[CmdletBinding()]
param([ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12', [string]$OutputDirectory='', [int]$TimeoutSeconds=180, [string]$ExistingBuildSha256='')
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$hectl=Join-Path $PSScriptRoot 'hectl.ps1'
if (!$OutputDirectory) { $OutputDirectory=Join-Path $repositoryRoot ('out/agent/pipeline-replacement/'+(Get-Date -Format yyyyMMdd-HHmmss)) }
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new evidence directory.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$ownerTag=[guid]::NewGuid().ToString('N'); $sequence=0; $sceneDirectory=$OutputDirectory
$currentScene='Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he'
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
function Snapshot-Outputs([string]$label) {
    $main=Save-Capture ($label+'-main')
    $null=Call-Agent 'render.view.capture'
    $null=Call-Agent 'runtime.step' @{frames=3}
    $preview=Call-Agent 'render.view.status'
    if(!$preview.available -or $preview.capture.status -ne 'complete'){throw 'Preview readback did not complete.'}
    $null=Save-Capture ($label+'-preview') @(640,360) $preview.capture
    $settings=Call-Agent 'render.settings.get'
    $passes=Get-GraphPasses
    $snapshot=@{settings=$settings;preview=$preview;passes=$passes;
        mainSha256=(Get-FileHash (Join-Path $sceneDirectory ($label+'-main.png')) -Algorithm SHA256).Hash;
        previewSha256=(Get-FileHash (Join-Path $sceneDirectory ($label+'-preview.png')) -Algorithm SHA256).Hash}
    Write-Json (Join-Path $sceneDirectory ($label+'.json')) $snapshot
    return $snapshot
}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$run=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'pipeline-replacement' -Paths @($OutputDirectory) -Outer
$state='failed'
$oldAlias=$env:HE_DEV_RENDER_PIPELINE_TEST_ALIAS
$oldFault=$env:HE_DEV_RENDER_FAIL_CANDIDATE_ONCE
$oldPreview=$env:HE_DEV_RENDER_FAIL_CANDIDATE_PREVIEW_ONLY
try {
    foreach($failure in @($true,$false)) {
        $sceneDirectory=Join-Path $OutputDirectory $(if($failure){'failure'}else{'success'})
        New-Item -ItemType Directory -Path $sceneDirectory | Out-Null
        $env:HE_DEV_RENDER_PIPELINE_TEST_ALIAS='1'
        $env:HE_DEV_RENDER_FAIL_CANDIDATE_ONCE=$(if($failure){'1'}else{'0'})
        # Prepare main successfully, then fail preview, proving multi-view rollback.
        $env:HE_DEV_RENDER_FAIL_CANDIDATE_PREVIEW_ONLY='1'
        try {
            $startArguments=@('agent','start','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-RenderValidation','-RenderStats','-Resolution','1280x720','-StartupScene',$currentScene)
            if($ExistingBuildSha256){$startArguments+=@('-ExistingBuildSha256',$ExistingBuildSha256)}
            Invoke-HeCtl $startArguments 'start' $false
            $null=Apply-Patch @{'Rendering.Pipeline.Features'=@();'Rendering.GI.Technique'='Off';'Rendering.AntiAliasing.Mode'='FXAA'}
            $null=Call-Agent 'render.view.create' @{width=640;height=360}
            $null=Call-Agent 'runtime.step' @{frames=30}
            $before=Snapshot-Outputs 'before'
            $current=Call-Agent 'render.settings.get'
            $receipt=Call-Agent 'render.settings.apply' @{layer='Session';baseRevision=$current.revision;baseHash=$current.hash;
                patch=@{'Rendering.Pipeline.Type'='test.realtime-alias'};persist=$false}
            Write-Json (Join-Path $sceneDirectory 'receipt.json') $receipt
            if([bool]$receipt.success -eq $failure){throw 'Replacement transaction returned an unexpected outcome.'}
            if($failure -and ($receipt|ConvertTo-Json -Depth 20) -notmatch 'Injected candidate failure'){throw 'Failure was not the candidate allocation fault.'}
            $after=Snapshot-Outputs 'after'
            $expectedType=$(if($failure){'realtime'}else{'test.realtime-alias'})
            if($after.settings.effective.'Rendering.Pipeline.Type' -ne $expectedType){throw 'Main effective pipeline type is wrong.'}
            if($failure) {
                if($before.settings.hash -ne $after.settings.hash -or
                    $before.preview.outputGeneration -ne $after.preview.outputGeneration -or
                    $before.mainSha256 -ne $after.mainSha256 -or $before.previewSha256 -ne $after.previewSha256){
                    throw 'Failed replacement changed settings, preview generation, or rendered output hashes.'
                }
            } else {
                if($after.preview.outputGeneration -le $before.preview.outputGeneration){throw 'Preview pipeline was not replaced.'}
                if(!$after.preview.hasRecordedGraph -or @($after.passes).Count -eq 0){throw 'Replacement did not render both views.'}
            }
            Add-Result $(if($failure){'replacement-preview-failure-rollback'}else{'replacement-all-views-success'}) 'PASS' 'Real main and preview output readbacks and transaction receipt validated.'
        } finally { Invoke-HeCtl @('agent','stop') 'stop' $false }
    }
    $state='success'
} finally {
    $env:HE_DEV_RENDER_PIPELINE_TEST_ALIAS=$oldAlias
    $env:HE_DEV_RENDER_FAIL_CANDIDATE_ONCE=$oldFault
    $env:HE_DEV_RENDER_FAIL_CANDIDATE_PREVIEW_ONLY=$oldPreview
    Write-Json (Join-Path $OutputDirectory 'results.json') $results
    Write-Json (Join-Path $OutputDirectory 'commands.json') $commands
    Complete-HEStorageRun -Run $run -State $state
}
