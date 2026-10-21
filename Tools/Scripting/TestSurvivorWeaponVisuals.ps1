$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$session = Join-Path $repo 'Tools/Agent/Session.ps1'
$owner = [Guid]::NewGuid().ToString('N')
$evidence = Join-Path $repo 'out/agent/weapon-visual-native'
New-Item -ItemType Directory -Force $evidence | Out-Null
function Invoke-WeaponCall([string]$method, [hashtable]$parameters = @{}) {
    $raw = & $session -Action Call -OwnerTag $owner -Method $method -ParamsJson ($parameters | ConvertTo-Json -Compress) -TimeoutSeconds 60
    $response = ($raw -join "`n") | ConvertFrom-Json
    if (!$response.ok) { throw "$method failed: $($response.error.message)" }
    return $response.result
}
try {
    $artifact = Join-Path $repo 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    & $session -Action Start -Configuration RelWithDebInfo -ExistingBuildSha256 (Get-FileHash $artifact).Hash -StartupScene Assets/Scenes/MainGameCSharp.he -OwnerTag $owner -TimeoutSeconds 120 *> (Join-Path $evidence 'start.log')
    $null = Invoke-WeaponCall 'runtime.step' @{frames=20}
    $element = Invoke-WeaponCall 'ui.element.rect' @{id='start_button'}
    $null = Invoke-WeaponCall 'input.click' @{x=($element.rect[0]+$element.rect[2]/2);y=($element.rect[1]+$element.rect[3]/2)}
    $null = Invoke-WeaponCall 'runtime.step' @{frames=2}
    foreach ($name in @('MoonBladeToon','FireballChibiToon')) {
        $effect = Invoke-WeaponCall 'vfx.play' @{path="Assets/Effects/Survivor/$name.efkpkg";position=@($(if($name -eq 'MoonBladeToon'){-2}else{2}),0.62,0);scale=$(if($name -eq 'MoonBladeToon'){0.69}else{0.56});seed=42}
        $null = Invoke-WeaponCall 'runtime.step' @{frames=10}
        $status = Invoke-WeaponCall 'vfx.control' @{handle=$effect.handle;action='status'}
        if (!$status.playing) { throw "Particle package stopped before the sample: $name" }
        $status | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $evidence "$name-status.json")
        Invoke-WeaponCall 'capture.frame' @{filename="$name.png"} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence "$name-capture.json")
        $null = Invoke-WeaponCall 'vfx.control' @{handle=$effect.handle;action='stop'}
    }
    Invoke-WeaponCall 'vfx.stats' | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $evidence 'vfx-stats.json')
    Invoke-WeaponCall 'logs.read' @{limit=100} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'logs.json')
    Write-Output 'runtime-manual: captured the two original particle packages in the PC renderer; inspect images and effect status separately from the adapter unit test.'
} finally {
    & $session -Action Stop -OwnerTag $owner *> (Join-Path $evidence 'stop.log')
}
