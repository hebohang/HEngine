$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Project.ps1')
$repositoryRoot = Split-Path (Split-Path $PSScriptRoot)
$testRoot = Join-Path $repositoryRoot ('out/agent/project-tests/' + [guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($testRoot)
$engine = Join-Path $testRoot 'Engine Checkout'
$null = [IO.Directory]::CreateDirectory((Join-Path $engine 'Engine/Templates'))
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'CMakeLists.txt') -Destination $engine
foreach ($template in @(Get-HEProjectTemplates)) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "Engine/Templates/$($template.Id)") -Destination (Join-Path $engine 'Engine/Templates') -Recurse
}
$null = [IO.Directory]::CreateDirectory((Join-Path $engine 'Engine/Content/Assets'))
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'Engine/Content/Assets/StarterContent') -Destination (Join-Path $engine 'Engine/Content/Assets') -Recurse
function Assert($condition, [string]$message) { if (-not $condition) { throw $message } }
function Reject([scriptblock]$action, [string]$message) {
    $rejected = $false
    try { & $action | Out-Null } catch { $rejected = $true }
    Assert $rejected $message
}
function Get-SourceSnapshot([string]$root) {
    return @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | ForEach-Object {
        $_.FullName.Substring($root.Length) + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }) -join "`n"
}
$previousEnvironment = $env:HE_PROJECT_DIR
try {
    Remove-Item Env:HE_PROJECT_DIR -ErrorAction SilentlyContinue
    Push-Location $engine
    $templatesBefore = Get-SourceSnapshot (Join-Path $engine 'Engine/Templates')
    $starterBefore = Get-SourceSnapshot (Join-Path $engine 'Engine/Content/Assets/StarterContent')
    Assert ((Resolve-HEProject -RepositoryRoot $engine).Root -eq (Join-Path $engine 'Engine/Templates/Blank')) 'Engine-only checkout lacks a default template'
    $a = New-HEProject $engine (Join-Path $engine 'Project') 'Default Game'
    $b = New-HEProject $engine (Join-Path $testRoot 'External Game') 'External Game'
    $scripted = New-HEProject $engine (Join-Path $testRoot 'Scripted Game') 'Scripted Game' -Template Scripted3D
    Assert (Test-Path (Join-Path $scripted.Root 'Scripts/CSharp/Player.cs')) 'Scripted template lost gameplay source'
    $scriptedPlugins = Get-Content -Raw (Join-Path $scripted.Root 'Config/Plugins.json') | ConvertFrom-Json
    Assert $scriptedPlugins.'he.script.csharp'.Game 'Scripted template does not enable shipping C#'
    $shared = New-HEProject $engine (Join-Path $testRoot 'Shared Game') 'Shared Game' -Template SharedArena
    $sharedProfiles = Get-Content -Raw -LiteralPath (Join-Path $shared.Root 'Config/BuildProfiles.json') | ConvertFrom-Json
    Assert ($sharedProfiles.profiles.Count -eq 4) 'Shared game lacks four build profiles'
    $android = Get-Content -Raw -LiteralPath (Join-Path $shared.Root 'Config/Android.json') | ConvertFrom-Json
    Assert ($android.applicationId -eq ('com.hengine.game' + $shared.ProjectId.Substring(5))) 'Android identity was not isolated'
    $createdStarters = @()
    foreach ($templateId in @('FirstPerson','TopDown')) {
        $sourceRoot = Join-Path $engine "Engine/Templates/$templateId"
        foreach ($instance in 1..2) {
            $starter = New-HEProject $engine (Join-Path $testRoot "$templateId Game $instance") "$templateId Game $instance" -Template ($templateId.ToLowerInvariant())
            $createdStarters += $starter
            $gameConfig = Get-Content -Raw -LiteralPath (Join-Path $starter.Root 'Config/Game.json') | ConvertFrom-Json
            Assert ($gameConfig.Game.Name -eq $starter.DisplayName) "$templateId game name was not customized"
            Assert (Test-Path -LiteralPath (Join-Path $starter.Root $gameConfig.Game.DefaultScene)) "$templateId default scene is missing"
            $scenePath = Join-Path $starter.Root 'Assets/Scenes/Main.he'
            Assert ((Get-FileHash -LiteralPath $scenePath).Hash -eq (Get-FileHash -LiteralPath (Join-Path $sourceRoot 'Assets/Scenes/Main.he')).Hash) "$templateId created the wrong scene"
            $scriptPath = "Scripts/CSharp/${templateId}Controller.cs"
            Assert ((Get-FileHash -LiteralPath (Join-Path $starter.Root $scriptPath)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $sourceRoot $scriptPath)).Hash) "$templateId lost gameplay source"
            $plugins = Get-Content -Raw -LiteralPath (Join-Path $starter.Root 'Config/Plugins.json') | ConvertFrom-Json
            Assert $plugins.'he.script.csharp'.Game "$templateId does not enable gameplay scripts"
            $binding = Get-Content -Raw -LiteralPath (Join-Path $starter.Root 'HEngine.workspace.local.json') | ConvertFrom-Json
            Assert ($binding.EngineRoot -eq $engine -and $binding.DefaultProject -eq '.') "$templateId is not bound to its engine and own project"
            Assert (Test-Path -LiteralPath (Join-Path $starter.Root 'Tools/hectl.ps1')) "$templateId has no independent launcher"
            Assert (-not (Test-Path -LiteralPath (Join-Path $starter.Root 'Assets/StarterContent'))) "$templateId duplicated shared Starter Content"
            $references = [regex]::Matches((Get-Content -Raw -LiteralPath $scenePath), '"(Assets/StarterContent/[^"\\]+)"')
            Assert ($references.Count -gt 0) "$templateId does not use shared Starter Content"
            foreach ($reference in $references) {
                Assert (Test-Path -LiteralPath (Join-Path (Join-Path $engine 'Engine/Content') $reference.Groups[1].Value)) "$templateId has a missing shared asset: $($reference.Groups[1].Value)"
            }
        }
    }
    $allProjects = @($a,$b,$scripted,$shared) + $createdStarters
    foreach ($field in @('ProjectId','ExecutableName','CacheKey','PackageKey')) {
        Assert (@($allProjects | Select-Object -ExpandProperty $field -Unique).Count -eq $allProjects.Count) "New projects share $field"
    }
    Assert ((Get-SourceSnapshot (Join-Path $engine 'Engine/Templates')) -eq $templatesBefore) 'Project creation modified a source template'
    Assert ((Get-SourceSnapshot (Join-Path $engine 'Engine/Content/Assets/StarterContent')) -eq $starterBefore) 'Project creation modified shared Starter Content'
    Assert ($a.ProjectId -ne $b.ProjectId) 'New projects share an identity'
    Assert ($b.CacheKey.Length -eq 16) 'Project build key exceeds the Windows path budget'
    Assert ((Resolve-HEProject -RepositoryRoot $engine).Root -eq $a.Root) 'Conventional default failed'
    Write-HEWorkspace (Join-Path $engine 'HEngine.workspace.json') ([ordered]@{SchemaVersion=1;DefaultProject='Project'})
    $null = Set-HEWorkspaceProject $engine $b.Root
    Assert ((Resolve-HEProject -RepositoryRoot $engine).Root -eq $b.Root) 'Persistent selection failed'
    $env:HE_PROJECT_DIR = $a.Root
    Assert ((Resolve-HEProject -RepositoryRoot $engine).Root -eq $a.Root) 'Environment precedence failed'
    Assert ((Resolve-HEProject $b.DescriptorPath $engine).Root -eq $b.Root) 'Explicit precedence failed'
    Remove-Item Env:HE_PROJECT_DIR
    Push-Location (Join-Path $a.Root 'Assets/Scenes')
    try { Assert ((Resolve-HEProject -RepositoryRoot $engine).Root -eq $a.Root) 'Nearest project discovery failed' }
    finally { Pop-Location }
    $descriptorBefore = [IO.File]::ReadAllText($b.DescriptorPath)
    Reject { New-HEProject $engine $b.Root 'Overwrite' } 'Existing directory was overwritten'
    Assert ([IO.File]::ReadAllText($b.DescriptorPath) -eq $descriptorBefore) 'Existing project changed'
    Reject { New-HEProject $engine (Join-Path $testRoot 'Invalid') "Bad`nName" } 'Multiline name accepted'
    Reject { New-HEProject $engine (Join-Path $testRoot 'Unknown Template') 'Unknown' -Template '../Blank' } 'Unknown/path template accepted'
    Assert (-not (Test-Path -LiteralPath (Join-Path $testRoot 'Unknown Template'))) 'Invalid template created a destination'
    Reject { New-HEProject $engine (Join-Path $testRoot 'Missing Parent/Game') 'Missing Parent' -Template FirstPerson } 'Missing parent directory accepted'
    foreach ($folder in @('NUL','Game.','Game?')) {
        Reject { New-HEProject $engine (Join-Path $testRoot $folder) 'Invalid Folder' -Template TopDown } 'Invalid Windows folder name accepted'
    }
    Reject { Set-HEWorkspaceProject $engine '' } 'Empty default selection accepted'
    $relative = New-HEProject $engine 'Relative Game' 'Relative Game'
    Assert ($relative.Root -eq (Join-Path $engine 'Relative Game')) 'Create ignored the PowerShell working directory'
    $clone = Join-Path $testRoot 'Cloned Game'
    Copy-Item -LiteralPath $b.Root -Destination $clone -Recurse
    $c = Resolve-HEProject $clone $engine
    Assert ($c.CacheKey -ne $b.CacheKey -and $c.PackageKey -ne $b.PackageKey) 'Same identity in different paths shares output'
    Write-HEWorkspace (Join-Path $engine 'HEngine.workspace.local.json') ([ordered]@{SchemaVersion=1;DefaultProject='Missing'})
    Reject { Resolve-HEProject -RepositoryRoot $engine } 'Broken binding silently selected another project'
    Write-HEWorkspace (Join-Path $engine 'HEngine.workspace.local.json') ([ordered]@{SchemaVersion=1;DefaultProject=''})
    Assert ((Resolve-HEProject -RepositoryRoot $engine).Root -eq $a.Root) 'Reset failed'
    $bad = Get-Content -Raw -LiteralPath $c.DescriptorPath | ConvertFrom-Json
    $bad.EngineVersion = '999.0.0'
    $bad | ConvertTo-Json | Set-Content -LiteralPath $c.DescriptorPath -Encoding UTF8
    Reject { Resolve-HEProject $c.Root $engine } 'Incompatible engine accepted'
    Add-HERecentProject $engine $a
    Add-HERecentProject $engine $b
    Add-HERecentProject $engine $a
    Assert (@(Get-HERecentProjects $engine).Count -eq 2) 'Recent entries were not deduplicated'
    '{broken' | Set-Content -LiteralPath (Join-Path $engine 'out/agent/projects/recent.json')
    Assert (@(Get-HERecentProjects $engine -WarningAction SilentlyContinue).Count -eq 0) 'Corrupt recent history prevented recovery'
    $projectFiles = Get-ChildItem -LiteralPath $b.Root -Recurse -File | Where-Object { $_.Extension -in @('.cpp','.h','.json','.he') }
    $leaks = $projectFiles | Select-String -Pattern 'Survivor|TouchSample|Starbloom'
    Assert (-not $leaks) 'Blank template depends on example game'
    Write-Output "PASS: five templates, starter scene/script/shared-asset references, isolated identities, unchanged template/content sources, workspace precedence, ancestor discovery, clone isolation, invalid directories/template/binding/version, non-overwrite, recent projects. Evidence: $testRoot"
}
finally {
    Pop-Location
    if ($null -eq $previousEnvironment) { Remove-Item Env:HE_PROJECT_DIR -ErrorAction SilentlyContinue }
    else { $env:HE_PROJECT_DIR = $previousEnvironment }
}

