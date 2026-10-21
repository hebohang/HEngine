[CmdletBinding()]
param(
    [ValidateSet('DX12','Vulkan')][string[]]$GraphicsApi=@('DX12','Vulkan'),
    [ValidateSet('Forward','Deferred')][string[]]$RenderingPath=@('Forward','Deferred'),
    [ValidateRange(8,16)][int]$Frames=8,
    [switch]$IncludeSsgi,
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExistingBuildSha256,
    [string]$RepositoryRoot='',
    [string]$OutputDirectory='',
    [int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
if(!$RepositoryRoot){$RepositoryRoot=Join-Path $PSScriptRoot '../..'}
$repositoryRoot=[IO.Path]::GetFullPath($RepositoryRoot)
$hectl=Join-Path $repositoryRoot 'Tools/Agent/hectl.ps1'
if(!$OutputDirectory){$OutputDirectory=Join-Path $repositoryRoot ('out/agent/pipeline-upgrade/supplement/camera-cut-'+(Get-Date -Format yyyyMMdd-HHmmss))}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
# Reuse only named helper declarations from the existing owned-session test.
# Parsing declarations does not execute that test's GPU body.
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repositoryRoot 'Tools/Agent/Test-PipelineCaptureSequence.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Existing capture helper syntax invalid.'}
foreach($name in @('Write-Json','Invoke-HeCtl','Call-Agent','Add-Result','Send-MailboxBatch')){
    $fn=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    if(!$fn){throw "Missing capture helper: $name"}
    Invoke-Expression $fn.Extent.Text
}
function Wait-Steps {
    $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $status=Call-Agent 'session.status'
        $control=$status.world.runtimeControl
        if($null -eq $control -or $null -eq $control.pendingSteps -or $null -eq $control.stepInProgress){throw 'Missing runtime step-completion fields.'}
        if([long]$control.pendingSteps -eq 0 -and !$control.stepInProgress -and $control.controlMode -eq 'paused'){return $status}
        if([DateTime]::UtcNow -ge $deadline){throw 'Timed out waiting for fully paused runtime.'}
        Start-Sleep -Milliseconds 100
    } while($true)
}
function Save-SequenceImage($image) {
    # Raw session.status frames provide relativePath; Session.ps1 capture calls
    # sometimes enrich path. Resolve only the authenticated owned mailbox path.
    $relative=[string]$image.relativePath
    if($relative -notmatch '^captures/[A-Za-z0-9_.-]+\.png$'){throw 'Unsafe or missing capture relativePath.'}
    $captureRoot=[IO.Path]::GetFullPath((Join-Path ([string]$ownedSession.sessionDirectory) 'captures'))
    $source=[IO.Path]::GetFullPath((Join-Path ([string]$ownedSession.sessionDirectory) $relative))
    if(!$source.StartsWith($captureRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Capture escaped owned directory.'}
    foreach($item in @((Get-Item -LiteralPath $captureRoot),(Get-Item -LiteralPath $source))){
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Capture path is a reparse point.'}
    }
    $stream=[IO.File]::OpenRead($source)
    try{$header=[byte[]]::new(24);if($stream.Read($header,0,24) -ne 24){throw 'Truncated PNG.'}}finally{$stream.Dispose()}
    if($header[0] -ne 137 -or $header[1] -ne 80 -or $header[2] -ne 78 -or $header[3] -ne 71){throw 'Capture is not PNG.'}
    $width=[int]($header[16]*16777216+$header[17]*65536+$header[18]*256+$header[19])
    $height=[int]($header[20]*16777216+$header[21]*65536+$header[22]*256+$header[23])
    if($width -ne 1280 -or $height -ne 720){throw "Unexpected capture dimensions ${width}x${height}."}
    $destination=Join-Path $script:sceneDirectory ([IO.Path]::GetFileName($source))
    if(Test-Path -LiteralPath $destination){throw 'Capture archive already exists.'}
    $hash=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $source -Destination $destination
    if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash){throw 'Capture archive hash mismatch.'}
    return @{path=$destination;relativePath=$relative;width=$width;height=$height;sha256=$hash;renderFrame=$image.renderFrame;simulationStep=$image.simulationStep;submission=$image.submission}
}
function Test-CutBoundary($before,$after,$firstImage) {
    foreach($status in @($before,$after)){
        if($null -eq $status.renderFrame -or $null -eq $status.completedRenderFrames){throw 'Missing application frame counters.'}
    }
    if($null -eq $firstImage.renderFrame){throw 'Missing first image frame counter.'}
    return [long]$before.renderFrame -eq [long]$after.renderFrame -and
        [long]$before.completedRenderFrames -eq [long]$after.completedRenderFrames -and
        [long]$firstImage.renderFrame -eq ([long]$before.completedRenderFrames+1)
}

. (Join-Path $repositoryRoot 'Tools/Agent/Storage.ps1')
$run=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'pipeline-camera-cut' -Paths @($OutputDirectory) -Outer
$state='failed';$sequence=0;$SampleMemory=$false
$results=[Collections.Generic.List[object]]::new();$commands=[Collections.Generic.List[object]]::new()
$currentScene='Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he'
try {
    foreach($api in $GraphicsApi){
        $ownedProcess=$null;$ownedSession=$null
        $ownerTag=[guid]::NewGuid().ToString('N')
        $script:sceneDirectory=Join-Path $OutputDirectory $api
        New-Item -ItemType Directory -Path $script:sceneDirectory|Out-Null
        try {
            $null=Invoke-HeCtl @('agent','start','-GraphicsApi',$api,'-StrictGraphicsApi','-RenderValidation','-FixedDeltaSeconds','0.0166667','-Resolution','1280x720','-StartupScene',$currentScene,'-ExistingBuildSha256',$ExistingBuildSha256) 'start' $false
            $ownedSession=Get-Content -LiteralPath (Join-Path $repositoryRoot 'out/agent/sessions/active.json') -Raw|ConvertFrom-Json
            if($ownedSession.ownerTag -ne $ownerTag){throw 'Active mailbox owner mismatch.'}
            $ownedProcess=Get-Process -Id ([int]$ownedSession.pid)
            if($ownedProcess.StartTime.ToUniversalTime().Ticks -ne [long]$ownedSession.processStartUtcTicks){throw 'Owned process identity changed.'}
            $binary=$ownedProcess.MainModule.FileName
            $actualHash=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
            Write-Json (Join-Path $script:sceneDirectory 'binary.json') @{path=$binary;sha256=$actualHash;requestedSha256=$ExistingBuildSha256;pid=$ownedProcess.Id;startTicks=$ownedSession.processStartUtcTicks}
            if($actualHash -ne $ExistingBuildSha256){throw 'Actual running binary hash differs from pinned target.'}
            # Existing main/preview isolation is already covered elsewhere; this
            # test does not claim a new independent-preview history experiment.
            $techniques=@('Off');if($IncludeSsgi){$techniques+=@('SSGI')}
            foreach($path in $RenderingPath){foreach($gi in $techniques){
                $label="$api-$path-$gi"
                $settings=Call-Agent 'render.settings.get'
                $receipt=Call-Agent 'render.settings.apply' @{layer='Session';baseRevision=$settings.revision;baseHash=$settings.hash;persist=$false;patch=@{
                    'Rendering.Path'=$path;'Rendering.AntiAliasing.Mode'='TAA';'Rendering.GI.Technique'=$(if($gi -eq 'SSGI'){'ScreenSpaceGI'}else{'Off'});
                    'Rendering.GI.Denoiser'=$(if($gi -eq 'SSGI'){'SVGF'}else{'None'});
                    'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false}}
                if(!$receipt.success){throw "Settings rejected: $label"}
                $null=Call-Agent 'runtime.step' @{frames=60}
                Write-Json (Join-Path $script:sceneDirectory ($label+'-before.json')) (Wait-Steps)
                $accepted=$false
                # No shell launches between status/cut/capture/status requests.
                # File renames are not atomic as a group. Require equal actual
                # frame counters around the batch, otherwise drain and retry.
                foreach($attempt in 1..3){
                    $null=Wait-Steps
                    $prefix="$label-cut-$attempt"
                    $reply=@(Send-MailboxBatch @(
                        @{method='session.status';params=@{}},
                        @{method='camera.cut';params=@{}},
                        @{method='capture.sequence';params=@{prefix=$prefix;frames=$Frames}},
                        @{method='session.status';params=@{}}
                    ) $prefix)
                    if(@($reply|Where-Object{!$_.ok}).Count){throw "Cut batch rejected: $prefix"}
                    if($reply[1].result.completion -ne 'queuedForNextExtraction'){throw 'Cut did not queue next extraction.'}
                    $before=$reply[0].result;$after=$reply[3].result
                    $ticket=$reply[2].result.ticket
                    $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
                    do {
                        $status=Call-Agent 'session.status'
                        $capture=$status.capture.latest
                        if($capture.ticket -ne $ticket){throw 'Sequence ticket changed.'}
                        if($capture.state -eq 'failed'){throw 'Sequence failed after cut.'}
                        if($capture.state -eq 'completed'){break}
                        if([DateTime]::UtcNow -gt $deadline){throw 'Cut sequence timeout.'}
                    } while($true)
                    Write-Json (Join-Path $script:sceneDirectory ($prefix+'-settled.json')) $status
                    $images=@($capture.frames)
                    if($images.Count -ne $Frames){throw 'Completed capture frame count mismatch.'}
                    $previous=$null;$archive=@()
                    foreach($image in $images){
                        if($image.state -ne 'completed'){throw 'Image not completed.'}
                        if($previous -and ([long]$image.renderFrame -ne [long]$previous.renderFrame+1 -or [long]$image.simulationStep -ne [long]$previous.simulationStep+1)){throw 'Image/render simulation sequence is not consecutive.'}
                        if($previous -and [long]$image.submission -le [long]$previous.submission){throw 'Submission did not advance.'}
                        $archive+=@(Save-SequenceImage $image)
                        $previous=$image
                    }
                    Write-Json (Join-Path $script:sceneDirectory ($prefix+'-images.json')) $archive
                    $firstExpected=[long]$before.completedRenderFrames+1
                    if(Test-CutBoundary $before $after $images[0]){
                        Add-Result $label 'CAPTURED_NOT_REVIEWED' 'Cut queued between same-frame status samples; first image is the next completed render, followed by consecutive images. Requires first-frame/history/edge visual review.' @{firstRenderFrame=$firstExpected;frames=$Frames;attempt=$attempt;ticket=$ticket;backend=$api;path=$path;gi=$gi}
                        $accepted=$true;break
                    }
                    Add-Result "$label-attempt-$attempt" 'BATCH_SPLIT_RETRY' 'Mailbox batch crossed a rendered-frame boundary; these images do not prove first-post-cut capture.'
                }
                if(!$accepted){throw "Could not prove first-post-cut frame after three attempts: $label"}
            }}
        } finally {
            try { $null=Invoke-HeCtl @('agent','stop') 'stop' $false }
            finally {
                try {
                    if($ownedSession){foreach($name in @('game.stdout.log','game.stderr.log')){
                        $log=Join-Path ([string]$ownedSession.runDirectory) $name
                        if(Test-Path -LiteralPath $log){Copy-Item -LiteralPath $log -Destination (Join-Path $script:sceneDirectory $name)}
                    }}
                } finally { if($ownedProcess){$ownedProcess.Dispose();$ownedProcess=$null} }
            }
        }
    }
    $state='success'
} finally {
    Write-Json (Join-Path $OutputDirectory 'results.json') $results
    Write-Json (Join-Path $OutputDirectory 'commands.json') $commands
    Complete-HEStorageRun -Run $run -State $state
}
