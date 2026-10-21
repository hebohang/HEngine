[CmdletBinding()]
param(
    [string]$OutputDirectory = '',
    [ValidateSet('Intel','Bistro','PBRTest')][string[]]$Scenes = @('Intel','Bistro'),
    [string[]]$QualityPaths = @(),
    [ValidatePattern('^[1-9][0-9]{0,3}x[1-9][0-9]{0,3}$')][string]$Resolution = '1920x1080',
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi = 'DX12',
    [ValidateRange(0,1000000)][int]$WarmupFrames = 60,
    [ValidateRange(1,1000000)][int]$MeasureFrames = 120,
    [ValidateRange(1,3600)][int]$TimeoutSeconds = 300,
    [string]$PythonPath = 'python',
    [ValidateSet('AuthoredDirect','OverheadDirect')][string]$LightingCase = 'AuthoredDirect',
    [switch]$IntelCloseView,
    [switch]$MovingCamera,
    [switch]$CheckOnly,
    [switch]$PrepareOnly
)
# Paired direct-only diagnostic. Uses owned hectl capture/check processes only.
# Example: -QualityPaths out/agent/candidate.json -PythonPath <Python-with-Pillow-and-NumPy>
# Moving runs require complete 600-frame warmup/measurement camera cycles.
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$projectRoot = Join-Path $repositoryRoot 'Project'
$runId = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'), $PID
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repositoryRoot "out/agent/ssgi/$runId" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory; evidence is never overwritten.' }
if ($MovingCamera -and ($WarmupFrames % 600 -ne 0 -or $MeasureFrames % 600 -ne 0)) {
    throw 'Moving-camera warmup and measurement must each contain whole 600-frame cycles.'
}
if ($MovingCamera -and $IntelCloseView) { throw '-IntelCloseView requires a static camera.' }
if ($MovingCamera -and $Scenes -contains 'PBRTest') { throw 'PBRTest has no authored benchmark trajectory; use its static primary camera.' }
if (!$PrepareOnly) {
    & $PythonPath -c 'import PIL, numpy' 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'Set -PythonPath to a Python runtime with Pillow and NumPy.' }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$sceneRelativeRoot = "Assets/RenderingBenchmarks/Validation/SSGIDirect/$runId"
$sceneRoot = Join-Path $projectRoot $sceneRelativeRoot
New-Item -ItemType Directory -Path $sceneRoot | Out-Null
function Save-Json($Value, [string]$Path) {
    [IO.File]::WriteAllText($Path, (ConvertTo-Json -InputObject $Value -Depth 100) + "`n", [Text.UTF8Encoding]::new($false))
}
function Hash-File([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Read-Settings([string]$Path) {
    $result = [ordered]@{}
    $source = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    foreach ($property in $source.PSObject.Properties) { $result[$property.Name] = $property.Value }
    return $result
}
if ($QualityPaths.Count -eq 0) {
    $quality = Read-Settings (Join-Path $projectRoot 'Assets/RenderingBenchmarks/Configurations/Native4KTwoFramesCandidates.json')
    $quality['Rendering.GI.SSGI.RayCount'] = 1
    $quality['Rendering.GI.SSGI.StepCount'] = 64
    $quality['Rendering.GI.MaxBounces'] = 1
    $quality['Rendering.GI.AtrousIterations'] = 2
    $quality['Rendering.GI.RenderScale'] = 0.5
    $quality['Rendering.GI.SVGF.OptimizedKernels'] = $true
    $quality['Rendering.GI.SSGI.HiZStartMip'] = 2
    $quality['Rendering.GI.SSGI.BinaryRefinementSteps'] = 3
    $quality['Rendering.GI.SSGI.MaximumRayDistance'] = 10.0
    $quality['Rendering.GI.SSGI.Thickness'] = 0.1
    $quality['Rendering.GI.SSGI.EdgeFade'] = 0.05
    $quality['Rendering.GI.SSGI.TemporalWeight'] = 0.97
    $QualityPaths = @((Join-Path $OutputDirectory 'low.input.json'))
    Save-Json $quality $QualityPaths[0]
}
$sceneFolders = @{ Intel='IntelSponza2048BCGPULOD'; Bistro='BistroExterior2048BCGPUMaskFixedEnvironment' }
$sourceFiles = @('Engine/Content/Shaders/GlobalIllumination/ScreenSpaceGlobalIllumination.hlsl',
    'Engine/Content/Shaders/GlobalIllumination/RtgiSvgf.hlsl',
    'Engine/Source/Runtime/Graphics/GlobalIllumination/ScreenSpaceGlobalIllumination.cpp',
    'Engine/Source/Runtime/Graphics/GlobalIllumination/SvgfDenoiser.cpp',
    'Engine/Source/Runtime/Graphics/Renderer/SceneRenderer.cpp',
    'Engine/Source/Runtime/Graphics/Renderer/SceneRendererRaster.inl',
    'Engine/Source/Runtime/Graphics/Settings/RenderSettings.cpp')
function Get-SourceHashes {
    $hashes = [ordered]@{}
    foreach ($source in $sourceFiles) { $hashes[$source] = Hash-File (Join-Path $repositoryRoot $source) }
    return $hashes
}
$manifest = [ordered]@{
    schemaVersion=1; evidenceType='runtime-manual'; createdUtc=[DateTime]::UtcNow.ToString('o')
    repositoryRoot=$repositoryRoot; sourceCommit=(& git -C $repositoryRoot rev-parse HEAD)
    sourceState=@(& git -C $repositoryRoot status --short); sourceHashes=(Get-SourceHashes)
    resolution=$Resolution; graphicsApi=$GraphicsApi
    lightingCase=$LightingCase; intelCloseView=[bool]$IntelCloseView
    warmupFrames=$WarmupFrames; measureFrames=$MeasureFrames; movingCamera=[bool]$MovingCamera
    fixedDeltaSeconds=1.0/60.0; scenes=@(); runs=@(); status='PREPARED'
    interpretation='Short paired diagnostic, not thermal stability or automatic physical-light-transport correctness. Images are tone-mapped sRGB.'
}
foreach ($name in $Scenes) {
    $authored = $null
    if ($name -eq 'PBRTest') {
        $sourcePath = Join-Path $projectRoot 'Assets/Scenes/PBRTest.he'
    } else {
    $folder = Join-Path $projectRoot ('Assets/RenderingBenchmarks/Trajectories/' + $sceneFolders[$name])
    $sourcePath = Join-Path $folder 'Benchmark.he'
    $authored = Get-Content -LiteralPath (Join-Path $folder 'benchmark_input.json') -Raw | ConvertFrom-Json
    foreach ($kind in @('scene','trajectory','script')) {
        if ((Hash-File (Join-Path $projectRoot $authored.$kind)) -ne $authored.($kind+'Sha256')) {
            throw "Authored benchmark $name $kind hash mismatch."
        }
    }
    }
    $scene = Get-Content -LiteralPath $sourcePath -Raw | ConvertFrom-Json
    $environments = @()
    $sceneChanges = [ordered]@{ lightingCase=$LightingCase; directLights=@(); cameras=@(); removedScripts=@() }
    foreach ($entity in $scene.PSObject.Properties) {
        $components = $entity.Value
        if ($components.EnvironmentLightComponent) {
            $environment = $components.EnvironmentLightComponent
            $environments += @{ entity=$entity.Name; originalIbl=$environment.IBLIntensity; modifiedIbl=0.0
                exposureEV100=$environment.ExposureEV100; exposureCompensation=$environment.ExposureCompensation; hdr=$environment.HdrTexturePath }
            $environment.IBLIntensity = 0.0
        }
        if ($LightingCase -eq 'OverheadDirect' -and $components.DirectionalLightComponent) {
            $original = [ordered]@{ light=$components.DirectionalLightComponent; transform=$components.TransformComponent } |
                ConvertTo-Json -Depth 20 | ConvertFrom-Json
            $components.DirectionalLightComponent.Intensity = 30.0
            $components.DirectionalLightComponent.Color = @(1.0,0.95,0.85)
            $components.TransformComponent.ParentUuid = ''
            $components.TransformComponent.Rotation = @([Math]::PI,0.0,0.0)
            $sceneChanges.directLights += @{ entity=$entity.Name; original=$original
                modified=@{ intensity=30.0; color=@(1.0,0.95,0.85); parentUuid=''; rotation=@([Math]::PI,0.0,0.0) } }
        }
        if ($IntelCloseView -and $name -eq 'Intel' -and $components.CameraComponent.Primary) {
            $original = $components.TransformComponent | ConvertTo-Json -Depth 20 | ConvertFrom-Json
            $components.TransformComponent.ParentUuid = ''
            $components.TransformComponent.Translation = @(-4.0,1.6,3.0)
            $components.TransformComponent.Rotation = @(-0.25,-1.57,0.0)
            $sceneChanges.cameras += @{ entity=$entity.Name; originalTransform=$original
                modified=@{ translation=@(-4.0,1.6,3.0); rotation=@(-0.25,-1.57,0.0); parentUuid='' } }
        }
        if (!$MovingCamera) {
            if ($components.PythonScriptComponent) {
                $sceneChanges.removedScripts += @{ entity=$entity.Name; path=$components.PythonScriptComponent.Path }
            }
            $components.PSObject.Properties.Remove('PythonScriptComponent')
        }
    }
    if ($environments.Count -eq 0) { throw "$name has no explicit environment component." }
    if ($LightingCase -eq 'OverheadDirect' -and $sceneChanges.directLights.Count -eq 0) {
        throw "$name has no directional light to override for the supplemental case."
    }
    if ($IntelCloseView -and $name -eq 'Intel' -and $sceneChanges.cameras.Count -ne 1) {
        throw 'Intel close view requires exactly one primary camera.'
    }
    $relative = "$sceneRelativeRoot/$($name.ToLowerInvariant()).he"
    $path = Join-Path $projectRoot $relative
    Save-Json $scene $path
    $manifest.scenes += @{ name=$name; source=$sourcePath; sourceSha256=(Hash-File $sourcePath)
        scene=$relative; sceneSha256=(Hash-File $path); environments=$environments; authored=$authored
        modifications=$sceneChanges
        comparisonScope= $(if ($LightingCase -eq 'OverheadDirect' -or $sceneChanges.cameras.Count) {
            'Supplemental controlled light-transport diagnostic; lighting and/or camera differ from the authored benchmark.'
        } else { 'Authored lighting/camera with environment IBL disabled; static mode removes the trajectory script.' }) }
}
$qualityIndex = 0
foreach ($qualityPath in $QualityPaths) {
    $qualityIndex++
    $quality = Read-Settings $qualityPath
    # These controls isolate indirect transport and keep Off/On otherwise equal.
    $quality['Rendering.Path'] = 'Deferred'
    $quality['Rendering.Presentation.VSync'] = $false
    $quality['Rendering.GI.Denoiser'] = 'SVGF'
    $quality['Rendering.GI.UnsupportedFallback'] = 'Off'
    $quality['Rendering.AO.Enabled'] = $false
    $quality['Rendering.Reflections.SSR.Enabled'] = $false
    $quality['Rendering.Reflections.Probes.Enabled'] = $false
    $quality['Rendering.PostProcess.Bloom.Intensity'] = 0.0
    $quality['Rendering.PostProcess.AutoExposure.Enabled'] = $false
    $quality['Rendering.Shadows.Enabled'] = $true
    $quality['Rendering.LOD.Enabled'] = $false
    $quality['Rendering.Debug.GI.View'] = 0
    $quality['Rendering.Debug.GI.DirectOnly'] = $false
    $quality['Rendering.Debug.PBR.Mode'] = 0
    $label = '{0}-{1}' -f $qualityIndex, [IO.Path]::GetFileNameWithoutExtension($qualityPath)
    foreach ($scene in $manifest.scenes) {
        $pair = "$($scene.name.ToLowerInvariant())-$label"
        foreach ($mode in @('off','on')) {
            $quality['Rendering.GI.Technique'] = if ($mode -eq 'off') { 'Off' } else { 'ScreenSpaceGI' }
            $prefix = Join-Path $OutputDirectory "$pair-$mode"
            Save-Json $quality ($prefix+'.json')
            $action = if ($CheckOnly) { 'check' } else { 'capture' }
            $arguments = @('game',$action,'-StartupScene',$scene.scene,'-RenderPath','Deferred',
                '-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-Resolution',$Resolution,
                '-RenderSettingsPath',($prefix+'.json'),'-Frames',"$($WarmupFrames+$MeasureFrames)",
                '-PerformanceWarmupFrames',"$WarmupFrames",'-FixedDeltaSeconds','0.016666666666666666',
                '-RenderStats','-RenderStatsPath',($prefix+'.csv'),'-RenderValidation','-TimeoutSeconds',"$TimeoutSeconds")
            if (!$CheckOnly) { $arguments += @('-CapturePath',($prefix+'.png')) }
            $manifest.runs += @{ pair=$pair; scene=$scene.name; mode=$mode; prefix=$prefix
                sceneSha256=$scene.sceneSha256; qualitySource=[IO.Path]::GetFullPath($qualityPath)
                qualitySourceSha256=(Hash-File $qualityPath); inputSha256=(Hash-File ($prefix+'.json'))
                executableSha256=$null; sourceHashes=$null
                program=(Join-Path $repositoryRoot 'hectl.bat'); arguments=$arguments; status='PREPARED' }
        }
    }
}
$manifestPath = Join-Path $OutputDirectory 'manifest.json'
Save-Json $manifest $manifestPath
if ($PrepareOnly) { Write-Host "SSGI paired inputs prepared: $manifestPath"; return }
$previousOutput = [Environment]::GetEnvironmentVariable('HE_RENDER_BENCHMARK_OUTPUT','Process')
Push-Location $repositoryRoot
try {
    foreach ($run in $manifest.runs) {
        if ($MovingCamera) {
            $cameraDirectory = $run.prefix+'.camera'
            New-Item -ItemType Directory -Path $cameraDirectory | Out-Null
            [Environment]::SetEnvironmentVariable('HE_RENDER_BENCHMARK_OUTPUT',$cameraDirectory,'Process')
        }
        $run.sourceHashes = Get-SourceHashes
        $run.status = 'RUNNING'; Save-Json $manifest $manifestPath
        $runArguments = $run.arguments
        & $run.program @runArguments *> ($run.prefix+'.log')
        $exitCode = $LASTEXITCODE
        foreach ($line in Get-Content -LiteralPath ($run.prefix+'.log')) {
            if ($line -match 'stdout: (.*); stderr: (.*)$') {
                Copy-Item -LiteralPath $Matches[1].Trim() -Destination ($run.prefix+'.stdout.log')
                Copy-Item -LiteralPath $Matches[2].Trim() -Destination ($run.prefix+'.stderr.log')
            }
        }
        $run.executableSha256 = Hash-File (Join-Path $repositoryRoot 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe')
        $run.status = if ($exitCode -eq 0) { 'CAPTURED' } else { 'FAIL' }
        Save-Json $manifest $manifestPath
        if ($exitCode -ne 0) { throw "SSGI run failed ($exitCode): $($run.prefix).log" }
        Write-Host "Captured $($run.pair) $($run.mode)"
    }
    & $PythonPath (Join-Path $PSScriptRoot 'Analyze-SSGI.py') --manifest $manifestPath
    if ($LASTEXITCODE -ne 0) { throw 'SSGI evidence analysis failed; inspect report/logs.' }
    $manifest.status = 'COMPLETE'
} finally {
    [Environment]::SetEnvironmentVariable('HE_RENDER_BENCHMARK_OUTPUT',$previousOutput,'Process')
    Pop-Location
    Save-Json $manifest $manifestPath
}
Write-Host "SSGI evidence and image/performance report: $OutputDirectory"
