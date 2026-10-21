[CmdletBinding()]
param([ValidateSet('DX12','Vulkan')][string]$GraphicsApi = 'DX12', [switch]$FailureProbe)
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output = Join-Path $repository ('out/agent/rendering-evidence/cloud-flow-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun = Start-HEStorageRun -Project (Join-Path $repository 'Project') -Workflow 'cloud-flow' -Paths @($output) -Outer
$storageState = 'failed'
$sessionArgs = @{SessionRoot=(Join-Path $repository 'out/agent/sessions/cloud-validation');
    OwnerTag=[guid]::NewGuid().ToString('N');Configuration='RelWithDebInfo';TimeoutSeconds=180}
$sessionScript = Join-Path $PSScriptRoot 'Session.ps1'
$transcript = [Collections.Generic.List[object]]::new()
function Call-CloudValidation([string]$Method, $Parameters = @{}) {
    $lines = @(& $sessionScript @sessionArgs -Action Call -Method $Method -ParamsJson ($Parameters | ConvertTo-Json -Depth 20 -Compress))
    $reply = $lines | Where-Object { $_ -is [string] -and $_.StartsWith('{') } | Select-Object -Last 1 | ConvertFrom-Json
    $transcript.Add(@{method=$Method;params=$Parameters;reply=$reply})
    if (!$reply.ok) { throw ($Method + ': ' + ($reply.error | ConvertTo-Json -Compress)) }
    return $reply.result
}
function Step-CloudValidation([int]$Frames = 1) { $null = Call-CloudValidation 'runtime.step' @{frames=$Frames} }
function Capture-CloudValidation([string]$Name) {
    $capture = Call-CloudValidation 'capture.frame' @{filename=($Name+'.png')}
    Copy-Item -LiteralPath $capture.path -Destination (Join-Path $output ($Name+'.png'))
}
function Click-CloudValidation([string]$Id) {
    $element = Call-CloudValidation 'ui.element.rect' @{id=$Id}
    if (!$element.visible) { throw ('Expected visible UI element: '+$Id) }
    $null = Call-CloudValidation 'input.click' @{x=($element.rect[0]+$element.rect[2]/2);y=($element.rect[1]+$element.rect[3]/2);button='left'}
}
function Settings-CloudValidation($Patch) {
    $current = Call-CloudValidation 'render.settings.get'
    $result = Call-CloudValidation 'render.settings.apply' @{layer='Session';baseRevision=$current.revision;baseHash=$current.hash;patch=$Patch;persist=$false}
    $transcript.Add(@{settingsResult=$result})
    if (!$result.success) { throw ('Cloud settings transaction failed: ' + ($result | ConvertTo-Json -Depth 10 -Compress)) }
}
function Key-CloudValidation([string]$Key) {
    $null = Call-CloudValidation 'input.set' @{held=@($Key);onStep=$true}
    Step-CloudValidation
    $null = Call-CloudValidation 'input.clear'
    Step-CloudValidation 30
}
$started = $false
$previousFault = $env:HE_DEV_RENDER_FAIL_CANDIDATE_ONCE
try {
    if ($FailureProbe) { $env:HE_DEV_RENDER_FAIL_CANDIDATE_ONCE = '1' }
    & $sessionScript @sessionArgs -Action Start -GraphicsApi $GraphicsApi -StrictGraphicsApi -RenderStats -RenderValidation
    $started = $true
    Step-CloudValidation 120
    Capture-CloudValidation 'menu'
    Click-CloudValidation 'start_button'
    for ($i=0;$i -lt 5;$i++) {
        Step-CloudValidation 120
        $world = Call-CloudValidation 'world.summary'
        if ($world.scene -eq 'Assets/Scenes/MainGame.he') { break }
    }
    if ($world.scene -ne 'Assets/Scenes/MainGame.he') { throw 'Menu launch did not commit gameplay' }
    Step-CloudValidation 120
    Capture-CloudValidation 'default-on'
    if ($FailureProbe) {
        $before = Call-CloudValidation 'render.settings.get'
        $failed = Call-CloudValidation 'render.settings.apply' @{layer='Session';baseRevision=$before.revision;
            baseHash=$before.hash;patch=@{'Rendering.Clouds.Quality'='Off'};persist=$false}
        $after = Call-CloudValidation 'render.settings.get'
        if ($failed.success -or $after.hash -ne $before.hash -or $after.revision -ne $before.revision -or
            $after.effective.'Rendering.Clouds.Quality' -ne 'Balanced') { throw 'Cloud candidate rollback failed' }
        Capture-CloudValidation 'rollback-retained'
    }
    Settings-CloudValidation @{'Rendering.Clouds.Quality'='Off'}
    Capture-CloudValidation 'default-off'
    Settings-CloudValidation @{'Rendering.Clouds.Quality'='Balanced'}
    $null = Call-CloudValidation 'input.set' @{held=@('a')}
    Step-CloudValidation 120
    $null = Call-CloudValidation 'input.clear'
    Capture-CloudValidation 'moved'
    $null = Call-CloudValidation 'input.pointer' @{x=640;y=360;rightHeld=$false}
    Step-CloudValidation
    for ($i=0;$i -le 12;$i++) {
        $null = Call-CloudValidation 'input.pointer' @{x=(640+$i*20);y=(360-$i*15);rightHeld=$true}
        Step-CloudValidation
        Capture-CloudValidation ('motion-'+$i.ToString('00'))
    }
    $null = Call-CloudValidation 'input.clear'
    Capture-CloudValidation 'sky-on'
    Settings-CloudValidation @{'Rendering.Clouds.Quality'='Off'}
    Capture-CloudValidation 'sky-off'
    Settings-CloudValidation @{'Rendering.Clouds.Quality'='Balanced'}
    $null = Call-CloudValidation 'camera.cut'
    Capture-CloudValidation 'camera-cut'
    foreach ($aa in @('None','TAA','SMAA','FXAA')) {
        Settings-CloudValidation @{'Rendering.AntiAliasing.Mode'=$aa}
        Capture-CloudValidation ('aa-'+$aa)
    }
    foreach ($quality in @('Low','High','Balanced','Off','Balanced','High','Low','Balanced')) {
        Settings-CloudValidation @{'Rendering.Clouds.Quality'=$quality}
        Step-CloudValidation 2
    }
    $null = Call-CloudValidation 'render.view.create' @{width=641;height=359}
    Step-CloudValidation 8
    $null = Call-CloudValidation 'render.view.capture'
    Step-CloudValidation 8
    $view = Call-CloudValidation 'render.view.status'
    if ($view.capture.status -ne 'complete' -or $view.passes -notcontains 'CloudRayMarch') {
        throw 'Independent view did not submit and capture its own cloud graph'
    }
    Copy-Item -LiteralPath $view.capture.path -Destination (Join-Path $output 'independent-view.png')
    $null = Call-CloudValidation 'render.view.destroy'
    Settings-CloudValidation @{'Rendering.Path'='Forward';'Rendering.Occlusion.Enabled'=$false;
        'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false;
        'Rendering.AntiAliasing.Mode'='MSAA';'Rendering.AntiAliasing.MsaaSamples'=4}
    Capture-CloudValidation 'forward-msaa4'
    $msaa = Call-CloudValidation 'render.settings.get'
    if ($msaa.effective.'Rendering.AntiAliasing.Mode' -ne 'MSAA' -or
        $msaa.effective.'Rendering.Path' -ne 'Forward') { throw 'Forward MSAA fell back' }
    Settings-CloudValidation @{'Rendering.Path'='Deferred';'Rendering.AntiAliasing.Mode'='FXAA';
        'Rendering.Occlusion.Enabled'=$true;'Rendering.Reflections.Probes.Enabled'=$true}
    $null = Call-CloudValidation 'window.resize' @{width=1279;height=719}
    Capture-CloudValidation 'odd-size'
    $null = Call-CloudValidation 'window.resize' @{width=1280;height=720}
    Key-CloudValidation 'escape'
    Capture-CloudValidation 'paused'
    Click-CloudValidation 'resume_button'
    Step-CloudValidation 60
    Key-CloudValidation 'escape'
    Click-CloudValidation 'pause_restart_button'
    Step-CloudValidation 120
    Capture-CloudValidation 'restart'
    Key-CloudValidation 'escape'
    Click-CloudValidation 'pause_home_button'
    Step-CloudValidation 120
    Capture-CloudValidation 'returned-menu'
    $null = Call-CloudValidation 'world.summary'
    $null = Call-CloudValidation 'render.graph.snapshot' @{section='passes';limit=100}
    Write-Host ('Cloud flow completed: '+$output)
    $storageState = 'success'
}
finally {
    $env:HE_DEV_RENDER_FAIL_CANDIDATE_ONCE = $previousFault
    $transcript | ConvertTo-Json -Depth 40 | Set-Content -Encoding utf8 (Join-Path $output 'transcript.json')
    if ($started) { & $sessionScript @sessionArgs -Action Stop }
    Complete-HEStorageRun -Run $storageRun -State $storageState
}
