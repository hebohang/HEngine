[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
    [ValidatePattern('^$|^[0-9a-fA-F]{64}$')][string]$ExistingBuildSha256='',
    [ValidateRange(1,120)][int]$SettleFrames=60,
    [string]$PythonExecutable='python',
    [int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
$null=Get-Command $PythonExecutable -ErrorAction Stop
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$hectl=Join-Path $PSScriptRoot 'hectl.ps1'
$ownerTag=[guid]::NewGuid().ToString('N');$sequence=0;$SampleMemory=$false
$sceneDirectory=$OutputDirectory;$expectedSize=@(1280,720)
$commands=[Collections.Generic.List[object]]::new();$observations=[Collections.Generic.List[object]]::new()
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Test-RenderingPaths.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Helper source cannot be parsed.'}
foreach($name in @('Write-Json','Invoke-HeCtl','Call-Agent','Save-Capture')){
    $definition=$ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)|Select-Object -First 1
    if(!$definition){throw "Missing helper $name"}
    . ([scriptblock]::Create($definition.Extent.Text))
}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$relativeRoot='Assets/RenderingBenchmarks/CustomLodValidation/'+$ownerTag
$fixtureRoot=[IO.Path]::GetFullPath((Join-Path $repositoryRoot ('Project/'+$relativeRoot)))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$run=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'custom-material-lod' -Paths @($OutputDirectory,$fixtureRoot) -Outer
$state='failed';$started=$false
try {
    $meshRelative='Assets/RenderingBenchmarks/IntelSponza2048BCGPULOD/Meshes/sponza_mesh_042.hmesh'
    $meshPath=Join-Path $repositoryRoot ('Project/'+$meshRelative)
    $mesh=Get-Content -LiteralPath $meshPath -Raw | ConvertFrom-Json
    if($mesh.primitives.Count -ne 1 -or $mesh.primitives[0].lods.Count -lt 3){throw 'The selected authored mesh does not have the required single primitive / three LODs.'}
    $primitive=$mesh.primitives[0]
    $position=$mesh.accessorVec[$primitive.attributes.POSITION]
    if($position.componentType -ne 5126 -or $position.type -ne 'VEC3'){throw 'Expected float3 positions.'}
    $bufferView=$mesh.bufferViewVec[$position.bufferViewIndex]
    $bufferPath=Join-Path $repositoryRoot ('Project/'+$mesh.bufferVec[$bufferView.bufferIndex].uri)
    $bytes=[IO.File]::ReadAllBytes($bufferPath)
    $stride=if($bufferView.byteStride -gt 0){[int]$bufferView.byteStride}else{12}
    $offset=[int]$bufferView.byteOffset+[int]$position.byteOffset
    $minimum=@([double]::PositiveInfinity,[double]::PositiveInfinity,[double]::PositiveInfinity)
    $maximum=@([double]::NegativeInfinity,[double]::NegativeInfinity,[double]::NegativeInfinity)
    for($vertex=0;$vertex -lt $position.count;$vertex++){
        for($axis=0;$axis -lt 3;$axis++){
            $value=[double][BitConverter]::ToSingle($bytes,$offset+$vertex*$stride+$axis*4)
            if([double]::IsNaN($value) -or [double]::IsInfinity($value)){throw 'Invalid authored position.'}
            $minimum[$axis]=[Math]::Min($minimum[$axis],$value);$maximum[$axis]=[Math]::Max($maximum[$axis],$value)
        }
    }
    $radiusSquared=0.0;$center=@(0.0,0.0,0.0)
    for($axis=0;$axis -lt 3;$axis++){$center[$axis]=($minimum[$axis]+$maximum[$axis])*0.5;$radiusSquared+=[Math]::Pow(($maximum[$axis]-$minimum[$axis])*0.5,2)}
    $radius=[Math]::Sqrt($radiusSquared)
    if($radius -le 0){throw 'Degenerate authored mesh bounds.'}
    $sourcePath=Join-Path $repositoryRoot 'Project/Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he'
    $source=Get-Content -LiteralPath $sourcePath -Raw|ConvertFrom-Json
    $fixture=[ordered]@{}
    foreach($entry in $source.PSObject.Properties){
        $entity=$entry.Value
        if($entity.CameraComponent){
            $entity.TransformComponent.Translation=@(0,0,4);$entity.TransformComponent.Rotation=@(0,0,0)
            $fixture[$entry.Name]=$entity
        }elseif($entity.EnvironmentLightComponent -or $entity.DirectionalLightComponent){$fixture[$entry.Name]=$entity}
        elseif($entity.TagComponent.Tag -eq 'FresnelRim'){
            $entity.MeshComponent.Path=$meshRelative
            $entity.TransformComponent.Scale=@((1.0/$radius),(1.0/$radius),(1.0/$radius))
            # This authored mesh has substantial XY-facing surface area.
            $entity.TransformComponent.Translation=@((-$center[0]/$radius),(-$center[1]/$radius),(-$center[2]/$radius))
            $entity.TransformComponent.Rotation=@(0,0,0)
            $fixture[$entry.Name]=$entity
        }
    }
    if(@($fixture.Values|Where-Object{$_.MeshComponent}).Count -ne 1){throw 'Fixture must have only the custom mesh.'}
    $currentScene=$relativeRoot+'/CustomMaterialLod.he'
    $fixturePath=Join-Path $fixtureRoot 'CustomMaterialLod.he'
    Write-Json $fixturePath $fixture
    Copy-Item -LiteralPath $fixturePath -Destination (Join-Path $OutputDirectory 'fixture.he')
    $emptyFixture=[ordered]@{}
    foreach($key in $fixture.Keys){if(!$fixture[$key].MeshComponent){$emptyFixture[$key]=$fixture[$key]}}
    $emptyScene=$relativeRoot+'/NoObject.he'
    Write-Json (Join-Path $fixtureRoot 'NoObject.he') $emptyFixture
    Copy-Item -LiteralPath (Join-Path $fixtureRoot 'NoObject.he') -Destination (Join-Path $OutputDirectory 'empty-fixture.he')
    Write-Json (Join-Path $OutputDirectory 'fixture-evidence.json') @{scene=$currentScene;sourceSha256=(Get-FileHash $sourcePath -Algorithm SHA256).Hash;mesh=$meshRelative;meshSha256=(Get-FileHash $meshPath -Algorithm SHA256).Hash;vertexBufferSha256=(Get-FileHash $bufferPath -Algorithm SHA256).Hash;material='Assets/Material/FresnelRim.hmat';materialModification='None; original Back-cull shader contract preserved';center=$center;radius=$radius;lods=$primitive.lods;baseIndexCount=$mesh.accessorVec[$primitive.indices].count;cameraPosition=@(0,0,4);normalization='Unit bounding sphere, centered with identity rotation. Camera distance 4 provides visible projected area while retaining eligible authored LODs.'}
    $arguments=@('agent','start','-StartupScene',$currentScene,'-Resolution','1280x720','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-RenderValidation','-FixedDeltaSeconds','0.0166667')
    if($ExistingBuildSha256){$arguments+=@('-ExistingBuildSha256',$ExistingBuildSha256)}
    $started=$true;$null=Invoke-HeCtl $arguments 'start' $false
    $null=Call-Agent 'render.view.create' @{width=1280;height=720}
    foreach($path in @('Forward','Deferred')){
      foreach($batched in @($false,$true)){
        $baseline=$null
        $emptyCapture=$null
        foreach($mode in @('empty','off','on')){
            $enabled=$mode -eq 'on'
            $label=$path+'-'+$(if($batched){'grouped'}else{'individual'})+'-lod-'+$mode
            if($mode -ne 'on'){$null=Call-Agent 'runtime.scene.load' @{path=$(if($mode -eq 'empty'){$emptyScene}else{$currentScene});forceReload=$true}}
            $settings=Call-Agent 'render.settings.get'
            $receipt=Call-Agent 'render.settings.apply' @{layer='Session';baseRevision=$settings.revision;baseHash=$settings.hash;persist=$false;patch=@{
                'Rendering.Path'=$path;'Rendering.GI.Technique'='Off';'Rendering.GI.Denoiser'='None';'Rendering.AntiAliasing.Mode'='TAA';
                'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false;
                'Rendering.GpuBatching.Enabled'=$batched;'Rendering.LOD.Enabled'=$enabled;'Rendering.LOD.MaxPixelError'=1.0}}
            Write-Json (Join-Path $OutputDirectory ($label+'-receipt.json')) $receipt
            if(!$receipt.success){throw 'LOD settings transaction rejected.'}
            $null=Call-Agent 'runtime.step' @{frames=$SettleFrames}
            $view=Call-Agent 'render.view.status'
            Write-Json (Join-Path $OutputDirectory ($label+'-view.json')) $view
            $capture=Save-Capture $label
            $observations.Add(@{path=$path;gpuBatching=$batched;mode=$mode;lodEnabled=$enabled;view=$view;capture=$capture;submissionInterpretation='Both grouped and individual paths are GPU indirect submissions; neither is a CPU fallback reference.'})
            if($mode -eq 'empty'){
                if($view.renderObjectCount -ne 0){throw 'Object-free baseline contains render objects.'}
                $emptyCapture=$capture
                continue
            }
            $visibilityPath=Join-Path $OutputDirectory ($label+'-visibility.json')
            $null=& $PythonExecutable (Join-Path $PSScriptRoot 'Compare-RenderingImages.py') $emptyCapture.path $capture.path --output $visibilityPath
            if($LASTEXITCODE -ne 0){throw 'Object visibility comparison failed.'}
            $visibility=Get-Content -LiteralPath $visibilityPath -Raw|ConvertFrom-Json
            if($visibility.pixelCountOver4 -lt 1000 -or !$visibility.pixelBoundsOver4 -or
                ($visibility.pixelBoundsOver4[2]-$visibility.pixelBoundsOver4[0]) -lt 40 -or
                ($visibility.pixelBoundsOver4[3]-$visibility.pixelBoundsOver4[1]) -lt 30){throw 'Custom mesh has insufficient visible shape versus the no-object baseline.'}
            if($view.renderObjectCount -ne 1 -or $view.customGpuObjects -ne 1 -or $view.customGpuDraws -ne 1 -or $view.customCpuDraws -ne 0){throw 'Expected exactly one GPU custom object and no CPU custom fallback.'}
            if(!$enabled){
                if($view.reducedLodSections -ne 0 -or $view.selectedRasterTriangles -ne $view.baseRasterTriangles -or $view.baseRasterTriangles -le 0){throw 'LOD-off baseline did not submit full geometry.'}
                $baseline=$view
            }else{
                if($view.reducedLodSections -ne 1 -or $view.selectedRasterTriangles -ge $view.baseRasterTriangles -or $view.selectedRasterTriangles -le 0){throw 'Custom GPU object did not select a reduced authored LOD.'}
                if($view.baseRasterTriangles -ne $baseline.baseRasterTriangles -or ($view.cameraPosition|ConvertTo-Json -Compress) -ne ($baseline.cameraPosition|ConvertTo-Json -Compress)){throw 'LOD A/B changed geometry baseline or camera.'}
            }
        }
      }
    }
    $state='success'
}finally{
    $stopped=!$started
    try{if($started){$null=Invoke-HeCtl @('agent','stop') 'stop' $false;$stopped=$true}}
    catch{$state='failed';throw}
    finally{
        Write-Json (Join-Path $OutputDirectory 'observations.json') $observations
        Write-Json (Join-Path $OutputDirectory 'commands.json') $commands
        Write-Json (Join-Path $OutputDirectory 'result.json') @{status=$(if($state -eq 'success'){'PASS'}else{'FAIL'});evidenceType='runtime-manual';visualStatus='NOT_REVIEWED';detail='GPU submission candidates and selected authored triangle counts; screenshots require visibility/error review, not proof of pixel-error bounds.'}
        # Remove only this invocation's exact unique fixture directory after stop.
        $allowedRoot=[IO.Path]::GetFullPath((Join-Path $repositoryRoot 'Project/Assets/RenderingBenchmarks/CustomLodValidation'))
        $resolved=[IO.Path]::GetFullPath($fixtureRoot)
        if([IO.Path]::GetDirectoryName($resolved) -ne $allowedRoot -or [IO.Path]::GetFileName($resolved) -ne $ownerTag){throw 'Fixture cleanup containment check failed.'}
        if($stopped -and (Test-Path -LiteralPath $resolved)){Remove-Item -LiteralPath $resolved -Recurse -Force}
        Complete-HEStorageRun -Run $run -State $state
    }
}
