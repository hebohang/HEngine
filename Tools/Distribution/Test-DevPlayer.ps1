param(
    [Parameter(Mandatory=$true)][string]$Engine,
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$BuildProfile = 'windows',
    [string]$MovementEntity = '',
    [switch]$UseWarp
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Engine).Path
if (!(Test-Path -LiteralPath "$root/HEngine.distribution.json")) { throw 'Validate the development player from a binary installation.' }
foreach ($forbidden in @('CMakeLists.txt','Engine/Source/Runtime/Launch','Engine/Source/Game','Project')) {
    if (Test-Path -LiteralPath "$root/$forbidden") { throw "Source-free fixture contains $forbidden" }
}
. "$root/Tools/Agent/Project.ps1"
. "$root/Tools/Distribution/Template.ps1"
$info = Resolve-HEProject $Project $root
$platform = Get-HEWindowsTemplatePlatform $root
$template = Get-HEExportTemplate $root $platform "$root/DevelopmentTemplates/$platform" -Development
$sessions = "$root/out/agent/projects/$($info.CacheKey)/sessions/dev-$([guid]::NewGuid().ToString('N').Substring(0,8))"
$null = New-Item -ItemType Directory -Path $sessions -Force
$session = "$root/Tools/Agent/Session.ps1"
$oldPath = $env:PATH
function Call-Session([string]$Method, [string]$Params = '{}') {
    $LASTEXITCODE = 0
    $output = & $session -Action Call `
        -SessionRoot $sessions -ProjectRoot $info.Root -Method $Method -ParamsJson $Params -TimeoutSeconds 120
    if ($LASTEXITCODE -ne 0) { throw "Agent call failed: $Method ($output)" }
    $response = $output | ConvertFrom-Json
    if (!$response.ok) { throw "Agent rejected: $Method" }
    return $response.result
}
try {
    $env:PATH = ($oldPath.Split(';') | Where-Object { $_ -notmatch '(?i)Visual Studio|VC[\\/]Tools' }) -join ';'
    $start = @{Action='Start';ProjectRoot=$info.Root;SessionRoot=$sessions;BuildProfile=$BuildProfile;
        TimeoutSeconds=120;Resolution='640x360';GraphicsApi='DX12';UseWarp=$UseWarp}
    $LASTEXITCODE = 0
    & $session @start *> "$sessions/start.log"
    $startExit = $LASTEXITCODE
    if ($startExit -ne 0) { throw "Development player startup failed; see $sessions/start.log" }
    $owner = Get-Content -Raw -Encoding UTF8 -LiteralPath "$sessions/active.json" | ConvertFrom-Json
    $expected = (Get-FileHash -LiteralPath $template.Files[$template.Manifest.Player]).Hash
    if ((Get-FileHash -LiteralPath $owner.artifact).Hash -cne $expected) { throw 'Development session rebuilt or changed the native player.' }
    $discovery = Call-Session 'api.discover'
    $discovery | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath "$sessions/discovery.json" -Encoding UTF8
    $before = Call-Session 'session.status'
    $entityQuery = @{uuid=$MovementEntity;components=@('TransformComponent')} | ConvertTo-Json -Compress
    $entityBefore = if ($MovementEntity) { Call-Session 'world.get' $entityQuery } else { $null }
    $null = Call-Session 'input.set' '{"held":["w"]}'
    $null = Call-Session 'runtime.step' '{"frames":30}'
    $null = Call-Session 'input.clear'
    $after = Call-Session 'session.status'
    $entityAfter = if ($MovementEntity) { Call-Session 'world.get' $entityQuery } else { $null }
    if ($MovementEntity) {
        if (!$entityBefore.found -or !$entityAfter.found) { throw 'Movement validation entity was not found.' }
        $distance = 0.0
        foreach ($index in 12..14) { $distance += [Math]::Pow(([double]$entityAfter.worldMatrix[$index] - [double]$entityBefore.worldMatrix[$index]),2) }
        if ($distance -lt 0.0001) { throw 'Injected input did not move the gameplay entity.' }
    }
    $capture = Call-Session 'capture.frame' '{"filename":"sdk-dev-player.png"}'
    [ordered]@{schema=1;evidence='test-layer';pass=$true;engine=$root;project=$info.Root;
        playerSha256=$expected.ToLowerInvariant();nativeCompilation=$false;before=$before;after=$after;capture=$capture;
        entityBefore=$entityBefore;entityAfter=$entityAfter;movementVerified=[bool]$MovementEntity;
        validation='Script compilation, Agent discovery, input injection, frame stepping and capture; gameplay assertions are separate.'} |
        ConvertTo-Json -Depth 24 | Set-Content -LiteralPath "$sessions/result.json" -Encoding UTF8
    Write-Output "Source-free development session PASS: $sessions/result.json"
} catch {
    [IO.File]::WriteAllText("$sessions/failure.log",($_ | Out-String),[Text.UTF8Encoding]::new($false))
    throw
} finally {
    try {
        if (Test-Path -LiteralPath "$sessions/active.json") {
            $LASTEXITCODE = 0
            & $session -Action Stop -SessionRoot $sessions -ProjectRoot $info.Root *> "$sessions/stop.log"
            if ($LASTEXITCODE -ne 0) { throw "Session cleanup failed; see $sessions/stop.log" }
        }
    } finally { $env:PATH = $oldPath }
}
