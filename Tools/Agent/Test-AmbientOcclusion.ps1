[CmdletBinding()]
param(
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi = 'DX12',
    [string]$Scene = 'Assets/Scenes/MainGame.he',
    [string]$OutputDirectory = '',
    [string]$Resolution = '3840x2160'
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repositoryRoot ('out/agent/ao-live-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new evidence directory.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$owner = [guid]::NewGuid().ToString('N')
$sequence = 0
$commands = [Collections.Generic.List[object]]::new()
$results = [Collections.Generic.List[object]]::new()
$shellExecutable = (Get-Process -Id $PID).Path
function Write-Json($path, $value) {
    [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 40), [Text.UTF8Encoding]::new($false))
}
function Invoke-Owned([string[]]$arguments, [bool]$parse = $true) {
    $script:sequence++
    $prefix = Join-Path $OutputDirectory ('{0:d4}' -f $script:sequence)
    $fullArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'hectl.ps1')) +
        $arguments + @('-AgentOwnerTag',$owner,'-Configuration','RelWithDebInfo','-TimeoutSeconds','180')
    $quoted = @($fullArgs | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace([string]$_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' '
    $process = Start-Process $shellExecutable -ArgumentList $quoted -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput ($prefix+'.stdout.log') -RedirectStandardError ($prefix+'.stderr.log')
    try { $null = $process.Handle; $process.WaitForExit(); $code = $process.ExitCode }
    finally { $process.Dispose() }
    $commands.Add(@{ arguments=$fullArgs; exitCode=$code; log=$prefix+'.stdout.log' })
    if ($code -ne 0) { throw "Owned command failed; see $prefix.stdout.log" }
    if (!$parse) { return }
    $stream = [IO.File]::Open($prefix+'.stdout.log', 'Open', 'Read', 'ReadWrite')
    $reader = [IO.StreamReader]::new($stream)
    try { $raw = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $response = $raw.Substring($raw.IndexOf('{')) | ConvertFrom-Json
    if ($response.ok -eq $false) { throw ($response | ConvertTo-Json -Depth 10 -Compress) }
    if ($null -ne $response.result) { return $response.result }
    return $response
}
function Call-Agent([string]$method, [hashtable]$parameters = @{}) {
    $request = Join-Path $OutputDirectory ('request-{0:d4}.json' -f ($script:sequence+1))
    Write-Json $request $parameters
    return Invoke-Owned @('agent','call','-Method',$method,'-RequestFile',$request)
}
function Apply-Ao([string]$name, [hashtable]$patch) {
    $state = Call-Agent 'render.settings.get'
    $applied = Call-Agent 'render.settings.apply' @{ layer='Session'; baseRevision=$state.revision;
        baseHash=$state.hash; patch=$patch; persist=$false }
    if (!$applied.success) { throw "AO transaction $name rejected." }
    # A paused runtime still renders; let the settings transaction settle without
    # advancing simulation, so all quality captures share geometry and animation.
    $capture = Call-Agent 'capture.frame' @{ filename=$name+'.png' }
    $state = Call-Agent 'render.settings.get'
    foreach ($key in $patch.Keys) {
        $values = if ($key.StartsWith('Rendering.Debug.')) { $state.debug } else { $state.effective }
        if ($values.PSObject.Properties[$key].Value -ne $patch[$key]) { throw "Effective $key differs in $name." }
    }
    $destination = Join-Path $OutputDirectory ($name+'.png')
    Copy-Item -LiteralPath $capture.path -Destination $destination
    $results.Add(@{ name=$name; status='PASS'; evidenceType='runtime-manual'; patch=$patch;
        settings=$state.effective; capture=$destination; sha256=(Get-FileHash $destination).Hash })
}
$oldLocation = Get-Location
try {
    Set-Location $repositoryRoot
    Invoke-Owned @('agent','start','-StartupScene',$Scene,'-GraphicsApi',$GraphicsApi,
        '-StrictGraphicsApi','-Resolution',$Resolution,'-RenderValidation') $false
    $null = Call-Agent 'runtime.step' @{frames=90}
    $cases = [ordered]@{
        reference = @{ 'Rendering.AO.Technique'='GTAO'; 'Rendering.AO.SliceCount'=4;
            'Rendering.AO.StepsPerSide'=8; 'Rendering.AO.FastUpsample'=$false }
        fast = @{ 'Rendering.AO.Technique'='GTAO'; 'Rendering.AO.SliceCount'=4;
            'Rendering.AO.StepsPerSide'=8; 'Rendering.AO.FastUpsample'=$true }
        balanced = @{ 'Rendering.AO.Technique'='GTAO'; 'Rendering.AO.SliceCount'=3;
            'Rendering.AO.StepsPerSide'=6; 'Rendering.AO.FastUpsample'=$true }
        ssao = @{ 'Rendering.AO.Technique'='SSAO'; 'Rendering.AO.SampleCount'=16;
            'Rendering.AO.FastUpsample'=$true }
    }
    foreach ($view in @(0,12)) {
        foreach ($case in $cases.GetEnumerator()) {
            $patch = $case.Value.Clone()
            $patch['Rendering.Debug.PBR.Mode']=$view
            $patch['Rendering.Reflections.SSR.Enabled']=$false
            Apply-Ao "$view-$($case.Key)" $patch
        }
    }
    foreach ($path in @('Forward','Deferred')) {
        foreach ($technique in @('GTAO','SSAO','GTAO')) {
            Apply-Ao "$path-$technique-$sequence" @{ 'Rendering.Path'=$path;
                'Rendering.AO.Technique'=$technique; 'Rendering.Debug.PBR.Mode'=0 }
        }
    }
    $null = Call-Agent 'input.set' @{held=@('w')}
    try {
        foreach ($step in 1..3) {
            $null = Call-Agent 'runtime.step' @{frames=20}
            foreach ($case in $cases.GetEnumerator()) { Apply-Ao "motion-$step-$($case.Key)" $case.Value }
        }
    } finally { $null = Call-Agent 'input.clear' }
    foreach ($size in @(@(1919,1079),@(3840,2160))) {
        $null = Call-Agent 'window.resize' @{width=$size[0]; height=$size[1]}
        $null = Call-Agent 'runtime.step' @{frames=2}
        foreach ($technique in @('SSAO','GTAO')) {
            Apply-Ao "resize-$($size[0])-$technique" @{ 'Rendering.AO.Technique'=$technique }
        }
    }
    Write-Output "AO switching and capture: PASS ($($results.Count) cases)"
} finally {
    try { Invoke-Owned @('agent','stop') $false }
    finally {
        Write-Json (Join-Path $OutputDirectory 'commands.json') $commands
        Write-Json (Join-Path $OutputDirectory 'results.json') $results
        Set-Location $oldLocation
    }
}
