param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$Request,
    [string]$CMake = 'cmake',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$projectRoot = [IO.Path]::GetFullPath($Project)
$requestData = Get-Content -LiteralPath $Request -Raw -Encoding UTF8 | ConvertFrom-Json
if ($requestData.Profile -notin @('Plugins.json','Plugins.Portable.json')) { throw 'Unknown plugin profile.' }
$target = Join-Path $projectRoot ('Config/' + $requestData.Profile)
$original = [IO.File]::ReadAllText($target)
if ($original -cne $requestData.OriginalText) { throw 'Plugin configuration changed on disk. Reload before saving.' }
$work = Join-Path $projectRoot ('Saved/PluginSettings/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
$candidate = Join-Path $work 'candidate.json'
$text = ($requestData.Pending | ConvertTo-Json -Depth 32) + "`n"
[IO.File]::WriteAllText($candidate, $text, [Text.UTF8Encoding]::new($false))
$platforms = if ($requestData.Profile -eq 'Plugins.Portable.json') { @('Web','WeChat') } else { @('Windows') }
foreach ($platformName in $platforms) {
    $passes = if ($platformName -eq 'Windows') { @('Development','Shipping') } else { @('Shipping') }
    foreach ($distribution in $passes) {
        $hosts = if ($distribution -eq 'Development') { 'Editor;DevGame;Game;Tests;Tools;RenderSmoke' } else { 'Game' }
        $arguments = @("-DPLUGIN_ROOT=$repo/Engine/Source/Plugins", "-DPLUGIN_CONFIG=$candidate",
            "-DVALIDATION_OUTPUT=$work/$platformName-$distribution", "-DPLUGIN_PLATFORM=$platformName",
            "-DPLUGIN_DISTRIBUTION=$distribution", "-DPLUGIN_HOSTS=$hosts", '-P', "$PSScriptRoot/ValidateConfig.cmake")
        & $CMake @arguments
        if ($LASTEXITCODE -ne 0) { throw "Plugin validation failed for $platformName/$distribution. Configuration was not saved. Evidence: $work" }
    }
}
# Recheck after validation; an external editor must never be silently overwritten.
if ([IO.File]::ReadAllText($target) -cne $original) { throw 'Plugin configuration changed during validation. Reload before saving.' }
if (!$CheckOnly) {
    $temporary = $target + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllText($temporary, $text, [Text.UTF8Encoding]::new($false))
        [IO.File]::Replace($temporary, $target, (Join-Path $work 'previous.json'))
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
}
Write-Output "Plugin configuration validated$(if (!$CheckOnly) {' and saved'}). Restart after rebuilding the affected targets. Evidence: $work"
