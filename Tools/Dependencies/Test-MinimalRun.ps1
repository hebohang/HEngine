$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$id = [guid]::NewGuid().ToString('N')
$relative = "Assets/Scenes/DependencyMinimal_$id.he"
$scene = Join-Path $repo "Project/$relative"
$archive = Join-Path $repo "out/agent/dependency-validation/minimal-scene-$id.he"
if ((Test-Path -LiteralPath $scene) -or !(Test-Path -LiteralPath (Split-Path $scene))) { throw 'Unsafe fixture path.' }
try {
    [IO.File]::WriteAllText($scene, '{}')
    & (Join-Path $repo 'Tools/Agent/Build.ps1') -Target HGame -Project (Join-Path $repo 'Project') -Configuration RelWithDebInfo -RenderBackends DX12 -RuntimeFeatures Minimal -Parallel 4 -Run -Wait -ExitAfterFrames 30 -TimeoutSeconds 90 -GraphicsApi DX12 -StrictGraphicsApi -Resolution 1280x720 -StartupScene $relative
    if ($LASTEXITCODE -ne 0) { throw 'Minimal runtime acceptance failed.' }
}
finally {
    if (Test-Path -LiteralPath $scene) { Move-Item -LiteralPath $scene -Destination $archive }
}
