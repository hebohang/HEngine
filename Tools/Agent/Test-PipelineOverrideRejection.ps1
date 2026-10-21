# Run only against a build containing persistent override rejection recovery.
# Main + mirrored preview coverage; no independent preview-world injection.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
    [string]$ExistingBuildSha256='', [int]$TimeoutSeconds=180)
$ErrorActionPreference='Stop'
$repositoryRoot=(git rev-parse --show-toplevel).Trim()
$agentRoot=Join-Path $repositoryRoot 'Tools/Agent'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$ownerTag=[guid]::NewGuid().ToString('N');$sequence=0;$sceneDirectory=$OutputDirectory
$hectl=Join-Path $agentRoot 'hectl.ps1';$SampleMemory=$false;$expectedSize=@(1280,720)
$commands=[Collections.Generic.List[object]]::new();$results=[Collections.Generic.List[object]]::new()
$parseErrors=$null;$tokens=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $agentRoot 'Test-RenderingPaths.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Helper source failed parsing.'}
foreach($name in @('Write-Json','Invoke-HeCtl','Call-Agent','Save-Capture','Get-GraphPasses')){
    $definition=$ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true) | Select-Object -First 1
    if(!$definition){throw "Missing helper $name"}
    . ([scriptblock]::Create($definition.Extent.Text))
}
. (Join-Path $agentRoot 'Storage.ps1')
$relativeRoot='Assets/RenderingBenchmarks/Validation/OverrideRejection-'+$ownerTag
$fixtureRoot=Join-Path $repositoryRoot ('Project/'+$relativeRoot)
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$storage=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'pipeline-override-rejection' -Paths @($OutputDirectory,$fixtureRoot) -Outer
$state='failed';$sessionRoot=$null
function Make-Fixture([string]$name,$world,$camera){
    $scene=Get-Content -LiteralPath (Join-Path $repositoryRoot 'Project/Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he') -Raw | ConvertFrom-Json
    foreach($entry in $scene.PSObject.Properties){
        if($entry.Value.CameraComponent){$entry.Value.CameraComponent | Add-Member -NotePropertyName RenderOverrides -NotePropertyValue $camera -Force}
    }
    $settings=@{TagComponent=@{Tag='Owned override test'};RenderSettingsComponent=@{Overrides=$world}}
    $scene | Add-Member -NotePropertyName ([guid]::NewGuid().ToString()) -NotePropertyValue $settings
    Write-Json (Join-Path $fixtureRoot ($name+'.he')) $scene
    return $relativeRoot+'/'+$name+'.he'
}
function Load-Fixture([string]$path){
    $before=Call-Agent 'world.summary'
    $null=Call-Agent 'runtime.scene.load' @{path=$path;forceReload=$true}
    for($attempt=0;$attempt -lt 30;$attempt++){
        $null=Call-Agent 'runtime.step' @{frames=1}
        $after=Call-Agent 'world.summary'
        if($after.sceneGeneration -ne $before.sceneGeneration){return}
    }
    throw 'Scene transition did not complete.'
}
function Assert-Paths([string]$expected,[string]$label){
    $main=Get-GraphPasses
    $preview=Call-Agent 'render.view.status'
    Write-Json (Join-Path $OutputDirectory ($label+'-graphs.json')) @{main=$main;preview=$preview}
    $required=$(if($expected -eq 'Forward'){'OpaqueForwardHDR'}else{'DeferredLightingHDR'})
    if(@($main|ForEach-Object{$_.name}) -notcontains $required -or @($preview.passes) -notcontains $required){throw "Expected $expected graph was not retained/adopted in both views."}
}
try {
    $good=Make-Fixture 'good' @{'Rendering.Path'='Forward'} @{}
    $fixed=Make-Fixture 'fixed' @{'Rendering.Path'='Deferred'} @{}
    $currentScene=$good
    $arguments=@('agent','start','-StartupScene',$good,'-Resolution','1280x720','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi')
    if($ExistingBuildSha256){$arguments+=@('-ExistingBuildSha256',$ExistingBuildSha256)}
    $null=Invoke-HeCtl $arguments 'start' $false
    $start=Get-Content -LiteralPath $commands[$commands.Count-1].output -Raw
    if($start -match '(?m)^session:\s*(.+?)\s*$'){$sessionRoot=Split-Path -Parent $Matches[1].Trim()}
    if(!$sessionRoot){throw 'Missing owned-session path.'}
    $runtimeLog=Join-Path $sessionRoot 'game.stdout.log'
    $null=Call-Agent 'render.view.create' @{width=1280;height=720}
    foreach($case in @(
        @{name='world-forbidden';world=@{'Rendering.Path'='Deferred';'Rendering.Backend'='Vulkan'};camera=@{}},
        @{name='camera-enum';world=@{'Rendering.Path'='Deferred'};camera=@{'Rendering.Path'='invalid-path'}},
        @{name='camera-type';world=@{'Rendering.Path'='Deferred'};camera=@{'Rendering.PostProcess.ExposureEV100'='bad'}},
        @{name='feature-parameters';world=@{'Rendering.Path'='Deferred'};camera=@{'Rendering.Pipeline.Features'=@(@{type='builtin.postprocess';instance='post';version=1;enabled=$true;parameters=@{invalid=$true}})}}
    )){
        Load-Fixture $good
        $null=Save-Capture ($case.name+'-before')
        Assert-Paths 'Forward' ($case.name+'-before')
        $countBefore=@(Select-String -LiteralPath $runtimeLog -SimpleMatch 'retaining all previous view configurations').Count
        $bad=Make-Fixture $case.name $case.world $case.camera
        Load-Fixture $bad
        $null=Save-Capture ($case.name+'-rejected')
        Assert-Paths 'Forward' ($case.name+'-rejected')
        $countSettled=@(Select-String -LiteralPath $runtimeLog -SimpleMatch 'retaining all previous view configurations').Count
        if($countSettled -le $countBefore){throw 'Rejected configuration produced no detailed diagnostic.'}
        $null=Call-Agent 'runtime.step' @{frames=30}
        $null=Save-Capture ($case.name+'-repeated')
        Assert-Paths 'Forward' ($case.name+'-repeated')
        $countAfter=@(Select-String -LiteralPath $runtimeLog -SimpleMatch 'retaining all previous view configurations').Count
        if($countAfter -ne $countSettled){throw 'Unchanged settled main/preview rejection was retried or re-diagnosed.'}
        Load-Fixture $fixed
        $null=Save-Capture ($case.name+'-recovered')
        Assert-Paths 'Deferred' ($case.name+'-recovered')
        $results.Add(@{case=$case.name;retention='PASS';recovery='PASS';dedup='PASS';visualReview='pending'
            limitation='Log dedup does not independently prove preparation counters stayed flat; CPU matcher and source boundary review are separate evidence.'})
    }
    $state='success'
} finally {
    try {$null=Invoke-HeCtl @('agent','stop') 'stop' $false}
    finally {
        if($sessionRoot){foreach($file in @('game.stdout.log','game.stderr.log')){
            $source=Join-Path $sessionRoot $file
            if(Test-Path -LiteralPath $source){Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDirectory $file)}
        }}
        $archive=Join-Path $OutputDirectory 'fixtures';New-Item -ItemType Directory -Path $archive -Force | Out-Null
        Get-ChildItem -LiteralPath $fixtureRoot -File | Copy-Item -Destination $archive
        $allowed=[IO.Path]::GetFullPath((Join-Path $repositoryRoot 'Project/Assets/RenderingBenchmarks/Validation'))+[IO.Path]::DirectorySeparatorChar
        $resolved=[IO.Path]::GetFullPath($fixtureRoot)
        if(!$resolved.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -ne ('OverrideRejection-'+$ownerTag)){throw 'Fixture cleanup escaped the owned directory.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force
        Write-Json (Join-Path $OutputDirectory 'results.json') $results
        Write-Json (Join-Path $OutputDirectory 'commands.json') $commands
        Complete-HEStorageRun -Run $storage -State $state
    }
}
