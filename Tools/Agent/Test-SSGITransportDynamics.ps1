param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$Scene,
    [Parameter(Mandatory)][string]$SettingsPath,
    [string]$Resolution = '1920x1080',
    [ValidateRange(1,64)][int]$StaticSamples = 8,
    [switch]$StaticOnly,
    [switch]$BoardOnly,
    [switch]$ReactiveNoiseOnly
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new evidence directory.' }
New-Item -ItemType Directory -Path $output | Out-Null
$ctl = Join-Path $PSScriptRoot 'hectl.ps1'
$receipts = [Collections.Generic.List[object]]::new()
$sun = 'a6383972-2314-4f4b-b9cf-001000000021'
$camera = 'a6383972-2314-4f4b-b9cf-001000000020'
$board = 'a6383972-2314-4f4b-b9cf-001000000011'
function Call-Agent([string]$method, [hashtable]$parameters = @{}) {
    $request = Join-Path $output 'request.json'
    [IO.File]::WriteAllText($request, ($parameters | ConvertTo-Json -Depth 15), [Text.UTF8Encoding]::new($false))
    $raw = & $ctl agent call -Method $method -RequestFile $request -TimeoutSeconds 120
    if ($LASTEXITCODE -ne 0) { throw "Agent call failed: $method $raw" }
    $response = ($raw -join "`n") | ConvertFrom-Json
    if (!$response.ok) { throw "Agent request rejected: $method $raw" }
    return $response.result
}
function Set-Transport([hashtable]$parameters) { Call-Agent 'render.validation.transport' $parameters }
function Warmup { $null = Call-Agent 'runtime.step' @{frames=120} }
function Capture-Event([string]$name, [hashtable]$change, [int]$delay=0) {
    $filename = "$name.pfm"
    $change.capture = @{filename=$filename; delayFrames=$delay}
    $event = Set-Transport $change
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    do {
        $status = Call-Agent 'session.status'
        if ($status.capture.latest.state -eq 'failed') { throw "Capture failed: $name" }
        if ($status.capture.latest.ticket -eq $event.capture.ticket -and $status.capture.latest.state -eq 'completed') { break }
        if ([DateTime]::UtcNow -gt $deadline) { throw "Capture timed out: $name" }
    } while ($true)
    $source = Join-Path $script:session.sessionDirectory "captures/$filename"
    Copy-Item -LiteralPath $source -Destination (Join-Path $output $filename)
    Copy-Item -LiteralPath "$source.world.json" -Destination (Join-Path $output "$filename.world.json")
    # Retain one verified HDR copy; long sequences otherwise duplicate hundreds
    # of MiB inside the disposable owned session. Keep all session receipts.
    $resolvedSource = (Resolve-Path -LiteralPath $source).Path
    $ownedSessions = [IO.Path]::GetFullPath((Join-Path $root 'out/agent/sessions')) + [IO.Path]::DirectorySeparatorChar
    if (!$resolvedSource.StartsWith($ownedSessions, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Capture source escaped the owned session directory.'
    }
    if ((Get-FileHash -LiteralPath $resolvedSource).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $output $filename)).Hash) {
        throw 'Copied HDR capture failed its hash check.'
    }
    Remove-Item -LiteralPath $resolvedSource
    $receipts.Add(@{name=$name; mutation=$event; capture=$status.capture.latest; delayFrames=$delay})
    [IO.File]::WriteAllText((Join-Path $output 'events.json'), ($receipts | ConvertTo-Json -Depth 20))
}
$started = $false
try {
    $sceneFile = Join-Path (Join-Path $root 'Project') $Scene
    $sceneData = Get-Content -LiteralPath $sceneFile -Raw | ConvertFrom-Json
    $materialHashes = @{}
    foreach ($entity in $sceneData.PSObject.Properties.Value) {
        foreach ($material in $entity.MeshComponent.MaterialOverrides) {
            $materialHashes[$material] = (Get-FileHash -LiteralPath (Join-Path (Join-Path $root 'Project') $material) -Algorithm SHA256).Hash
        }
    }
    @{scene=$Scene; sceneSha256=(Get-FileHash -LiteralPath $sceneFile -Algorithm SHA256).Hash;
      settingsSha256=(Get-FileHash -LiteralPath $SettingsPath -Algorithm SHA256).Hash;
      materialSha256=$materialHashes} | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $output 'inputs.json')
    & $ctl agent start -Configuration RelWithDebInfo -StartupScene $Scene -RenderPath Deferred -GraphicsApi DX12 -StrictGraphicsApi -Resolution $Resolution -RenderSettingsPath $SettingsPath -RenderValidation -TimeoutSeconds 300 *> (Join-Path $output 'start.log')
    if ($LASTEXITCODE -ne 0) { throw 'Agent start failed.' }
    $started = $true
    $script:session = Get-Content (Join-Path $root 'out/agent/sessions/active.json') -Raw | ConvertFrom-Json
    $settings = Call-Agent 'render.settings.get'
    [IO.File]::WriteAllText((Join-Path $output 'settings.json'), ($settings | ConvertTo-Json -Depth 25))
    Warmup
    for ($i=0; $i -lt $StaticSamples; ++$i) { Capture-Event ('static-{0:d2}' -f $i) @{uuid=$sun; directionalLux=100000} }
    if ($StaticOnly) { Write-Output "Captured $StaticSamples static observations."; return }
    if ($ReactiveNoiseOnly) {
        # Subpixel secondary motion exercises the short-history window while
        # keeping the reference irradiance practically constant. This samples
        # the reactive state; it is not a continuous-motion video.
        for ($i=0; $i -lt 8; ++$i) {
            Capture-Event ('reactive-noise-{0:d2}' -f $i) @{uuid=$board; position=@(-.05,1,(.00001*($i+1)))} 12
        }
        Write-Output 'Captured 8 static and 8 reactive-window observations.'
        return
    }
    if (!$BoardOnly) {
    foreach ($delay in @(0,1,2,4,8,16,32)) {
        $null = Set-Transport @{uuid=$sun; directionalLux=100000}
        Warmup
        Capture-Event "off-$delay" @{uuid=$sun; directionalLux=0} $delay
        Warmup
        Capture-Event "on-$delay" @{uuid=$sun; directionalLux=100000} $delay
    }
    Warmup
    $baseYaw = [Math]::Atan2(3,5)
    $basePitch = -[Math]::Atan2(2,[Math]::Sqrt(34))
    for ($i=1; $i -le 6; ++$i) {
        Capture-Event "translate-$i" @{uuid=$camera; position=@((3+.08*$i),3,5)}
    }
    for ($i=1; $i -le 6; ++$i) {
        Capture-Event "rotate-$i" @{uuid=$camera; rotation=@($basePitch,($baseYaw+.03*$i),0)}
    }
    Capture-Event 'camera-cut' @{uuid=$camera; position=@(1.5,2,3); rotation=@(-.25,.4,0)}
    Capture-Event 'camera-restore' @{uuid=$camera; position=@(3,3,5); rotation=@($basePitch,$baseYaw,0)}
    }
    Warmup
    foreach ($delay in @(0,4,16,32)) {
        $null = Set-Transport @{uuid=$board; position=@(-.05,1,0)}
        Warmup
        Capture-Event "board-out-$delay" @{uuid=$board; position=@(-.05,1,6)} $delay
    }
    Capture-Event 'board-restore' @{uuid=$board; position=@(-.05,1,0)} 32
    Write-Output "Captured $($receipts.Count) bounded events; use renderer world.frame and requestedRenderFrame, not tool latency."
} finally {
    if ($started) { & $ctl agent stop *> (Join-Path $output 'stop.log') }
}
