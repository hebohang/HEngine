# Shared project identity and workspace resolution. No side effects when sourced.
function Get-HEEngineVersion([string]$RepositoryRoot) {
    $distribution = Join-Path $RepositoryRoot 'HEngine.distribution.json'
    if (Test-Path -LiteralPath $distribution) {
        $manifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $distribution | ConvertFrom-Json
        if ($manifest.SchemaVersion -ne 1 -or $manifest.EngineVersion -notmatch '^\d+\.\d+\.\d+$') {
            throw 'Invalid HEngine distribution manifest.'
        }
        return [string]$manifest.EngineVersion
    }
    $versionText = Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot 'CMakeLists.txt')
    return [regex]::Match($versionText, 'project\(HEngine VERSION ([0-9.]+)').Groups[1].Value
}

function Resolve-HEProject {
    param(
        [string]$ProjectPath,
        [string]$RepositoryRoot
    )

    if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
        $ProjectPath = $env:HE_PROJECT_DIR
    }
    if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
        $selection = Find-HEProjectSelection -RepositoryRoot $RepositoryRoot
        $ProjectPath = $selection.Path
    }
    elseif (-not [System.IO.Path]::IsPathRooted($ProjectPath) -and
        -not (Test-Path -LiteralPath $ProjectPath)) {
        $repositoryRelativePath = Join-Path $RepositoryRoot $ProjectPath
        if (Test-Path -LiteralPath $repositoryRelativePath) {
            $ProjectPath = $repositoryRelativePath
        }
    }

    $resolvedInput = Resolve-Path -LiteralPath $ProjectPath -ErrorAction Stop
    $inputItem = Get-Item -LiteralPath $resolvedInput.Path -Force
    if ($inputItem.PSIsContainer) {
        $projectRoot = $inputItem.FullName
        $descriptorPath = Join-Path $projectRoot "Project.heproject"
    }
    else {
        if ($inputItem.Name -ine "Project.heproject") {
            throw "-Project must name a project directory or Project.heproject: $($inputItem.FullName)"
        }
        $descriptorPath = $inputItem.FullName
        $projectRoot = $inputItem.Directory.FullName
    }

    if (-not (Test-Path -LiteralPath $descriptorPath -PathType Leaf)) {
        throw "Project descriptor was not found: $descriptorPath"
    }
    try {
        $descriptor = Get-Content -Raw -Encoding UTF8 -LiteralPath $descriptorPath | ConvertFrom-Json
    }
    catch {
        throw "Project descriptor is not valid JSON: $descriptorPath. $($_.Exception.Message)"
    }

    $projectId = [string]$descriptor.ProjectId
    if (($descriptor.SchemaVersion -isnot [int] -and $descriptor.SchemaVersion -isnot [long]) -or $descriptor.SchemaVersion -ne 1) { throw "Unsupported project schema: $descriptorPath" }
    foreach ($field in @('ProjectId', 'DisplayName', 'EngineVersion', 'ExecutableName', 'GameConfig', 'PackageConfig')) {
        if ($descriptor.$field -isnot [string]) { throw "Project $field must be a string: $descriptorPath" }
    }
    $displayName = [string]$descriptor.DisplayName
    if ([string]::IsNullOrWhiteSpace($displayName) -or $displayName.Length -gt 128 -or $displayName -match '[\r\n]') {
        throw "Project DisplayName must contain 1-128 characters on one line: $descriptorPath"
    }
    $engineVersion = Get-HEEngineVersion $RepositoryRoot
    if ($descriptor.EngineVersion -ne $engineVersion) {
        throw "Project requires engine $($descriptor.EngineVersion); this engine is $engineVersion."
    }
    foreach ($field in @('GameConfig', 'PackageConfig')) {
        $value = [string]$descriptor.$field
        if ($value -notmatch '^[A-Za-z0-9_./-]+\.json$' -or [IO.Path]::IsPathRooted($value) -or $value -match '(^|/)\.\.(/|$)') {
            throw "Project $field must be a project-relative JSON path."
        }
        if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $value) -PathType Leaf)) { throw "Missing project file: $value" }
    }
    $executableName = [string]$descriptor.ExecutableName
    if ($projectId.Length -gt 128 -or
        $projectId -notmatch '^[a-z0-9][a-z0-9._-]*$' -or
        $projectId.EndsWith('.') -or
        $projectId -match '(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') {
        throw "Project.heproject ProjectId must use lowercase ASCII letters, digits, '.', '_' or '-': $descriptorPath"
    }
    if ($executableName.Length -gt 64 -or
        $executableName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or
        $executableName.EndsWith('.') -or $executableName -match '(?i)\.exe$') {
        throw "Project.heproject ExecutableName must be a safe extension-free executable basename: $descriptorPath"
    }
    if ($executableName -match '(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') {
        throw "Project.heproject ExecutableName uses a reserved Windows filename: $descriptorPath"
    }

    $normalizedRoot = [System.IO.Path]::GetFullPath($projectRoot).TrimEnd('\', '/')
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $packageBytes = [System.Text.Encoding]::UTF8.GetBytes(
            ("${projectId}`n${normalizedRoot}").ToLowerInvariant())
        $packageKey = ([System.BitConverter]::ToString(
            $sha256.ComputeHash($packageBytes))).Replace('-', '').Substring(0, 16).ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }

    return [pscustomobject]@{
        Root = $normalizedRoot
        DescriptorPath = [System.IO.Path]::GetFullPath($descriptorPath)
        ProjectId = $projectId
        DisplayName = $displayName
        EngineVersion = $engineVersion
        ExecutableName = $executableName
        # Keep the Windows MSVC object paths short even for long project IDs.
        CacheKey = $packageKey
        PackageKey = $packageKey
    }
}


function Read-HEWorkspace {
    param([string]$Path)
    $value = Get-Content -Raw -Encoding UTF8 -LiteralPath $Path | ConvertFrom-Json
    if ($value.SchemaVersion -ne 1) { throw "Unsupported workspace schema: $Path" }
    return $value
}

function Find-HEProjectSelection {
    param([string]$RepositoryRoot, [string]$StartDirectory = (Get-Location).Path)
    # The nearest workspace/project wins. Broken explicit bindings never silently
    # fall through to a different game.
    $directory = [IO.Path]::GetFullPath($StartDirectory)
    while ($directory) {
        foreach ($name in @('HEngine.workspace.local.json', 'HEngine.workspace.json')) {
            $path = Join-Path $directory $name
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                $workspace = Read-HEWorkspace $path
                if (-not [string]::IsNullOrWhiteSpace($workspace.DefaultProject)) {
                    $selected = [string]$workspace.DefaultProject
                    if (-not [IO.Path]::IsPathRooted($selected)) { $selected = Join-Path $directory $selected }
                    return [pscustomobject]@{ Path = $selected; Source = $path }
                }
            }
        }
        if (Test-Path -LiteralPath (Join-Path $directory 'Project.heproject') -PathType Leaf) {
            return [pscustomobject]@{ Path = $directory; Source = 'ancestor project' }
        }
        if ($directory -ieq [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\','/')) { break }
        $parent = [IO.Directory]::GetParent($directory)
        $directory = if ($parent) { $parent.FullName } else { $null }
    }
    # An engine invoked from an unrelated working directory retains its binding.
    foreach ($name in @('HEngine.workspace.local.json', 'HEngine.workspace.json')) {
        $path = Join-Path $RepositoryRoot $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $workspace = Read-HEWorkspace $path
            if ($workspace.DefaultProject) {
                $selected = [string]$workspace.DefaultProject
                if (-not [IO.Path]::IsPathRooted($selected)) { $selected = Join-Path $RepositoryRoot $selected }
                return [pscustomobject]@{ Path = $selected; Source = $path }
            }
        }
    }
    $legacyProject = Join-Path $RepositoryRoot 'Project'
    if (Test-Path -LiteralPath (Join-Path $legacyProject 'Project.heproject') -PathType Leaf) {
        return [pscustomobject]@{ Path = $legacyProject; Source = 'conventional Project directory' }
    }
    return [pscustomobject]@{ Path = (Join-Path $RepositoryRoot 'Engine/Templates/Blank'); Source = 'engine Blank template' }
}

function Write-HEWorkspace {
    param([string]$Path, $Value)
    $Path = [IO.Path]::GetFullPath($Path)
    $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    # Atomic replacement under a short exclusive lock; never truncate a binding.
    $lock = [IO.File]::Open($Path + '.lock', 'OpenOrCreate', 'ReadWrite', 'None')
    $temporary = $Path + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
        if ([IO.File]::Exists($Path)) {
            $backup = $temporary + '.bak'
            [IO.File]::Replace($temporary, $Path, $backup)
            [IO.File]::Delete($backup)
        }
        else { [IO.File]::Move($temporary, $Path) }
    }
    finally { $lock.Dispose(); if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}

function Set-HEWorkspaceProject {
    param([string]$RepositoryRoot, [string]$ProjectPath, [string]$WorkspaceRoot = $RepositoryRoot)
    if ([string]::IsNullOrWhiteSpace($ProjectPath)) { throw 'Choose a project before setting the workspace default.' }
    $info = Resolve-HEProject -ProjectPath $ProjectPath -RepositoryRoot $RepositoryRoot
    $path = Join-Path $WorkspaceRoot 'HEngine.workspace.local.json'
    $value = [ordered]@{ SchemaVersion = 1; DefaultProject = $info.Root }
    Write-HEWorkspace -Path $path -Value $value
    return $info
}

function Get-HERecentProjects {
    [CmdletBinding()]
    param([string]$RepositoryRoot)
    $path = Join-Path $RepositoryRoot 'out/agent/projects/recent.json'
    if (Test-Path -LiteralPath $path) {
        try { return @(Read-HEWorkspace $path).Projects | Select-Object -First 20 }
        catch { Write-Warning "Recent project history is unreadable; use Browse to register projects again. $($_.Exception.Message)" }
    }
    return @()
}

function Add-HERecentProject {
    param([string]$RepositoryRoot, $ProjectInfo)
    $path = Join-Path $RepositoryRoot 'out/agent/projects/recent.json'
    $null = [IO.Directory]::CreateDirectory((Split-Path $path))
    $entries = @([ordered]@{ Path = $ProjectInfo.Root; Name = $ProjectInfo.DisplayName })
    $entries += @(Get-HERecentProjects $RepositoryRoot | Where-Object { $_.Path -ine $ProjectInfo.Root } | Select-Object -First 19)
    Write-HEWorkspace $path ([ordered]@{ SchemaVersion = 1; Projects = $entries })
}

function Get-HEProjectTemplates {
    # The CLI and project manager use the same IDs, labels and descriptions.
    @(
        [pscustomobject]@{ Id = 'Blank'; DisplayName = '空白项目'; Description = '空场景与基础项目配置，从零开始。' }
        [pscustomobject]@{ Id = 'Scripted3D'; DisplayName = '3D 移动示例 · Windows'; Description = '简单的 C# 移动脚本与 3D 场景。' }
        [pscustomobject]@{ Id = 'SharedArena'; DisplayName = '2D 守护游戏 · 四端共享规则'; Description = 'Windows、Android、Web 和微信共用游戏规则。' }
        [pscustomobject]@{ Id = 'FirstPerson'; DisplayName = '第一人称射击 · Windows'; Description = 'WASD 移动，按住右键转向，左键或空格射击，R 重置；共用 Starter Content。' }
        [pscustomobject]@{ Id = 'TopDown'; DisplayName = '俯视角射击 · Windows'; Description = 'WASD 移动，方向键瞄准，左键或空格射击，R 重置；共用 Starter Content。' }
    )
}

function New-HEProject {
    param([string]$RepositoryRoot, [string]$ProjectPath, [string]$Name,
        [string]$Template = 'Blank')
    $templates = @(Get-HEProjectTemplates)
    $selectedTemplate = $templates | Where-Object { $_.Id -ieq $Template }
    if (-not $selectedTemplate) { throw "Unknown project template '$Template'. Choose: $($templates.Id -join ', ')." }
    $Template = $selectedTemplate.Id
    $templateRoot = Join-Path $RepositoryRoot "Engine/Templates/$Template"
    if (-not (Test-Path -LiteralPath $templateRoot -PathType Container)) { throw "Project template is missing: $templateRoot" }
    if ([string]::IsNullOrWhiteSpace($ProjectPath)) { throw 'Supply -Project with a new directory.' }
    if ([string]::IsNullOrWhiteSpace($Name) -or $Name.Length -gt 128 -or $Name -match '[\x00-\x1f]') {
        throw 'Supply -Name with a display name (1-128 characters).'
    }
    $destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ProjectPath)
    $folder = Split-Path $destination -Leaf
    if (-not $folder -or $folder -match '[<>:"/\\|?*\x00-\x1f]' -or $folder.EndsWith('.') -or $folder.EndsWith(' ') -or
        $folder -match '(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') { throw 'Supply -Project with a valid new folder name.' }
    if (Test-Path -LiteralPath $destination) { throw "Destination already exists: $destination. Choose a new directory." }
    $parent = Split-Path $destination
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw "Parent directory does not exist: $parent" }
    $staging = Join-Path $parent ('.he-create-' + [guid]::NewGuid().ToString('N'))
    $null = [IO.Directory]::CreateDirectory($staging)
    # Failed staging is retained for inspection. Never overwrite existing files.
    Get-ChildItem -Force -LiteralPath (Join-Path $RepositoryRoot 'Engine/Templates/Blank') | Copy-Item -Destination $staging -Recurse -Force
    if ($Template -ne 'Blank') {
        foreach ($file in Get-ChildItem -LiteralPath $templateRoot -Recurse -File) {
            $relative = $file.FullName.Substring($templateRoot.Length + 1)
            $target = Join-Path $staging $relative
            $null = [IO.Directory]::CreateDirectory((Split-Path $target))
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        }
    }
    $id = 'game-' + [guid]::NewGuid().ToString('N')
    $descriptorPath = Join-Path $staging 'Project.heproject'
    $descriptor = Get-Content -Raw -Encoding UTF8 -LiteralPath $descriptorPath | ConvertFrom-Json
    $descriptor.ProjectId = $id
    $descriptor.DisplayName = $Name
    $descriptor.ExecutableName = 'Game' + $id.Substring(5, 8)
    $descriptor.EngineVersion = Get-HEEngineVersion $RepositoryRoot
    if ($Template -eq 'SharedArena') {
        $androidPath = Join-Path $staging 'Config/Android.json'
        $android = Get-Content -Raw -Encoding UTF8 -LiteralPath $androidPath | ConvertFrom-Json
        $android.applicationId = 'com.hengine.game' + $id.Substring(5)
        Write-HEWorkspace $androidPath $android
        $rulesPath = Join-Path $staging 'Config/GameRules.json'
        $rules = Get-Content -Raw -Encoding UTF8 -LiteralPath $rulesPath | ConvertFrom-Json
        $rules.title = $Name
        Write-HEWorkspace $rulesPath $rules
    }
    [IO.File]::WriteAllText($descriptorPath, ($descriptor | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    $configPath = Join-Path $staging 'Config/Game.json'
    $config = Get-Content -Raw -Encoding UTF8 -LiteralPath $configPath | ConvertFrom-Json
    $config.Game.Name = $Name
    [IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    Write-HEWorkspace (Join-Path $staging 'HEngine.workspace.local.json') ([ordered]@{
        SchemaVersion = 1; DefaultProject = '.'; EngineRoot = $RepositoryRoot
    })
    [IO.File]::WriteAllText((Join-Path $staging 'hectl.bat'), "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Tools\hectl.ps1`" %*`r`nexit /b %ERRORLEVEL%`r`n")
    $null = [IO.Directory]::CreateDirectory((Join-Path $staging 'Tools'))
    $launcher = @'
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$binding = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $root 'HEngine.workspace.local.json') | ConvertFrom-Json
$script = Join-Path $binding.EngineRoot 'Tools/Agent/hectl.ps1'
if (-not (Test-Path -LiteralPath $script)) { throw 'Engine installation is missing. Update HEngine.workspace.local.json EngineRoot.' }
if ($args -contains '-Project') { & $script @args }
else { & $script @args -Project $root }
exit $LASTEXITCODE
'@
    [IO.File]::WriteAllText((Join-Path $staging 'Tools/hectl.ps1'), $launcher, [Text.UTF8Encoding]::new($false))
    $null = Resolve-HEProject -ProjectPath $staging -RepositoryRoot $RepositoryRoot
    [IO.Directory]::Move($staging, $destination)
    return Resolve-HEProject -ProjectPath $destination -RepositoryRoot $RepositoryRoot
}

