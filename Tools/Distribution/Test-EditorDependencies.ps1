param([Parameter(Mandatory=$true)][string]$Engine, [switch]$Json)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../Packaging/PeImage.ps1"
. "$PSScriptRoot/../Dependencies/RuntimeDependencies.ps1"
$root = (Resolve-Path -LiteralPath $Engine).Path
$platform = (Get-Content -LiteralPath "$root/HEngine.distribution.json" -Raw | ConvertFrom-Json).Platform
if ($platform -notin @('Windows-x64','Windows-arm64')) { throw 'Unsupported SDK architecture.' }
$expectedMachine = if ($platform -eq 'Windows-arm64') { 0xaa64 } else { 0x8664 }
$native = Test-HENativeRuntimeDependencies -Root $root
$main = Get-PeImage -Path "$root/HEngineEditor.exe"
$exports = Get-PeExports -Image $main
$lock = Get-Content -LiteralPath "$PSScriptRoot/../Dependencies/native-runtime.lock.json" -Raw | ConvertFrom-Json
if ((Read-ExportedUInt32 -Image $main -Exports $exports -Name 'D3D12SDKVersion') -ne $lock.agilitySdkVersion -or
    (Read-ExportedStringPointer -Image $main -Exports $exports -Name 'D3D12SDKPath') -cne '.\D3D12\') {
    throw 'Editor Agility SDK exports do not match its app-local runtime.'
}
$licenses = Get-Content -LiteralPath "$root/Licenses/EditorDependencies.json" -Raw -Encoding UTF8 | ConvertFrom-Json
if ($licenses.SchemaVersion -ne 1 -or !$licenses.Files) { throw 'Editor license inventory is missing or invalid.' }
foreach ($entry in $licenses.Files) {
    if ($entry.Path -notmatch '^Licenses/[A-Za-z0-9_./-]+$' -or $entry.Path.Contains('..')) { throw 'Unsafe Editor license inventory path.' }
    if ((Get-FileHash -LiteralPath (Join-Path $root $entry.Path)).Hash.ToLowerInvariant() -cne $entry.Sha256) { throw "Editor license missing or modified: $($entry.Path)" }
}
$queue = [Collections.Generic.Queue[string]]::new()
$visited = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($name in @('HEngineEditor.exe','ProjectManager.exe','D3D12/D3D12Core.dll')) { $queue.Enqueue((Join-Path $root $name)) }
foreach ($file in $native.DynamicRoots) { $queue.Enqueue($file) }
# CoreCLR selects native DLLs through metadata, not the Editor import table.
foreach ($file in Get-ChildItem -LiteralPath "$root/Scripting/CSharp" -Filter '*.dll' -File -Recurse) { $queue.Enqueue($file.FullName) }
# Importers, cooker and shader compiler are separate processes used by the SDK.
foreach ($directory in @('Programs', "ExportTemplates/$platform/Tools")) {
    if (Test-Path -LiteralPath "$root/$directory") {
        foreach ($file in Get-ChildItem -LiteralPath "$root/$directory" -File -Recurse) {
            if ($file.Extension -in @('.exe','.dll')) { $queue.Enqueue($file.FullName) }
        }
    }
}
$system = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
$edges = 0
while ($queue.Count) {
    $path = $queue.Dequeue()
    if (!$visited.Add($path)) { continue }
    $image = Get-PeImage -Path $path
    $clr = Get-PeDataDirectory -Image $image -Index 14
    if ($null -ne $clr) {
        $offset = Convert-RvaToFileOffset -Image $image -Rva $clr.Rva
        $flags = Read-UInt32 -Bytes $image.Bytes -Offset ($offset + 16)
        if ($image.Machine -eq 0x14c -and ($flags -band 1) -and !($flags -band 0x20012)) { continue }
    }
    if ($image.Machine -ne $expectedMachine) { throw "Editor dependency does not match ${platform}: $path" }
    $imports = @(@(Get-PeNormalImports -Image $image | ForEach-Object { @{Name=$_;Delay=$false} }) +
        @(Get-PeDelayImports -Image $image | ForEach-Object { @{Name=$_;Delay=$true} }))
    foreach ($import in $imports) {
        $edges++
        $name = $import.Name
        if ($name -match '^(api-ms-win-|ext-ms-win-)') { continue }
        if ($name -ieq 'vulkan-1.dll' -and $import.Delay) { continue }
        $candidate = Join-Path (Split-Path $path) $name
        if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) { $candidate = Join-Path $root $name }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $queue.Enqueue($candidate); continue }
        if ($name -match '^(OpenImageDenoise|tbb|embree|vcruntime|msvcp|concrt|python|dxcompiler|dxil)') {
            throw "Missing app-local Editor dependency '$name' required by '$path'"
        }
        if (!(Test-Path -LiteralPath (Join-Path $system $name) -PathType Leaf)) {
            throw "Unresolved Editor dependency '$name' required by '$path'"
        }
    }
}
$result = [ordered]@{Evidence='static-check';Success=$true;Binaries=$visited.Count;Imports=$edges;Fsr=$native.Fsr;Oidn=$native.Oidn}
if ($Json) { $result | ConvertTo-Json } else { Write-Output "Editor PE dependency closure PASS: $($visited.Count) binaries, $edges imports; FSR=$($native.Fsr), OIDN=$($native.Oidn)" }
