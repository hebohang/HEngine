param(
    [ValidateSet('DX12', 'Vulkan')]
    [string]$GraphicsApi = 'DX12',
    [string[]]$Scenes = @(
        'Assets/Scenes/MainGame.he',
        'Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he',
        'Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he',
        'Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixedEnvironment/Benchmark.he'
    ),
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot ('out/agent/scene-open-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$outputRoot = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$results = @()
Push-Location $repositoryRoot
try {
    foreach ($scene in $Scenes) {
        $scenePath = Join-Path 'Project' $scene
        $sourceHash = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
        $logPath = Join-Path $outputRoot ('scene-{0}-{1}.log' -f $results.Count, $GraphicsApi)
        & ./hectl.bat editor check -StartupScene $scene -GraphicsApi $GraphicsApi -StrictGraphicsApi `
            -Frames 120 -Resolution 1280x720 -RenderValidation -TimeoutSeconds 240 *> $logPath
        if ($LASTEXITCODE -ne 0) { throw "Editor check failed: $logPath" }
        $launchLog = Get-Content -LiteralPath $logPath -Raw
        $match = [regex]::Match($launchLog, 'stdout: ([^;\r\n]+); stderr:')
        if (-not $match.Success) { throw "Missing owned runtime log: $logPath" }
        $runtimeLog = Get-Content -LiteralPath $match.Groups[1].Value -Raw
        if (-not $runtimeLog.Contains('Scene opened:') -or
            -not $runtimeLog.Contains('; dirty=false') -or
            $runtimeLog -match 'Scene (opened|document state):[^\r\n]*dirty=true' -or
            -not $runtimeLog.Contains('Completed frame limit: 120')) {
            throw "Scene did not remain clean through initial updates: $scene; $($match.Groups[1].Value)"
        }
        if ((Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash -ne $sourceHash) {
            throw "Opening changed the source scene: $scene"
        }
        $results += [ordered]@{
            evidence = 'runtime-manual'; result = 'PASS'; scene = $scene; graphicsApi = $GraphicsApi
            frames = 120; sourceSha256 = $sourceHash; runtimeLog = $match.Groups[1].Value
            command = "hectl.bat editor check -StartupScene $scene -GraphicsApi $GraphicsApi -StrictGraphicsApi -Frames 120 -Resolution 1280x720 -RenderValidation -TimeoutSeconds 240"
        }
        $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputRoot 'results.json')
        Write-Output "runtime-manual PASS: clean open + 120 frames + unchanged source; $GraphicsApi; $scene"
    }
}
finally {
    Pop-Location
}
