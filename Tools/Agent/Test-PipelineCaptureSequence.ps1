[CmdletBinding()]
param([ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12', [string]$OutputDirectory='', [int]$TimeoutSeconds=180, [string]$ExistingBuildSha256='')
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$hectl=Join-Path $PSScriptRoot 'hectl.ps1'
if (!$OutputDirectory) { $OutputDirectory=Join-Path $repositoryRoot ('out/agent/pipeline-capture-sequence/'+(Get-Date -Format yyyyMMdd-HHmmss)) }
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
# Same authenticated, atomic file-mailbox protocol as Session.ps1. Publish the
# bounded batch without a shell launch or completion wait between its requests.
function Send-MailboxBatch([object[]]$calls,[string]$label) {
    if($calls.Count -lt 1 -or $calls.Count -gt 8){throw 'Mailbox batch must contain 1..8 requests.'}
    $batchRoot=Join-Path $OutputDirectory $label
    New-Item -ItemType Directory -Path $batchRoot | Out-Null
    $batchId='b'+[guid]::NewGuid().ToString('N')
    $requests=@()
    for($index=0;$index -lt $calls.Count;$index++) {
        $handle=$batchId+('-{0:d2}' -f $index)
        $payload=[ordered]@{protocol=1;id=$handle;method=$calls[$index].method;token=[string]$ownedSession.token;params=$calls[$index].params}
        $target=Join-Path ([string]$ownedSession.sessionDirectory) ('requests/'+$handle+'.request.json')
        $temporary=$target+'.tmp.'+[guid]::NewGuid().ToString('N')
        [IO.File]::WriteAllText($temporary,($payload|ConvertTo-Json -Depth 32 -Compress),[Text.UTF8Encoding]::new($false))
        $payload.token='[redacted]'
        Write-Json (Join-Path $batchRoot ($handle+'.request.json')) $payload
        $requests+=@{id=$handle;temporary=$temporary;target=$target;response=(Join-Path ([string]$ownedSession.sessionDirectory) ('responses/'+$handle+'.response.json'))}
    }
    foreach($request in $requests){[IO.File]::Move($request.temporary,$request.target)}
    $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $replies=@()
    foreach($request in $requests) {
        while(!(Test-Path -LiteralPath $request.response)) {
            if([DateTime]::UtcNow -gt $deadline){throw 'Mailbox batch response timed out.'}
            Start-Sleep -Milliseconds 1
        }
        $raw=[IO.File]::ReadAllText($request.response)
        [IO.File]::WriteAllText((Join-Path $batchRoot ($request.id+'.response.json')),$raw)
        $reply=$raw|ConvertFrom-Json
        if($reply.id -ne $request.id){throw 'Mailbox response ID mismatch.'}
        $replies+=@($reply)
    }
    return $replies
}
function Save-Sequence($sequence,[string]$label) {
    if($sequence.state -ne 'completed' -or @($sequence.frames).Count -ne 8){throw 'Expected exactly eight completed captures.'}
    $last=$null
    foreach($frame in $sequence.frames) {
        if($null -ne $last -and ([long]$frame.renderFrame -ne [long]$last.renderFrame+1 -or
            [long]$frame.simulationStep -ne [long]$last.simulationStep+1 -or
            [long]$frame.submission -le [long]$last.submission)){throw 'Actual image/simulation/submission progression is invalid.'}
        if($null -ne $last -and [Math]::Abs(([double]$frame.simulationTime-[double]$last.simulationTime)-0.0166667) -gt 0.00001){throw 'Image simulation timestamps did not advance by the configured fixed step.'}
        $destination=Join-Path $OutputDirectory ([IO.Path]::GetFileName($frame.path))
        Copy-Item -LiteralPath $frame.path -Destination $destination
        $frame|Add-Member -NotePropertyName sha256 -NotePropertyValue (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
        $last=$frame
    }
    Write-Json (Join-Path $OutputDirectory ($label+'.json')) $sequence
}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$run=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'pipeline-capture-sequence' -Paths @($OutputDirectory) -Outer
$state='failed'
try {
    $arguments=@('agent','start','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-RenderValidation','-FixedDeltaSeconds','0.0166667','-Resolution','1280x720','-StartupScene',$currentScene)
    if($ExistingBuildSha256){$arguments+=@('-ExistingBuildSha256',$ExistingBuildSha256)}
    Invoke-HeCtl $arguments 'start' $false
    $ownedSession=Get-Content -LiteralPath (Join-Path $repositoryRoot 'out/agent/sessions/active.json') -Raw|ConvertFrom-Json
    if($ownedSession.ownerTag -ne $ownerTag){throw 'Active mailbox does not belong to this test.'}
    $ownedProcess=Get-Process -Id ([int]$ownedSession.pid)
    if($ownedProcess.StartTime.ToUniversalTime().Ticks -ne [long]$ownedSession.processStartUtcTicks){throw 'Owned process identity changed.'}
    $null=Call-Agent 'runtime.step' @{frames=8}
    $positive=Call-Agent 'capture.sequence' @{prefix='positive';frames=8}
    Save-Sequence $positive 'positive'
    Add-Result 'eight-consecutive-images' 'PASS' 'Eight real readbacks: adjacent render and simulation counters increment exactly once.'
    $invalid=@(
        @{method='capture.sequence';params=@{prefix='seven';frames=7}},
        @{method='capture.sequence';params=@{prefix='seventeen';frames=17}},
        @{method='capture.sequence';params=@{prefix='fractional';frames=8.5}},
        @{method='capture.sequence';params=@{prefix='../escape';frames=8}},
        @{method='capture.sequence';params=@{prefix='bad-format';frames=8;format='../raw';cameraCut=$true}},
        @{method='capture.sequence';params=@{prefix='numeric-format';frames=8;format=42;cameraCut=$true}},
        @{method='capture.sequence';params=@{prefix='null-format';frames=8;format=$null;cameraCut=$true}},
        @{method='capture.sequence';params=@{prefix='positive';frames=8}})
    $replies=@(Send-MailboxBatch $invalid 'invalid-requests')
    if(@($replies|Where-Object{$_.ok}).Count){throw 'An invalid sequence request was accepted.'}
    Add-Result 'invalid-sequence-requests' 'PASS' '7/17/fractional/traversal/invalid-format/numeric-format/null-format/existing-prefix requests rejected by the live protocol.'
    $active=@(
        @{method='capture.sequence';params=@{prefix='cancelled';frames=16}},
        @{method='capture.frame';params=@{filename='must-not-run.png'}},
        @{method='capture.sequence';params=@{prefix='must-not-run';frames=8}},
        @{method='capture.cancel';params=@{}})
    $replies=@(Send-MailboxBatch $active 'active-ticket-batch')
    if(!$replies[0].ok -or $replies[1].ok -or $replies[2].ok -or !$replies[3].ok){throw 'Active-ticket batch failed to exercise sequence/single-capture exclusion and cancellation.'}
    if($replies[1].error.code -ne 'capture_pending'){throw 'Single-capture rejection did not identify an actually pending capture.'}
    $ticket=$replies[0].result.ticket
    if(!$replies[3].result.pending -or $replies[3].result.latest.ticket -ne $ticket){throw 'Cancellation missed the active sequence window; no cancellation coverage may be claimed.'}
    $status=Call-Agent 'session.status'
    Write-Json (Join-Path $OutputDirectory 'cancelled-status.json') $status
    if($status.capture.latest.ticket -ne $ticket -or $status.capture.latest.state -ne 'failed' -or $status.world.runtimeControl.pendingSteps -gt 0){throw 'Cancelled ticket did not settle as failure with simulation stopped.'}
    Add-Result 'active-ticket-exclusion-and-cancel' 'PASS' 'Batched live requests rejected competing captures; cancellation targeted the active ticket and settled failed.'
    $recovered=Call-Agent 'capture.sequence' @{prefix='recovered';frames=8}
    Save-Sequence $recovered 'recovered'
    Add-Result 'capture-after-cancel' 'PASS' 'A fresh eight-frame sequence completed after cancellation.'
    $state='success'
} finally {
    try { Invoke-HeCtl @('agent','stop') 'stop' $false }
    finally {
        Write-Json (Join-Path $OutputDirectory 'results.json') $results
        Write-Json (Join-Path $OutputDirectory 'commands.json') $commands
        Complete-HEStorageRun -Run $run -State $state
    }
}
