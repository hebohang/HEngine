[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
    [ValidateSet('Rendering.Pipeline','Engine.Input','Engine.Navigation')][string]$Page='Rendering.Pipeline',
    [ValidateRange(30,600)][int]$Frames=90
)
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new capture directory.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'Capture-PipelineSettings' -Paths @($OutputDirectory) -Outer
$storageState='failed'
$oldPanel=[Environment]::GetEnvironmentVariable('HE_EDITOR_SETTINGS_CAPTURE','Process')
$oldScale=[Environment]::GetEnvironmentVariable('HE_EDITOR_CAPTURE_UI_SCALE','Process')
$results=[Collections.Generic.List[object]]::new()
try {
    [Environment]::SetEnvironmentVariable('HE_EDITOR_SETTINGS_CAPTURE',$Page,'Process')
    foreach($case in @(
        @{name='standard';resolution='1920x1080';scale='1.0'},
        @{name='compact';resolution='1280x720';scale='1.0'},
        @{name='high-dpi';resolution='1920x1080';scale='1.5'}
    )) {
        [Environment]::SetEnvironmentVariable('HE_EDITOR_CAPTURE_UI_SCALE',$case.scale,'Process')
        $capture=Join-Path $OutputDirectory ($case.name+'.png')
        $log=Join-Path $OutputDirectory ($case.name+'.log')
        & (Join-Path $repositoryRoot 'hectl.bat') editor capture -Configuration RelWithDebInfo -GraphicsApi $GraphicsApi -StrictGraphicsApi -Resolution $case.resolution -Frames $Frames -TimeoutSeconds 300 -CapturePath $capture *> $log
        $code=$LASTEXITCODE
        $present=Test-Path -LiteralPath $capture
        $results.Add(@{name=$case.name;page=$Page;resolution=$case.resolution;uiScale=$case.scale;backend=$GraphicsApi;exitCode=$code;capture=$capture;captureExists=$present;log=$log;visualReview='pending'})
        if($code -ne 0 -or -not $present){throw "Editor capture failed for $($case.name); see $log"}
    }
    $storageState='success'
} finally {
    [Environment]::SetEnvironmentVariable('HE_EDITOR_SETTINGS_CAPTURE',$oldPanel,'Process')
    [Environment]::SetEnvironmentVariable('HE_EDITOR_CAPTURE_UI_SCALE',$oldScale,'Process')
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'capture-runs.json'),(ConvertTo-Json -InputObject $results.ToArray() -Depth 8),[Text.UTF8Encoding]::new($false))
    Complete-HEStorageRun -Run $storageRun -State $storageState
}
Write-Host "Settings captures created in $OutputDirectory. Inspect images before claiming visual acceptance."
