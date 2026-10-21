# Shared by staging and package verification. Module declarations may extend the
# fixed host closure, never its category or filename policy.
function Get-HEModulePackageFiles($Receipt) {
    if (!$Receipt.PSObject.Properties['modules']) { return @() }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $ids = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $files = @()
    foreach ($module in @($Receipt.modules)) {
        if (!$module) { continue }
        if ($module.category -ne 'Runtime' -or $module.id -notmatch '^[A-Za-z0-9_.-]{1,128}$' -or !$ids.Add($module.id) -or
            $module.assembly -notmatch '^[A-Za-z_][A-Za-z0-9_.]{0,127}$' -or $module.assembly -match '^(System\.|HEngine\.|GameScripts$)' -or
            !$names.Add($module.assembly) -or $module.identity -notmatch '^[a-f0-9]{64}$' -or $module.sha256 -notmatch '^[a-f0-9]{64}$') {
            throw 'Invalid Runtime module in Shipping manifest.'
        }
        $files += ($module.assembly + '.dll')
    }
    foreach ($module in @($Receipt.modules)) {
        if (!$module) { continue }
        foreach ($reference in $module.references) { if (!$ids.Contains($reference)) { throw "Missing packaged module dependency: $reference" } }
        $entry = @($Receipt.managedFiles | Where-Object { $_.path -eq ($module.assembly + '.dll') })
        if ($entry.Count -ne 1 -or $entry[0].sha256 -ne $module.sha256) { throw "Module output identity differs from file manifest: $($module.id)" }
    }
    return $files
}
