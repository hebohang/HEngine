param(
    [Parameter(Mandatory=$true)][string]$Engine,
    [Parameter(Mandatory=$true)][string]$Evidence,
    [switch]$UseWarp
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Engine).Path
if (!(Test-Path -LiteralPath "$root/HEngine.distribution.json")) { throw 'Use an assembled binary SDK.' }
$work = [IO.Path]::GetFullPath($Evidence)
if (Test-Path -LiteralPath $work) { throw 'Use a fresh evidence directory.' }
$null = New-Item -ItemType Directory -Path $work
$project = "$work/Project"
& "$root/hectl.bat" project create -Project $project -Name ScriptSdkProbe -Template Scripted3D *> "$work/create.log"
if ($LASTEXITCODE -ne 0) { throw 'Could not create the SDK template project.' }
$fixtureArguments = @("$root/Tools/Distribution/Create-ScriptSdkFixture.py", '--engine', $root, '--project', $project)
& py -3.11 @fixtureArguments
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare API probes.' }
$sessions = "$work/session"
$session = "$root/Tools/Agent/Session.ps1"
function Call-Probe([string]$Method, [string]$Params = '{}') {
    $response = & $session -Action Call -SessionRoot $sessions -ProjectRoot $project -Method $Method -ParamsJson $Params -TimeoutSeconds 120
    if ($LASTEXITCODE -ne 0) { throw "Probe command failed: $Method" }
    $data = $response | ConvertFrom-Json
    if (!$data.ok) { throw "Probe rejected: $Method" }
    return $data.result
}
try {
    & $session -Action Start -ProjectRoot $project -SessionRoot $sessions -BuildProfile windows -TimeoutSeconds 120 -Resolution 640x360 -GraphicsApi DX12 -UseWarp:$UseWarp *> "$work/start.log"
    if ($LASTEXITCODE -ne 0) { throw "SDK startup failed; see $work/start.log" }
    $null = Call-Probe 'runtime.step' '{"frames":3}'
    # Action edges belong to presentation frames; do not let paused rendering
    # consume Pressed before the simulation gets its next explicit step.
    $null = Call-Probe 'runtime.resume'
    $null = Call-Probe 'input.set' '{"held":["e"]}'
    $null = Call-Probe 'session.status'
    $null = Call-Probe 'input.clear'
    $null = Call-Probe 'runtime.pause'
    $capture = Call-Probe 'capture.frame' '{"filename":"script-sdk-probe.png"}'
    $capture | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$work/capture.json" -Encoding UTF8
    $owner = Get-Content -Raw -LiteralPath "$sessions/active.json" | ConvertFrom-Json
    $saved = Join-Path $owner.runDirectory 'Saved/Scripting'
    $resultFile = "$saved/sdk-api-smoke.txt"
    if (!(Test-Path -LiteralPath $resultFile)) { throw 'The gameplay script did not reach its success marker.' }
    $result = Get-Content -Raw -LiteralPath $resultFile | ConvertFrom-Json
    if (!$result.passed) { throw 'Script API probe failed.' }
    if (!(Test-Path -LiteralPath "$saved/sdk-input.txt")) { throw 'InputAction press was not observed during running simulation.' }
    if ((Get-Content -Raw -LiteralPath "$saved/sdk-input.txt").Trim() -ne '1') { throw 'Input edge did not reach the script exactly once.' }
    $result | Add-Member -MemberType NoteProperty -Name evidence -Value 'test-layer'
    $result | Add-Member -MemberType NoteProperty -Name audioNote -Value 'Silent PCM fixture verifies playback state, not audible quality.'
    $result | Add-Member -MemberType NoteProperty -Name developmentPlayerSha256 -Value (Get-FileHash -LiteralPath $owner.artifact).Hash.ToLowerInvariant()
    $result | Add-Member -MemberType NoteProperty -Name capture -Value $capture
} finally {
    if (Test-Path -LiteralPath "$sessions/active.json") {
        & $session -Action Stop -SessionRoot $sessions -ProjectRoot $project *> "$work/stop.log"
        if ($LASTEXITCODE -ne 0) { throw "Owned session cleanup failed; see $work/stop.log" }
    }
}
& "$root/Tools/Distribution/Test-WindowsExporter.ps1" -Project $project -ValidationRoot $root -SkipAssembly *> "$work/shipping-export.log"
if ($LASTEXITCODE -ne 0) { throw "Source-free Shipping export failed; see $work/shipping-export.log" }
. "$root/Tools/Agent/Project.ps1"
$info = Resolve-HEProject $project $root
$receipt = Get-Content -Raw -LiteralPath "$project/Saved/Build/windows.json" | ConvertFrom-Json
if (!$receipt.success) { throw 'Shipping export did not produce a success receipt.' }
& "$root/Tools/Packaging/Test-PackagedGame.ps1" -PackageDir $receipt.output -ExecutableName $info.ExecutableName `
    -LogDirectory "$work/shipping-runtime" -UseWarp:$UseWarp -RequiredLogMarker 'SDK_API_SMOKE_PASS' *> "$work/shipping-runtime.log"
if ($LASTEXITCODE -ne 0) { throw "Shipping API smoke failed; see $work/shipping-runtime.log" }
$result | Add-Member -MemberType NoteProperty -Name shipping -Value $true
$result | ConvertTo-Json | Set-Content -LiteralPath "$work/result.json" -Encoding UTF8
Write-Output "Script SDK development and Shipping gameplay PASS: $work/result.json"
