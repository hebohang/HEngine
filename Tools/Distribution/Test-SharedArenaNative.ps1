param([Parameter(Mandatory=$true)][string]$Engine,[Parameter(Mandatory=$true)][string]$Project,
      [string]$EvidenceRoot='')
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $Engine).Path
. "$root/Tools/Agent/Project.ps1"
$info=Resolve-HEProject $Project $root
$sessionBase=if($EvidenceRoot){[IO.Path]::GetFullPath($EvidenceRoot)}else{"$root/out/agent/projects/$($info.CacheKey)/sessions"}
$sessions=Join-Path $sessionBase ("arena-"+[guid]::NewGuid().ToString('N').Substring(0,8))
$null=New-Item -ItemType Directory -Force -Path $sessions
$session="$root/Tools/Agent/Session.ps1"
function Call([string]$method,$parameters=@{}) {
    $LASTEXITCODE=0
    $json=& $session -Action Call -ProjectRoot $info.Root -SessionRoot $sessions -Method $method -ParamsJson ($parameters | ConvertTo-Json -Compress) -TimeoutSeconds 120
    if($LASTEXITCODE -ne 0){throw "Agent call failed: $method"}
    $response=$json | ConvertFrom-Json
    if(!$response.ok){throw "Agent rejected $method"}
    return $response.result
}
function Click-Action {
    $element=Call 'ui.element.rect' @{id='portable_action_0'}
    if(!$element.visible){throw 'Primary gameplay button is missing'}
    $null=Call 'input.click' @{x=($element.rect[0]+$element.rect[2]/2);y=($element.rect[1]+$element.rect[3]/2)}
    $null=Call 'runtime.step' @{frames=2}
}
try {
    $LASTEXITCODE=0
    & $session -Action Start -ProjectRoot $info.Root -SessionRoot $sessions -BuildProfile windows -Resolution 1280x720 -UseWarp -GraphicsApi DX12 -TimeoutSeconds 120 *> "$sessions/start.log"
    if($LASTEXITCODE -ne 0){throw "Startup failed: $sessions/start.log"}
    Click-Action
    $entities=Call 'world.list' @{limit=100}
    $player=$null
    foreach($entity in $entities.entities | Where-Object name -eq 'PortableSprite') {
        $data=Call 'world.get' @{uuid=$entity.uuid;components=@('TransformComponent','SpriteRendererComponent')}
        $color=$data.components.SpriteRendererComponent.Color
        if($color -and $color[2]-$color[0] -gt .2){$player=$data;break}
    }
    if(!$player){throw 'Blue player sprite was not spawned'}
    $null=Call 'input.set' @{held=@('d')};$null=Call 'runtime.step' @{frames=30};$null=Call 'input.clear'
    $moved=Call 'world.get' @{uuid=$player.uuid;components=@('TransformComponent')}
    if($moved.worldMatrix[12]-$player.worldMatrix[12] -lt 1){throw 'Player did not move after Start'}
    $capture=Call 'capture.frame' @{filename='shared-arena-game.png'}
    $null=Call 'runtime.step' @{frames=10}
    $settledCapture=Call 'capture.frame' @{filename='shared-arena-game-settled.png'}
    Click-Action
    $paused=Call 'world.get' @{uuid=$player.uuid;components=@('TransformComponent')}
    $null=Call 'input.set' @{held=@('d')};$null=Call 'runtime.step' @{frames=30};$null=Call 'input.clear'
    $still=Call 'world.get' @{uuid=$player.uuid;components=@('TransformComponent')}
    if([Math]::Abs($paused.worldMatrix[12]-$still.worldMatrix[12]) -gt .001){throw 'Paused game moved'}
    Click-Action
    $null=Call 'input.set' @{held=@('a')};$null=Call 'runtime.step' @{frames=30};$null=Call 'input.clear'
    $resumed=Call 'world.get' @{uuid=$player.uuid;components=@('TransformComponent')}
    if($still.worldMatrix[12]-$resumed.worldMatrix[12] -lt 1){throw 'Resume button did not resume movement'}
    [ordered]@{schema=1;pass=$true;evidence='test-layer';project=$info.Root;capture=$capture;settledCapture=$settledCapture;
        before=$player.worldMatrix;moved=$moved.worldMatrix;paused=$still.worldMatrix;resumed=$resumed.worldMatrix;
        validation='Actual native C# game, UI Start/Pause/Resume clicks, movement and paused-state assertions; screenshot visual review separate'} |
        ConvertTo-Json -Depth 16 | Set-Content -LiteralPath "$sessions/result.json" -Encoding UTF8
    Write-Output "SharedArena native interaction PASS: $sessions/result.json"
} finally {
    if(Test-Path -LiteralPath "$sessions/active.json"){
        $LASTEXITCODE=0
        & $session -Action Stop -ProjectRoot $info.Root -SessionRoot $sessions *> "$sessions/stop.log"
        if($LASTEXITCODE -ne 0){throw "Session cleanup failed: $sessions/stop.log"}
    }
}
