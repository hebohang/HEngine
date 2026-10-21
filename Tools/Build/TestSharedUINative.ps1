$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$owner = [Guid]::NewGuid().ToString('N')
$evidence = Join-Path $repo 'out/agent/shared-ui-native'
New-Item -ItemType Directory -Force $evidence | Out-Null
$session = Join-Path $repo 'Tools/Agent/Session.ps1'
function Call-SharedUi([string]$method, [hashtable]$parameters = @{}) {
    $raw = & $session -Action Call -OwnerTag $owner -Method $method -ParamsJson ($parameters | ConvertTo-Json -Compress) -TimeoutSeconds 60
    $response = ($raw -join "`n") | ConvertFrom-Json
    if (!$response.ok) { throw "Native call failed: $method $($response.error.message)" }
    return $response.result
}
function Click-SharedUi([string]$id) {
    $element = Call-SharedUi 'ui.element.rect' @{id=$id}
    if (!$element.visible) { throw "UI element not visible: $id" }
    $null = Call-SharedUi 'input.click' @{x=($element.rect[0]+$element.rect[2]/2);y=($element.rect[1]+$element.rect[3]/2)}
    $null = Call-SharedUi 'runtime.step' @{frames=12}
}
function Pause-SharedUi {
    $null = Call-SharedUi 'input.set' @{held=@('escape')}
    $null = Call-SharedUi 'runtime.step' @{frames=2}
    $null = Call-SharedUi 'input.clear'
    $null = Call-SharedUi 'runtime.step' @{frames=12}
    $element = Call-SharedUi 'ui.element.rect' @{id='resume_button'}
    if (!$element.visible -or $element.rect[2] -le 0) { throw 'Desktop pause panel is not visible.' }
    if ((Call-SharedUi 'session.status').cursorCaptured) { throw 'Pause must release the cursor.' }
}
try {
    $artifact = Join-Path $repo 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    $hash = (Get-FileHash $artifact -Algorithm SHA256).Hash
    & $session -Action Start -Configuration RelWithDebInfo -ExistingBuildSha256 $hash -StartupScene Assets/Scenes/MainGameCSharp.he -CaptureCursor -OwnerTag $owner -TimeoutSeconds 120 *> (Join-Path $evidence 'start.log')
    $null = Call-SharedUi 'runtime.step' @{frames=30}
    if ((Call-SharedUi 'session.status').cursorCaptured) { throw 'Menu must release the cursor.' }
    Click-SharedUi 'hub_nav_armory'
    Click-SharedUi 'armory_slot_0'
    Call-SharedUi 'capture.frame' @{filename='desktop-armory.png'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'armory-capture.json')
    Click-SharedUi 'armory_tab_tomes'
    Click-SharedUi 'armory_filter_owned'
    Click-SharedUi 'armory_slot_0'
    Click-SharedUi 'armory_tab_evolutions'
    Click-SharedUi 'hub_nav_shop'
    Click-SharedUi 'powerup_node_1'
    Call-SharedUi 'capture.frame' @{filename='desktop-shop.png'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'shop-capture.json')
    Click-SharedUi 'hub_nav_quests'
    Click-SharedUi 'quest_slot_0'
    Click-SharedUi 'quest_track_button'
    Call-SharedUi 'capture.frame' @{filename='desktop-quests.png'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'quests-capture.json')
    Click-SharedUi 'quest_page_next'
    Click-SharedUi 'quest_filter_character'
    Click-SharedUi 'hub_nav_collection'
    Click-SharedUi 'collection_tab_bosses'
    Call-SharedUi 'capture.frame' @{filename='desktop-collection.png'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'collection-capture.json')
    Click-SharedUi 'collection_tab_stats'
    Click-SharedUi 'hub_nav_settings'
    Click-SharedUi 'hub_volume_up'
    Click-SharedUi 'hub_vsync_on'
    Click-SharedUi 'hub_vsync_off'
    Click-SharedUi 'hub_ui_scale_large'
    Click-SharedUi 'hub_ui_scale_normal'
    Click-SharedUi 'hub_language_en'
    Click-SharedUi 'hub_language_zh'
    Call-SharedUi 'capture.frame' @{filename='desktop-settings.png'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'settings-capture.json')
    Click-SharedUi 'hub_nav_play'
    Click-SharedUi 'start_button'
    if (!(Call-SharedUi 'session.status').cursorCaptured) { throw 'Gameplay must capture the cursor.' }
    $null = Call-SharedUi 'runtime.step' @{frames=120}
    $null = Call-SharedUi 'runtime.step' @{frames=120}
    $vfx = Call-SharedUi 'vfx.stats'
    $vfx | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $evidence 'vfx-stats.json')
    if ($vfx.totalRecordedDrawCount -le 0) { throw 'Desktop authored effects did not draw.' }
    Call-SharedUi 'capture.frame' @{filename='desktop-combat.png'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'combat-capture.json')
    $scene = Get-Content (Join-Path $repo 'Project/Assets/Scenes/MainGameCSharp.he') -Raw | ConvertFrom-Json
    $cameraId = ($scene.PSObject.Properties | Where-Object { $_.Value.CameraComponent } | Select-Object -First 1).Name
    $null = Call-SharedUi 'input.pointer' @{x=640;y=360;rightHeld=$false}
    $null = Call-SharedUi 'runtime.step' @{frames=12}
    $before = Call-SharedUi 'world.get' @{uuid=$cameraId;components=@('TransformComponent')}
    $null = Call-SharedUi 'input.pointer' @{x=740;y=390;rightHeld=$false}
    $null = Call-SharedUi 'runtime.step' @{frames=12}
    $after = Call-SharedUi 'world.get' @{uuid=$cameraId;components=@('TransformComponent','FollowCameraComponent','SpringArmComponent')}
    @{before=$before;after=$after} | ConvertTo-Json -Depth 15 | Set-Content (Join-Path $evidence 'camera.json')
    if ([Math]::Abs($after.components.TransformComponent.Rotation[1] - $before.components.TransformComponent.Rotation[1]) -lt 0.1) { throw 'Camera did not rotate without holding a mouse button.' }
    if (!$after.components.SpringArmComponent.bEnableCollision -or !$after.components.SpringArmComponent.bEnableLag) { throw 'Desktop camera must retain collision and lag.' }
    $null = Call-SharedUi 'input.clear'
    Pause-SharedUi
    Call-SharedUi 'capture.frame' @{filename='shared-pause.png'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'capture.json')
    Click-SharedUi 'pause_settings_button'
    Click-SharedUi 'volume_down'
    Call-SharedUi 'capture.frame' @{filename='pause-settings.png'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'pause-settings-capture.json')
    Click-SharedUi 'settings_back_button'
    Click-SharedUi 'resume_button'
    if (!(Call-SharedUi 'session.status').cursorCaptured) { throw 'Resume must recapture the cursor.' }
    Pause-SharedUi
    Click-SharedUi 'pause_restart_button'
    Pause-SharedUi
    Click-SharedUi 'pause_home_button'
    Click-SharedUi 'game_over_home_button'
    Click-SharedUi 'start_button'
    $logs = Call-SharedUi 'logs.read' @{limit=100}
    $text = $logs | ConvertTo-Json -Depth 8
    $text | Set-Content (Join-Path $evidence 'logs.json')
    if ($text -match 'Unhandled.*Exception|Script callback.*failed') { throw 'Native script exception observed.' }
    Write-Output 'runtime-manual PASS: original armory/shop/quests/collection, tracking, authored VFX draws, menu cursor, settings, direct mouse look, camera collision/lag enabled, pause/resume/restart/abandon.'
} finally {
    & $session -Action Stop -OwnerTag $owner *> (Join-Path $evidence 'stop.log')
}
