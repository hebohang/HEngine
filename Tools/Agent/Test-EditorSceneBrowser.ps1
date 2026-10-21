param(
    [ValidateSet('DX12', 'Vulkan')]
    [string]$GraphicsApi = 'DX12',
    [ValidateRange(1200, 10000)]
    [int]$Frames = 1200
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$outputDirectory = Join-Path $repositoryRoot 'out/agent/rendering-evidence'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$logPath = Join-Path $outputDirectory ('editor-scene-browser-{0}.log' -f $GraphicsApi.ToLowerInvariant())
$previousDirectory = $env:HE_EDITOR_CONTENT_DIRECTORY
Push-Location $repositoryRoot
try {
    $env:HE_EDITOR_CONTENT_DIRECTORY = 'Assets/Scenes'
    & ./hectl.bat editor check -Frames $Frames -TimeoutSeconds 300 -GraphicsApi $GraphicsApi -RenderValidation *> $logPath
    if ($LASTEXITCODE -ne 0) { throw "Editor check failed; see $logPath" }
    $summary = Get-Content -LiteralPath $logPath -Raw
    $match = [regex]::Match($summary, 'stdout: ([^;\r\n]+); stderr:')
    if (-not $match.Success) { throw 'Editor check did not report its owned runtime log.' }
    $runtimeLog = Get-Content -LiteralPath $match.Groups[1].Value -Raw
    $expected = @(Get-ChildItem -LiteralPath 'Project/Assets/Scenes' -Filter '*.he' -File)
    foreach ($scene in $expected) {
        $completion = 'Scene thumbnail completed: Assets/Scenes/' + $scene.Name
        if (-not $runtimeLog.Contains($completion)) {
            throw "Missing thumbnail completion for $($scene.Name); see $logPath"
        }
    }
    if (-not $runtimeLog.Contains("Completed frame limit: $Frames")) {
        throw "Editor did not complete the requested frames; see $logPath"
    }
    Write-Output "runtime-manual PASS: $GraphicsApi; $($expected.Count) scene thumbnails; $Frames frames; $logPath"
}
finally {
    if ($null -eq $previousDirectory) {
        Remove-Item Env:HE_EDITOR_CONTENT_DIRECTORY -ErrorAction SilentlyContinue
    } else {
        $env:HE_EDITOR_CONTENT_DIRECTORY = $previousDirectory
    }
    Pop-Location
}
