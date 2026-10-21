param(
    [Parameter(Mandatory=$true)][string]$Engine,
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$Profile
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Engine).Path
$projectRoot = (Resolve-Path -LiteralPath $Project).Path
if (!(Test-Path -LiteralPath "$root/HEngine.distribution.json")) { throw 'Use a binary installation.' }
foreach ($forbidden in @('CMakeLists.txt','Engine/Source/Runtime/Launch','Engine/Source/Game','Project')) {
    if (Test-Path -LiteralPath "$root/$forbidden") { throw "Source-free fixture contains $forbidden" }
}
$profiles = Get-Content -Raw -Encoding UTF8 -LiteralPath "$projectRoot/Config/BuildProfiles.json" | ConvertFrom-Json
$selected = @($profiles.profiles | Where-Object id -eq $Profile)
if ($selected.Count -ne 1 -or $selected[0].platform -notin @('Web','WeChat')) { throw 'Choose one Web or WeChat profile.' }
$platform = $selected[0].platform
$logRoot = "$root/out/agent/portable-validation/$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $logRoot -Force
$oldPath = $env:PATH
try {
    $env:PATH = ($oldPath.Split(';') | Where-Object { $_ -notmatch '(?i)Visual Studio|VC[\\/]Tools|android-ndk' }) -join ';'
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/Tools/Build/Publish.ps1" `
            -Project $projectRoot -Profile $Profile -Action Build *> "$logRoot/export.log"
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
    if ($code -ne 0) { throw "Source-free $platform export failed; see $logRoot/export.log" }
    $receipt = Get-Content -Raw -Encoding UTF8 -LiteralPath "$projectRoot/Saved/Build/$Profile.json" | ConvertFrom-Json
    if (!$receipt.success) { throw 'No successful publication receipt.' }
    $required = if ($platform -eq 'Web') { @('index.html','main.js','_framework/dotnet.js','content-manifest.json') }
        else { @('game.js','game.json','project.config.json','game-manifest.json') }
    foreach ($file in $required) {
        if (!(Test-Path -LiteralPath (Join-Path $receipt.output $file) -PathType Leaf)) { throw "Missing portable artifact: $file" }
    }
    [ordered]@{schema=1;evidence='test-layer';pass=$true;platform=$platform;engine=$root;project=$projectRoot;
        output=$receipt.output;nativeEngineCompilation=$false;
        validation='Actual managed Wasm publish and platform exporter from a source-free SDK; no browser or phone gameplay claim.'} |
        ConvertTo-Json | Set-Content -LiteralPath "$logRoot/result.json" -Encoding UTF8
    Write-Output "Source-free $platform export PASS: $logRoot/result.json"
} finally { $env:PATH = $oldPath }
