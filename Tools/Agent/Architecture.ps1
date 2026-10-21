# Shared Windows host/target selection. Cross-target caches must never alias.
function Get-HeWindowsHostArchitecture {
    $value = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    if ($value -notin @('x64','arm64')) { throw "Unsupported Windows host architecture: $value" }
    return $value
}

function Resolve-HeWindowsArchitecture([string]$Architecture = '') {
    if (!$Architecture) { $Architecture = $env:HE_TARGET_ARCHITECTURE }
    if (!$Architecture) { $Architecture = Get-HeWindowsHostArchitecture }
    if ($Architecture -notin @('x64','arm64')) { throw "Unsupported Windows target architecture: $Architecture" }
    return $Architecture.ToLowerInvariant()
}

function Get-HeArchitectureVariant([string]$Variant, [string]$Architecture) {
    $arch = Resolve-HeWindowsArchitecture $Architecture
    if ($arch -eq 'x64') {
        if ($Variant -match '(^|-)arm64($|-)') { throw 'The arm64 cache-name segment is reserved for ARM64 targets.' }
        return $Variant
    }
    if (!$Variant) { return $arch }
    if ($Variant -notmatch '(^|-)arm64$') { return "$Variant-$arch" }
    return $Variant
}
