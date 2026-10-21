param([Parameter(Mandatory=$true)][string]$CMake)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$work = Join-Path $repo ('out/agent/plugin-settings-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$project = Join-Path $work 'Project'
New-Item -ItemType Directory -Force (Join-Path $project 'Config') | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'Project/Config/Plugins.json') -Destination (Join-Path $project 'Config/Plugins.json')
Copy-Item -LiteralPath (Join-Path $repo 'Project/Config/Plugins.Portable.json') -Destination (Join-Path $project 'Config/Plugins.Portable.json')
$results = @()
function Invoke-Case([string]$Name, [string]$Profile, [scriptblock]$Change, [bool]$ShouldPass, [switch]$Conflict) {
    $target = Join-Path $project ('Config/' + $Profile)
    $original = [IO.File]::ReadAllText($target)
    $pending = $original | ConvertFrom-Json
    & $Change $pending
    $request = Join-Path $work ($Name + '.json')
    @{Profile=$Profile;OriginalText=$(if ($Conflict) { $original + ' ' } else { $original });Pending=$pending} |
        ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $request -Encoding UTF8
    $before = (Get-FileHash -LiteralPath $target).Hash
    $passed = $false
    try {
        & "$PSScriptRoot/ApplyConfig.ps1" -Project $project -Request $request -CMake $CMake *> (Join-Path $work ($Name + '.log'))
        $passed = $true
    } catch { $_ | Out-String | Add-Content -LiteralPath (Join-Path $work ($Name + '.log')) }
    if ($passed -ne $ShouldPass) { throw "Unexpected result: $Name; evidence $work" }
    if (!$ShouldPass -and (Get-FileHash -LiteralPath $target).Hash -ne $before) { throw "Rejected configuration was modified: $Name" }
    if ($ShouldPass) {
        $actual = Get-Content -LiteralPath $target -Raw | ConvertFrom-Json
        if (($actual | ConvertTo-Json -Depth 32) -cne ($pending | ConvertTo-Json -Depth 32)) { throw "Saved configuration differs: $Name" }
    }
    $script:results += @{name=$Name;result='PASS';expectedAccepted=$ShouldPass}
}
Invoke-Case 'desktop-disable-csharp' 'Plugins.json' {param($p) $p.'he.script.csharp'.Editor=$false} $true
Invoke-Case 'desktop-reenable-csharp' 'Plugins.json' {param($p) $p.'he.script.csharp'.Editor=$true} $true
Invoke-Case 'portable-valid' 'Plugins.Portable.json' {} $true
Invoke-Case 'portable-python-rejected' 'Plugins.Portable.json' {param($p) $p.'he.script.python'.Game=$true} $false
Invoke-Case 'invalid-host-rejected' 'Plugins.json' {param($p) $p.'he.script.csharp'.Tools=$true} $false
Invoke-Case 'nonboolean-rejected' 'Plugins.json' {param($p) $p.'he.script.python'.Game='yes'} $false
Invoke-Case 'external-edit-conflict' 'Plugins.json' {} $false -Conflict
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $work 'results.json') -Encoding UTF8
Write-Output "PASS: $($results.Count) real configuration save/validation cases. Evidence: $work"
