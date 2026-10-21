[CmdletBinding()]
param(
    [string]$Project = '',
    [Parameter(Mandatory=$true)][string]$BenchmarkInputPath,
    [Parameter(Mandatory=$true)][string]$RenderSettingsPath,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
    [ValidateRange(1,3600)][int]$Frames=180,
    [ValidatePattern('^[1-9][0-9]{0,3}x[1-9][0-9]{0,3}$')][string]$Resolution='3840x2160'
)
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $PSScriptRoot 'Project.ps1')
if (!$Project) { $Project = Join-Path $repositoryRoot 'Examples/RenderingTests' }
$projectInfo = Resolve-HEProject $Project $repositoryRoot
$projectRoot = $projectInfo.Root
$gameBuild = if ($projectRoot -ieq (Join-Path $repositoryRoot 'Project')) { 'out/agent/game-relwithdebinfo' } else { "out/agent/projects/$($projectInfo.CacheKey)/game-relwithdebinfo" }
$gameExecutable = Join-Path $repositoryRoot "$gameBuild/bin/$($projectInfo.ExecutableName).exe"
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new capture directory.'}
$inputData=Get-Content -LiteralPath $BenchmarkInputPath -Raw | ConvertFrom-Json
$quality=Get-Content -LiteralPath $RenderSettingsPath -Raw | ConvertFrom-Json
$dimensions=$Resolution.Split('x') | ForEach-Object {[int]$_}
foreach($kind in @('scene','script','trajectory')){
    $path=Join-Path $projectRoot $inputData.$kind
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $inputData.($kind+'Sha256')){
        throw "Benchmark $kind hash mismatch."
    }
}
if($inputData.environment){
    $environmentPath=Join-Path $projectRoot $inputData.environment.path
    if((Get-FileHash -LiteralPath $environmentPath -Algorithm SHA256).Hash -ne $inputData.environment.sha256){
        throw 'Benchmark environment hash mismatch.'
    }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun = Start-HEStorageRun -Project $projectRoot -Workflow 'Capture-RenderingBenchmark' -Paths @($OutputDirectory) -Outer
$storageState = 'failed'
try {

Copy-Item -LiteralPath $RenderSettingsPath -Destination (Join-Path $OutputDirectory 'quality.json')
Copy-Item -LiteralPath $BenchmarkInputPath -Destination (Join-Path $OutputDirectory 'benchmark_input.json')
$savedOutput=[Environment]::GetEnvironmentVariable('HE_RENDER_BENCHMARK_OUTPUT','Process')
$results=[Collections.Generic.List[object]]::new()
try{
    foreach($path in @('Deferred','Forward')){
        $prefix=Join-Path $OutputDirectory $path.ToLowerInvariant()
        $cameraDirectory=$prefix+'.camera'
        New-Item -ItemType Directory -Path $cameraDirectory | Out-Null
        $cameraOutput=Join-Path $cameraDirectory 'camera.runtime.json'
        [Environment]::SetEnvironmentVariable('HE_RENDER_BENCHMARK_OUTPUT',$cameraDirectory,'Process')
        $arguments=@('game','capture','-Project',$projectRoot,'-StartupScene',$inputData.scene,'-RenderPath',$path,
            '-GraphicsApi',$GraphicsApi,'-Resolution',$Resolution,'-RenderSettingsPath',
            (Join-Path $OutputDirectory 'quality.json'),'-Frames',"$Frames",'-FixedDeltaSeconds',
            ([double]$inputData.fixedDeltaSeconds).ToString('R',[Globalization.CultureInfo]::InvariantCulture),
            '-CapturePath',($prefix+'.png'),'-RenderValidation','-RenderStats',
            '-RenderStatsPath',($prefix+'.csv'),'-PerformanceWarmupFrames','0','-TimeoutSeconds','300')
        & (Join-Path $repositoryRoot 'hectl.bat') @arguments *> ($prefix+'.log')
        if($LASTEXITCODE -ne 0){throw "HGame capture failed: $prefix.log"}
        $camera=Get-Content -LiteralPath $cameraOutput -Raw | ConvertFrom-Json
        if($camera.status -ne 'PASS_FIXED_TICK_INPUT' -or $camera.ticks -ne $Frames -or
            $camera.trajectorySha256 -ne $inputData.trajectorySha256 -or $camera.scriptSha256 -ne $inputData.scriptSha256){
            throw 'Capture camera did not complete the exact authored input.'
        }
        $settings=Get-Content -LiteralPath ($prefix+'.csv.settings.json') -Raw | ConvertFrom-Json
        if($settings.renderer.framesInFlight -ne $settings.effective.'Rendering.Presentation.FramesInFlight'){
            throw 'Capture backend frame count differs from effective settings.'
        }
        if($settings.renderer.phase -ne 'shutdown' -or $settings.renderer.width -ne $dimensions[0] -or
            $settings.renderer.height -ne $dimensions[1] -or $settings.effective.'Rendering.Path' -ne $path -or
            $settings.effective.'Rendering.Backend' -ne $GraphicsApi){throw 'Capture effective path/backend/dimensions mismatch.'}
        foreach($property in $quality.PSObject.Properties){
            if($property.Name -in @('Rendering.Path','Rendering.Backend')){continue}
            $actual=$settings.effective.PSObject.Properties[$property.Name]
            $expected=$property.Value
            $matches=$null -ne $actual
            if($matches -and ($expected -is [bool] -or $expected -is [string])){$matches=$actual.Value -eq $expected}
            elseif($matches){$matches=[Math]::Abs([double]$actual.Value-[double]$expected) -le [Math]::Max(0.000001,[Math]::Abs([double]$expected)*0.00001)}
            if(!$matches){throw "Capture quality mismatch: $($property.Name)"}
        }
        $results.Add(@{evidenceType='runtime-manual';status='CAPTURED';program='hectl.bat';arguments=$arguments;
            camera=$camera;settings=$settings;image=$prefix+'.png';imageSha256=(Get-FileHash ($prefix+'.png')).Hash;
            executableSha256=(Get-FileHash $gameExecutable).Hash})
    }
    if($results[0].camera.poseSequenceSha256 -ne $results[1].camera.poseSequenceSha256){
        throw 'Forward and Deferred capture camera sequences differ.'
    }
}finally{
    [Environment]::SetEnvironmentVariable('HE_RENDER_BENCHMARK_OUTPUT',$savedOutput,'Process')
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'captures.json'),
        (ConvertTo-Json -InputObject $results.ToArray() -Depth 40),[Text.UTF8Encoding]::new($false))
}
Write-Host "Native captures retained: $OutputDirectory; image quality requires separate review."

$storageState = 'success'
}
finally { Complete-HEStorageRun -Run $storageRun -State $storageState }
