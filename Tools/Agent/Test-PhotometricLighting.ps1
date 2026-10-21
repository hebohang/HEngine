[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi = 'DX12'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new evidence directory.' }
New-Item -ItemType Directory -Path $output | Out-Null
$relative = 'Assets/RenderingBenchmarks/Validation/Photometric/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
$sceneRoot = Join-Path (Join-Path $root 'Project') $relative
New-Item -ItemType Directory -Path $sceneRoot | Out-Null
function Save-Json($value, $path) {
    [IO.File]::WriteAllText($path, (ConvertTo-Json -InputObject $value -Depth 100) + "`n", [Text.UTF8Encoding]::new($false))
}
$settings = [ordered]@{
    'Rendering.GI.Technique'='Off'; 'Rendering.AntiAliasing.Mode'='None'
    'Rendering.AO.Enabled'=$false; 'Rendering.Reflections.SSR.Enabled'=$false
    'Rendering.Reflections.Probes.Enabled'=$false; 'Rendering.PostProcess.Bloom.Intensity'=0.0
    'Rendering.PostProcess.AutoExposure.Enabled'=$false; 'Rendering.Presentation.VSync'=$false
    'Rendering.Debug.PBR.Mode'=0; 'Rendering.LOD.Enabled'=$false
}
$cases = @(
    @{name='local-cd'; unit=1; path='Deferred'},
    @{name='local-lm'; unit=2; path='Deferred'},
    @{name='local-unitless'; unit=0; path='Deferred'},
    @{name='local-forward'; unit=1; path='Forward'},
    @{name='sun-reference'; lux=100.0; ev=(12.0-[Math]::Log(1000.0,2.0)); path='Deferred'},
    @{name='sun-daylight'; lux=100000.0; ev=12.0; path='Deferred'},
    @{name='sun-high-ev-bloom'; lux=100000.0; ev=20.0; bloom=0.05; path='Deferred'}
)
$runs = @()
foreach ($case in $cases) {
    $source = if ($case.ContainsKey('unit')) { 'Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he' } else { 'Assets/Scenes/PBRTest.he' }
    $scene = Get-Content -LiteralPath (Join-Path (Join-Path $root 'Project') $source) -Raw | ConvertFrom-Json
    foreach ($entry in $scene.PSObject.Properties) {
        $entity = $entry.Value
        $entity.PSObject.Properties.Remove('PythonScriptComponent')
        if ($entity.EnvironmentLightComponent) {
            $entity.EnvironmentLightComponent.IBLIntensity = 0.0
            $entity.EnvironmentLightComponent.ExposureEV100 = if ($case.ContainsKey('unit')) { 5.0 } else { $case.ev }
            $entity.EnvironmentLightComponent.ExposureCompensation = 0.0
        }
        if ($case.ContainsKey('lux') -and $entity.DirectionalLightComponent) { $entity.DirectionalLightComponent.Intensity = $case.lux }
        foreach ($kind in @('PointLightComponent','SpotLightComponent')) {
            if (!$case.ContainsKey('unit') -or !$entity.$kind) { continue }
            $light = $entity.$kind
            $solidAngle = if ($kind -eq 'PointLightComponent') { 4.0*[Math]::PI } else { 2.0*[Math]::PI*(1.0-[Math]::Cos($light.OuterConeAngle*[Math]::PI/180.0)) }
            $light.IntensityUnits = $case.unit
            $light.Intensity = switch ($case.unit) { 0 { 800.0*625.0 } 1 { 800.0 } 2 { 800.0*$solidAngle } }
        }
    }
    $scenePath = Join-Path $sceneRoot ($case.name+'.he')
    Save-Json $scene $scenePath
    $settings['Rendering.Path'] = $case.path
    $settings['Rendering.PostProcess.Bloom.Intensity'] = if ($case.ContainsKey('bloom')) { $case.bloom } else { 0.0 }
    $prefix = Join-Path $output $case.name
    Save-Json $settings ($prefix+'.json')
    $arguments = @('game','capture','-StartupScene',"$relative/$($case.name).he",'-RenderPath',$case.path,
        '-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-Resolution','1280x720','-Frames','60',
        '-FixedDeltaSeconds','0.016666666666666666','-RenderSettingsPath',($prefix+'.json'),
        '-CapturePath',($prefix+'.png'),'-RenderValidation','-TimeoutSeconds','180')
    $run = @{ name=$case.name; case=$case; scene="$relative/$($case.name).he"; sceneSha256=(Get-FileHash $scenePath -Algorithm SHA256).Hash;
        command='hectl.bat'; arguments=$arguments; evidenceType='runtime-manual'; status='RUNNING' }
    $runs += $run
    Save-Json $runs (Join-Path $output 'manifest.json')
    & (Join-Path $root 'hectl.bat') @arguments *> ($prefix+'.log')
    $run.status = if ($LASTEXITCODE -eq 0) { 'PASS' } else { 'FAIL' }
    Save-Json $runs (Join-Path $output 'manifest.json')
    if ($LASTEXITCODE -ne 0) { throw "Capture failed: $($case.name). See $prefix.log" }
    Write-Output "Captured $($case.name)"
}
