$ErrorActionPreference='Stop'
$owner=[Guid]::NewGuid().ToString('N')
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-Location $repo
$evidence=Join-Path $repo 'out/agent/survivor-native/latest'
New-Item -ItemType Directory -Force $evidence | Out-Null
$session=Join-Path (Get-Location) 'Tools/Agent/Session.ps1'
function Call([string]$method,[hashtable]$parameters=@{}) {
    $raw=& $session -Action Call -OwnerTag $owner -Method $method -ParamsJson ($parameters|ConvertTo-Json -Compress) -TimeoutSeconds 60
    $raw | Set-Content -LiteralPath (Join-Path $evidence ($method+'.json')) -Encoding UTF8
    $response=($raw -join "`n")|ConvertFrom-Json
    if(!$response.ok){throw "Native call failed: $method $($response.error.message)"}
    return $response.result
}
function Click([string]$id){
    $element=Call 'ui.element.rect' @{id=$id}
    if(!$element.visible){throw "UI element not visible: $id"}
    $null=Call 'input.click' @{x=($element.rect[0]+$element.rect[2]/2);y=($element.rect[1]+$element.rect[3]/2)}
    $null=Call 'runtime.step' @{frames=12}
}
try {
    $hash=(Get-FileHash out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe -Algorithm SHA256).Hash
    & $session -Action Start -Configuration RelWithDebInfo -ExistingBuildSha256 $hash -StartupScene Assets/Scenes/MainGameCSharp.he -OwnerTag $owner -TimeoutSeconds 120 *> (Join-Path $evidence 'start.log')
    $null=Call 'runtime.step' @{frames=30}
    $null=Call 'capture.frame' @{filename='menu.png'}
    Click 'hub_nav_characters'
    $null=Call 'capture.frame' @{filename='characters.png'}
    Click 'hub_nav_play'
    Click 'start_button'
    $null=Call 'input.set' @{held=@('d')}
    $null=Call 'runtime.step' @{frames=120}
    $null=Call 'input.clear'
    $null=Call 'capture.frame' @{filename='playing.png'}
    $null=Call 'input.pointer' @{x=640;y=400;rightHeld=$true}
    $null=Call 'runtime.step' @{frames=1}
    $null=Call 'input.pointer' @{x=700;y=420;rightHeld=$true}
    $null=Call 'runtime.step' @{frames=2}
    $null=Call 'input.clear'
    $null=Call 'capture.frame' @{filename='orbit.png'}
    $null=Call 'input.set' @{held=@('escape')}
    $null=Call 'runtime.step' @{frames=2}
    $null=Call 'input.clear'
    $null=Call 'runtime.step' @{frames=2}
    $null=Call 'capture.frame' @{filename='paused.png'}
    Click 'resume_button'
    $upgraded=$false
    for($attempt=0;$attempt -lt 20;$attempt++) {
        $null=Call 'runtime.step' @{frames=120}
        $upgrade=Call 'ui.element.rect' @{id='upgrade_1'}
        if($upgrade.visible){$null=Call 'capture.frame' @{filename='upgrade.png'};Click 'upgrade_1';$upgraded=$true;break}
    }
    if(!$upgraded){throw 'Native combat did not reach an experience upgrade.'}
    $null=Call 'input.set' @{held=@('escape')}
    $null=Call 'runtime.step' @{frames=2}
    $null=Call 'input.clear'
    $null=Call 'runtime.step' @{frames=8}
    Click 'pause_home_button'
    $null=Call 'capture.frame' @{filename='result.png'}
    Click 'game_over_home_button'
    $null=Call 'capture.frame' @{filename='saved-menu.png'}
    Click 'start_button'
    $null=Call 'runtime.step' @{frames=60}
    $null=Call 'capture.frame' @{filename='second-run.png'}
    $logs=Call 'logs.read' @{limit=100}
    $logText=$logs|ConvertTo-Json -Depth 8
    if($logText -match 'Unhandled.*Exception|Script callback.*failed'){throw 'Native script exception observed.'}

    Write-Output 'runtime-manual native interaction sequence completed; review captures and logs.'
} finally { & $session -Action Stop -OwnerTag $owner *> (Join-Path $evidence 'stop.log') }
