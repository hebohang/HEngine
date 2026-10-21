param([Parameter(Mandatory=$true)][string]$Engine, [switch]$Json)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Engine).Path
$sdkPlatform = (Get-Content -LiteralPath "$root/HEngine.distribution.json" -Raw | ConvertFrom-Json).Platform
if ($sdkPlatform -notin @('Windows-x64','Windows-arm64')) { throw 'Unsupported Windows SDK architecture.' }
$contract = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot 'RequiredFiles.json') | ConvertFrom-Json
if ($contract.schema -ne 1) { throw 'Unsupported distribution layout contract.' }
$errors = [Collections.Generic.List[string]]::new()
foreach ($relative in $contract.files) {
    $relative = $relative.Replace('Windows-x64', $sdkPlatform)
    if ($sdkPlatform -eq 'Windows-arm64') { $relative = $relative.Replace('@esbuild/win32-x64/', '@esbuild/win32-arm64/') }
    $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (!$path.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Distribution contract path escapes engine: $relative"
    }
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { $errors.Add("Missing: $relative") }
}
foreach ($unwanted in @('Project','HEngine.workspace.json','HEngine.workspace.local.json')) {
    if (Test-Path -LiteralPath (Join-Path $root $unwanted)) { $errors.Add("Engine distribution contains a project or workspace binding: $unwanted") }
}
if (Test-Path -LiteralPath "$root/Engine/Source") {
    foreach ($file in Get-ChildItem -LiteralPath "$root/Engine/Source" -Recurse -File) {
        if ($file.Extension -in @('.cpp','.h','.hpp','.c') -or $file.Name -eq 'CMakeLists.txt') {
            $errors.Add("Binary SDK contains native source: $($file.FullName.Substring($root.Length+1))")
        }
    }
}
$manifestPath = Join-Path $root 'HEngine.distribution.json'
try {
    $apiCheck = & py -3.11 (Join-Path $PSScriptRoot '../Scripting/GenerateReference.py') --engine $root --check
    if ($LASTEXITCODE -ne 0) { $errors.Add("C# API reference does not match the SDK: $apiCheck") }
} catch { $errors.Add("Could not verify C# API reference: $($_.Exception.Message)") }
if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
    try {
        $manifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $manifestPath | ConvertFrom-Json
        if ($manifest.SchemaVersion -ne 1 -or $manifest.EngineVersion -notmatch '^\d+\.\d+\.\d+$' -or
            $manifest.Platform -notin @('Windows-x64','Windows-arm64') -or $manifest.Editor -ne 'HEngineEditor.exe') {
            $errors.Add('Invalid distribution manifest.')
        }
    } catch { $errors.Add('Invalid distribution manifest JSON.') }
}
try {
    . (Join-Path $PSScriptRoot '../Dependencies/RuntimeDependencies.ps1')
    $null = Test-HENativeRuntimeDependencies -Root $root
    $null = Test-HENativeRuntimeDependencies -Root "$root/ExportTemplates/$sdkPlatform/Player" -Shipping
    $null = Test-HENativeRuntimeDependencies -Root "$root/DevelopmentTemplates/$sdkPlatform/Player"
} catch { $errors.Add($_.Exception.Message) }
$result = [ordered]@{schema=1;evidence='static-check';success=($errors.Count -eq 0);engine=$root;checked=$contract.files.Count;errors=@($errors)}
if ($Json) { $result | ConvertTo-Json -Depth 4 }
if ($errors.Count) { throw ("Distribution layout FAIL:`n" + ($errors -join "`n")) }
if (!$Json) { Write-Output "Distribution layout PASS: $($contract.files.Count) required files; no bundled project. This is not runtime or export validation." }
