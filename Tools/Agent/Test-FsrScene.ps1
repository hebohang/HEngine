[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$StartupScene,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
    [ValidateSet('Debug','RelWithDebInfo')][string]$Configuration='RelWithDebInfo'
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$sessionTool=Join-Path $PSScriptRoot 'Session.ps1'
$contextJson=& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') project current -Project $Project -Configuration $Configuration -Json
if($LASTEXITCODE -ne 0){throw 'Project resolution failed.'}
$context=($contextJson -join "`n") | ConvertFrom-Json
$common=@{Configuration=$Configuration;ProjectRoot=$context.project.Root;
    SessionRoot=$context.agentSessions;TimeoutSeconds=180;OwnerTag=[guid]::NewGuid().ToString('N');
    ArtifactPath=(Join-Path $context.gameBuild ('bin/'+$context.project.ExecutableName+'.exe'))}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun=Start-HEStorageRun -Project $context.project.Root -Workflow 'fsr-scene' -Paths @($OutputDirectory) -Outer
$state='failed'; $started=$false
$receipts=[Collections.Generic.List[object]]::new()
$expectedSize=@(2560,1440)
function Invoke-Check([string]$method,[hashtable]$parameters=@{}) {
    $lines=@(& $sessionTool @common -Action Call -Method $method -ParamsJson (ConvertTo-Json -InputObject $parameters -Depth 16 -Compress))
    $response=($lines | Select-Object -Last 1) | ConvertFrom-Json
    if(!$response.ok){throw ($response | ConvertTo-Json -Depth 8 -Compress)}
    return $response.result
}
function Set-Settings([hashtable]$patch) {
    $snapshot=Invoke-Check 'render.settings.get'
    $apply=Invoke-Check 'render.settings.apply' @{baseRevision=$snapshot.revision;baseHash=$snapshot.hash;patch=$patch}
    if(!$apply.success){throw ($apply | ConvertTo-Json -Depth 12 -Compress)}
    $effective=(Invoke-Check 'render.settings.get').effective
    foreach($key in $patch.Keys){
        $matches=if($patch[$key] -is [double] -or $patch[$key] -is [single]){
            [Math]::Abs([double]$effective.$key-[double]$patch[$key]) -lt 0.00001
        } else {$effective.$key -eq $patch[$key]}
        if(!$matches){throw "Setting $key is overridden; this run cannot prove switching."}
    }
    return $apply
}
function Save-Frame([string]$name,[string]$quality='Performance',[double]$sharpness=0.2,[bool]$active=$true) {
    $capture=Invoke-Check 'capture.frame' @{filename=($name+'.png');delayFrames=8}
    $world=Get-Content -LiteralPath ($capture.path+'.world.json') -Raw | ConvertFrom-Json
    $raster=$world.recordedRaster; $fsr=$raster.upscaling
    if(!$raster.valid -or $fsr.active -ne $active -or $raster.taaActive -eq $active){throw "${name}: incorrect FSR/TAA selection."}
    if($active){
        $ratio=@{NativeAA=1.0;Quality=1.5;Balanced=1.7;Performance=2.0;UltraPerformance=3.0}[$quality]
        $width=[Math]::Max(1,[Math]::Floor([single]$expectedSize[0]/[single]$ratio))
        $height=[Math]::Max(1,[Math]::Floor([single]$expectedSize[1]/[single]$ratio))
        if($fsr.quality -ne $quality -or $fsr.renderSize[0] -ne $width -or $fsr.renderSize[1] -ne $height -or
            $fsr.outputSize[0] -ne $expectedSize[0] -or $fsr.outputSize[1] -ne $expectedSize[1] -or
            [Math]::Abs($fsr.sharpness-$sharpness) -gt 0.00001 -or $fsr.failure -or $fsr.preExposure -ne 1){
            throw "${name}: raster metadata does not match the requested reconstruction."
        }
    }
    $destination=Join-Path $OutputDirectory ($name+'.png')
    Copy-Item -LiteralPath $capture.path -Destination $destination
    Copy-Item -LiteralPath ($capture.path+'.world.json') -Destination ($destination+'.world.json')
    $capture.path=$destination
    $receipts.Add(@{test=$name;capture=$capture;upscaling=$fsr;taaActive=$raster.taaActive})
    Write-Host "FSR scene $GraphicsApi ${name}: PASS"
}
try {
    # Keep FSR settings out of CommandLine: that layer overrides Session edits.
    & $sessionTool @common -Action Start -GraphicsApi $GraphicsApi -StrictGraphicsApi -RenderValidation -Resolution 2560x1440 -StartupScene $StartupScene
    $started=$true
    $null=Invoke-Check 'runtime.pause'
    $null=Set-Settings @{'Rendering.Upscaling.Mode'='FSR3.1';'Rendering.Upscaling.Quality'='Performance';
        'Rendering.Upscaling.Sharpness'=0.2;'Rendering.AntiAliasing.Mode'='TAA';'Rendering.GI.Technique'='Off';'Rendering.Resolution.RenderHeight'=0}
    Save-Frame 'baseline'
    foreach($quality in @('Quality','Balanced','Performance','UltraPerformance','NativeAA')) {
        $apply=Set-Settings @{'Rendering.Upscaling.Quality'=$quality}
        if(!$apply.changePlan.renderTargetRecreateRequired){throw 'Quality did not recreate resolution-dependent resources.'}
        Save-Frame $quality $quality
    }
    foreach($mode in @('Off','FSR3.1')) {
        $null=Set-Settings @{'Rendering.Upscaling.Mode'=$mode;'Rendering.Upscaling.Quality'='Performance'}
        Save-Frame $mode 'Performance' 0.2 ($mode -ne 'Off')
        $passes=@(); $offset=0; $snapshot=$null
        do {
            $request=@{section='passes';offset=$offset;limit=100}
            if($snapshot){$request.snapshotId=$snapshot}
            $page=Invoke-Check 'render.graph.snapshot' $request
            $passes+=@($page.items); $offset=$page.nextOffset; $snapshot=$page.snapshotId
        } while($null -ne $offset)
        $names=@($passes | Where-Object {!$_.culled} | ForEach-Object {$_.name})
        if(($names -contains 'FSR31Upscale') -ne ($mode -ne 'Off') -or
            ($names -contains 'FSRBeginTransparency') -ne ($mode -ne 'Off') -or
            ($names -contains 'FSRPrepareInputs') -ne ($mode -ne 'Off') -or
            ($names -contains 'SceneTAA') -ne ($mode -eq 'Off')){
            throw 'Executing passes disagree with the upscaling mode.'
        }
        $passes | ConvertTo-Json -Depth 16 | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory ($mode+'-passes.json'))
    }
    foreach($sharpness in @(0.0,1.0)) {
        $apply=Set-Settings @{'Rendering.Upscaling.Sharpness'=$sharpness}
        if($apply.changePlan.renderTargetRecreateRequired -or $apply.changePlan.temporalHistoryResetRequired){throw 'Sharpness unnecessarily invalidated temporal resources.'}
        Save-Frame ('sharpness-'+$sharpness) 'Performance' $sharpness
    }
    foreach($source in @('motion','reactive','composition')) {
        $capture=Invoke-Check 'capture.frame' @{filename=($source+'.raw');source=$source;delayFrames=1}
        $destination=Join-Path $OutputDirectory ($source+'.raw')
        foreach($suffix in @('','.json','.world.json')) {Copy-Item -LiteralPath ($capture.path+$suffix) -Destination ($destination+$suffix)}
        $capture.path=$destination
        $receipts.Add(@{test=$source;capture=$capture})
    }
    foreach($size in @(@(3840,2160),@(1920,1080),@(2560,1440))) {
        $null=Invoke-Check 'window.resize' @{width=$size[0];height=$size[1]}
        $expectedSize=$size
        Save-Frame ('resize-'+($size -join 'x')) 'Performance' 1.0
    }
    $sequence=Invoke-Check 'capture.sequence' @{prefix='camera-cut';frames=8;cameraCut=$true;settleFrames=0}
    for($i=0;$i -lt $sequence.frames.Count;$i++) {
        $frame=$sequence.frames[$i]
        $world=Get-Content -LiteralPath ($frame.path+'.world.json') -Raw | ConvertFrom-Json
        $raster=$world.recordedRaster
        if(!$raster.valid -or $raster.upscaling.reset -ne ($i -eq 0) -or $raster.upscaling.submittedFrames -ne $i){throw 'Camera cut did not reset/advance FSR history exactly once.'}
        $destination=Join-Path $OutputDirectory ('camera-cut-'+$i+'.png')
        foreach($suffix in @('','.world.json')) {Copy-Item -LiteralPath ($frame.path+$suffix) -Destination ($destination+$suffix)}
        $frame.path=$destination
    }
    $receipts.Add(@{test='camera-cut';capture=$sequence})
    $state='success'
} finally {
    try {
        $receipts | ConvertTo-Json -Depth 32 | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory 'receipts.json')
        if($started){ & $sessionTool @common -Action Stop }
    } finally {Complete-HEStorageRun -Run $storageRun -State $state}
}
Write-Host 'FSR scene contracts: PASS. Review images/masks separately; this is not a ghosting, UI, particle or deformation quality acceptance.'
