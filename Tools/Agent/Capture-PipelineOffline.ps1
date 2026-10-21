[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string[]]$GraphicsApi=@('DX12','Vulkan'),
    [ValidateRange(30,1200)][int]$Frames=180,
    [ValidateRange(64,1024)][int]$Width=320,
    [ValidateRange(64,1024)][int]$Height=180,
    [ValidateRange(1,64)][int]$Samples=8,
    [ValidateRange(60,1800)][int]$TimeoutSeconds=600,
    [string]$StartupScene='Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he'
)
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
# Configuration is shared with interactive editors. Require an idle GPU host.
if(Get-Process -Name HEngineEditor,HGame -ErrorAction SilentlyContinue){throw 'Stop existing editor/game processes before this serial capture.'}
$configPath=Join-Path $repositoryRoot 'Project/Config/DevOnly/OfflineReferenceRenderer.json'
if(!(Test-Path -LiteralPath $configPath)){throw "Missing existing offline settings: $configPath"}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'Capture-PipelineOffline' -Paths @($OutputDirectory) -Outer
$storageState='failed'
$originalBytes=[IO.File]::ReadAllBytes($configPath)
$originalHash=(Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
$backupPath=Join-Path $OutputDirectory 'OfflineReferenceRenderer.original.json'
[IO.File]::WriteAllBytes($backupPath,$originalBytes)
$results=[Collections.Generic.List[object]]::new()
$manifest=[ordered]@{
    evidenceType='runtime-manual';status='FAILED';visualReview='pending';providerVerification='pending'
    mode='he.viewport.path-tracing';configurationPath=$configPath;backupPath=$backupPath
    originalSha256=$originalHash;restoredSha256=$null;restored=$false
    runs=$results;error=$null
    acceptance='Inspect each screenshot: Offline Renderer must show a live session with Completed spp average > 0 at the requested fixed size, no session error, and a rendered viewport. Provider ID is he.viewport.path-tracing in the panel details. Mode selection, exit 0, or absence of errors alone is insufficient. Current host emits no positive provider-progress log; do not mark PASS until this visual evidence is verified.'
}
try {
    if((Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash -ne $originalHash){throw 'Original settings archive hash mismatch.'}
    $temporary=[Text.Encoding]::UTF8.GetString($originalBytes).TrimStart([char]0xfeff) | ConvertFrom-Json
    foreach($entry in @{
        FixedWidth=$Width;FixedHeight=$Height;LockAspectRatio=$false;TargetSamples=$Samples
        SamplesPerFrame=1;MaxBounces=2;Interactive=$true;InteractiveScale=1.0
        Denoise=$false;WorkBudgetMilliseconds=4.0;SeedMode='Fixed';Seed=1212501575
        RenderScale=1.0;AutoReset=$true;PreviewAov=0
    }.GetEnumerator()) { $temporary | Add-Member -NotePropertyName $entry.Key -NotePropertyValue $entry.Value -Force }
    $temporaryJson=$temporary | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($configPath,$temporaryJson,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'OfflineReferenceRenderer.capture.json'),$temporaryJson,[Text.UTF8Encoding]::new($false))
    foreach($backend in $GraphicsApi) {
        $capture=Join-Path $OutputDirectory ($backend+'.png')
        $log=Join-Path $OutputDirectory ($backend+'.capture.log')
        $arguments=@('editor','capture','-Configuration','RelWithDebInfo','-GraphicsApi',$backend,
            '-StrictGraphicsApi','-StartupViewportMode','he.viewport.path-tracing','-StartupScene',$StartupScene,
            '-Resolution','1920x1080','-Frames',"$Frames",'-TimeoutSeconds',"$TimeoutSeconds",'-CapturePath',$capture)
        $run=[ordered]@{backend=$backend;program=(Join-Path $repositoryRoot 'hectl.bat');arguments=$arguments
            startedUtc=[DateTime]::UtcNow.ToString('o');exitCode=$null;capture=$capture;log=$log
            artifact=$null;artifactSha256=$null;runtimeLogs=@();status='FAILED';visualReview='pending';providerVerification='pending'}
        $results.Add($run)
        & $run.program @arguments *> $log
        $run.exitCode=$LASTEXITCODE
        $raw=Get-Content -LiteralPath $log -Raw
        $artifactMatch=[regex]::Match($raw,'(?m)^artifact:\s*(.+?)\s*$')
        if($artifactMatch.Success){
            $run.artifact=$artifactMatch.Groups[1].Value.Trim()
            if(Test-Path -LiteralPath $run.artifact){$run.artifactSha256=(Get-FileHash -LiteralPath $run.artifact -Algorithm SHA256).Hash}
        }
        $logs=[Collections.Generic.List[string]]::new()
        foreach($match in [regex]::Matches($raw,'(?:stdout|stderr):\s*([^;\r\n]+)')){
            $source=$match.Groups[1].Value.Trim()
            if(Test-Path -LiteralPath $source){
                $destination=Join-Path $OutputDirectory ($backend+'.'+[IO.Path]::GetFileName($source))
                Copy-Item -LiteralPath $source -Destination $destination
                $logs.Add($destination)
            }
        }
        $run.runtimeLogs=$logs.ToArray()
        if($run.exitCode -ne 0 -or !(Test-Path -LiteralPath $capture)){throw "Offline capture failed for $backend; see $log"}
        if(!$run.artifactSha256 -or $logs.Count -lt 2){throw "Incomplete binary/runtime log provenance for $backend"}
        $runtimeText=($logs | ForEach-Object {Get-Content -LiteralPath $_ -Raw}) -join "`n"
        if($runtimeText -match '(?i)Failed to switch viewport renderer mode|offline.*(?:failed|unavailable|unsupported)|(?:failed|unavailable|unsupported).*offline|\[error\]|\[critical\]'){
            throw "Offline runtime reported a failure for $backend; inspect archived runtime logs"
        }
        $run.captureSha256=(Get-FileHash -LiteralPath $capture -Algorithm SHA256).Hash
        $run.status='CAPTURED_PENDING_VERIFICATION'
    }
    $manifest.status='CAPTURED_PENDING_VERIFICATION'
    $storageState='success'
} catch {
    $manifest.error=$_.Exception.Message
    throw
} finally {
    try {
        [IO.File]::WriteAllBytes($configPath,$originalBytes)
        $manifest.restoredSha256=(Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
        $manifest.restored=($manifest.restoredSha256 -eq $originalHash)
        if(!$manifest.restored){$manifest.status='FAILED';$storageState='failed';throw "Offline settings restoration hash mismatch. Recovery archive: $backupPath"}
    } finally {
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'manifest.json'),($manifest | ConvertTo-Json -Depth 20),[Text.UTF8Encoding]::new($false))
        Complete-HEStorageRun -Run $storageRun -State $storageState
    }
}
Write-Host "Offline captures archived at $OutputDirectory; provider and visual acceptance remain pending. No PASS has been inferred from process exit."
