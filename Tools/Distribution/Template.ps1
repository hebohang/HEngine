# Shared by SDK assembly and source-free exporters. A template is immutable;
# the exporter writes only to its own staging directory.
function Get-HEWindowsTemplatePlatform([string]$Engine) {
    $descriptor = Join-Path $Engine 'HEngine.distribution.json'
    if (Test-Path -LiteralPath $descriptor) {
        $platform = (Get-Content -LiteralPath $descriptor -Raw | ConvertFrom-Json).Platform
        if ($platform -notin @('Windows-x64','Windows-arm64')) { throw 'Unsupported SDK host platform.' }
        return $platform
    }
    . (Join-Path $Engine 'Tools/Agent/Architecture.ps1')
    return 'Windows-' + (Resolve-HeWindowsArchitecture)
}

function Resolve-HEExportTemplateFile([string]$Root, [string]$Relative) {
    if (!$Relative -or [IO.Path]::IsPathRooted($Relative) -or $Relative.Contains('\') -or
        $Relative.Contains(':') -or @($Relative.Split('/') | Where-Object { $_ -in @('','.','..') }).Count) {
        throw "Invalid export template path: $Relative"
    }
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    $candidate = [IO.Path]::GetFullPath((Join-Path $rootPath $Relative))
    if (!$candidate.StartsWith($rootPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Export template path escapes its root: $Relative"
    }
    $part = $rootPath
    foreach ($segment in @('') + $Relative.Split('/')) {
        if ($segment) { $part = Join-Path $part $segment }
        $item = Get-Item -LiteralPath $part -Force -ErrorAction Stop
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Export template contains a redirected path: $Relative" }
    }
    if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) { throw "Export template file is missing: $Relative" }
    return $candidate
}

function Get-HEExportTemplate([string]$Engine, [string]$Platform, [string]$Path = '', [switch]$Development) {
    if (!$Path) { $Path = Join-Path $Engine "ExportTemplates/$Platform" }
    $root = [IO.Path]::GetFullPath($Path)
    $manifestPath = Resolve-HEExportTemplateFile $root 'Template.json'
    $manifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $manifestPath | ConvertFrom-Json
    . (Join-Path $Engine 'Tools/Agent/Project.ps1')
    $validMode = if ($Development) {
        $manifest.Configuration -in @('Release','RelWithDebInfo') -and $manifest.PackGame -eq $false -and $manifest.DevAgent -eq $true
    } else {
        $manifest.Configuration -eq 'Release' -and $manifest.PackGame -eq $true -and $manifest.DevAgent -eq $false
    }
    if ($manifest.SchemaVersion -ne 1 -or $manifest.EngineVersion -ne (Get-HEEngineVersion $Engine) -or
        $manifest.Platform -cne $Platform -or $manifest.RuntimeFingerprint -cnotmatch '^[0-9a-f]{64}$' -or !$validMode) {
        throw 'Template does not match this engine/platform or the requested Shipping/Development mode.'
    }
    if (!$manifest.Files -or @($manifest.Files).Count -gt 20000) { throw 'Export template has no bounded payload inventory.' }
    $scriptApi = Join-Path $Engine 'Tools/Scripting/Api.schema.json'
    if ($manifest.ScriptApiHash -cne (Get-FileHash -LiteralPath $scriptApi -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw 'Template script/service contract differs from this SDK. Rebuild the player template before assembly or export.'
    }
    if ($Platform -eq 'Android-arm64' -and ($manifest.Player -cne 'Player/libhegame.so' -or
        $manifest.MinimumSdk -isnot [int] -or $manifest.MinimumSdk -lt 29 -or $manifest.MinimumSdk -gt 36)) {
        throw 'Android template must declare the ARM64 player and supported minimum SDK.'
    }
    $files = @{}
    foreach ($entry in $manifest.Files) {
        if ($entry.Path -ceq 'Template.json' -or $files.ContainsKey($entry.Path) -or $entry.Sha256 -cnotmatch '^[0-9a-f]{64}$') {
            throw "Invalid or duplicate export template inventory entry: $($entry.Path)"
        }
        $file = Resolve-HEExportTemplateFile $root $entry.Path
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.Sha256) {
            throw "Export template file was modified: $($entry.Path)"
        }
        $files[$entry.Path] = $file
    }
    $requiredFiles = @($manifest.Player)
    if (!$Development) { $requiredFiles += @($manifest.Cooker, $manifest.ShaderCompiler) }
    foreach ($required in $requiredFiles) {
        if (!$required -or !$files.ContainsKey($required)) { throw 'Export template does not inventory a required player/tool.' }
    }
    if ($Platform -in @('Windows-x64','Windows-arm64')) {
        . (Join-Path $Engine 'Tools/Dependencies/RuntimeDependencies.ps1')
        if (!$files.ContainsKey('Player/HEngine.Runtime.json')) { throw 'Windows template is missing its dynamic runtime contract; rebuild the template.' }
        $null = Test-HENativeRuntimeDependencies -Root "$root/Player" -Shipping:(!$Development)
    }
    return [pscustomobject]@{Root=$root;Manifest=$manifest;Files=$files}
}
