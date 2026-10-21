param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$Metadata,
    [Parameter(Mandatory=$true)][string]$Manifest
)
$ErrorActionPreference = 'Stop'
$registry = Get-Content -LiteralPath $Metadata -Raw -Encoding UTF8 | ConvertFrom-Json
$cooked = Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8 | ConvertFrom-Json
$usedTypes = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
function Find-ScriptTypes($node) {
    if ($null -eq $node) { return }
    if ($node -is [System.Management.Automation.PSCustomObject]) {
        $script = $node.ScriptComponent
        if ($script -and $script.Backend -eq 'he.script.csharp') {
            $null = $usedTypes.Add([string]$script.Type)
        }
        foreach ($property in $node.PSObject.Properties) { Find-ScriptTypes $property.Value }
    } elseif ($node -is [array]) {
        foreach ($item in $node) { Find-ScriptTypes $item }
    }
}
# Only scripts referenced by shipped scenes/prefabs need their resources. Merely
# registering an unused sample script must not require shipping its assets.
foreach ($file in $cooked.Files) {
    if ([IO.Path]::GetExtension($file.Path) -in @('.he','.hprefab')) {
        $source = Join-Path $Project $file.Path
        # Cooker-generated import/showcase scenes have no project source here.
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Find-ScriptTypes (Get-Content -LiteralPath $source -Raw -Encoding UTF8 | ConvertFrom-Json)
        }
    }
}
foreach ($id in $usedTypes) {
    $type = @($registry.types | Where-Object id -eq $id)
    if ($type.Count -ne 1) { throw "Cooked content references unregistered Android script '$id'." }
    foreach ($dependency in $type[0].dependencies) {
        if ($dependency -notin $cooked.Files.Path) {
            throw "Android script '$id' requires uncooked resource '$dependency'. Declare it for the host cooker as well."
        }
    }
}
Write-Output "ANDROID SCRIPT DEPENDENCIES PASS: $($usedTypes.Count) shipped script types."
