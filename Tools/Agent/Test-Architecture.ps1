$ErrorActionPreference='Stop'
. "$PSScriptRoot/Architecture.ps1"
function Require([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$previous=$env:HE_TARGET_ARCHITECTURE
try {
    $env:HE_TARGET_ARCHITECTURE=''
    Require ((Resolve-HeWindowsArchitecture 'arm64') -eq 'arm64') 'Explicit ARM target was lost.'
    Require ((Get-HeArchitectureVariant '' 'x64') -eq '') 'Legacy x64 cache names changed.'
    Require ((Get-HeArchitectureVariant '' 'arm64') -eq 'arm64') 'ARM target needs its own cache.'
    Require ((Get-HeArchitectureVariant 'sdk' 'arm64') -eq 'sdk-arm64') 'Named ARM variant is not isolated.'
    foreach($variant in @('arm64','sdk-arm64','arm64-minimal','sdk-arm64-no-physics')) {
        $rejected=$false
        try { $null=Get-HeArchitectureVariant $variant 'x64' } catch { $rejected=$true }
        Require $rejected "x64 variant aliases ARM cache: $variant"
    }
    $env:HE_TARGET_ARCHITECTURE='arm64'
    Require ((Resolve-HeWindowsArchitecture) -eq 'arm64') 'Child build lost its target selection.'
    Require ((Resolve-HeWindowsArchitecture 'x64') -eq 'x64') 'Host tool override did not win.'
    $env:HE_TARGET_ARCHITECTURE='x86'
    $rejected=$false
    try { $null=Resolve-HeWindowsArchitecture } catch { $rejected=$true }
    Require $rejected 'Unsupported inherited target was accepted.'
    Write-Output 'unit-test PASS: Windows architecture selection and cache alias rejection.'
} finally { $env:HE_TARGET_ARCHITECTURE=$previous }
