param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$WorkDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Configuration = 'Release',
    [string]$ManagedProject = 'Scripts/CSharp/GameScripts.csproj',
    [string]$Dotnet = 'dotnet',
    [string]$CMake = 'cmake',
    [string]$Python = '',
    [switch]$Shipping,
    [ValidateRange(1,32)][int]$Parallel = 2,
    [ValidateSet('Desktop','Portable')][string]$Quality = 'Desktop',
    [ValidateSet('Windows','Android','Web','WeChat','macOS')][string]$Platform = 'Windows',
    [switch]$IsolatedCompiler,
    [switch]$EditorHost
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Dependencies/Environment.ps1')
Import-HEDependencyEnvironment
if ($Shipping -and $EditorHost) { throw 'The reload host cannot enter a Shipping build.' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$Project = [IO.Path]::GetFullPath($Project)
$work = [IO.Path]::GetFullPath($WorkDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
function Get-ManagedHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $hasher.Dispose(); $stream.Dispose() }
}
function Get-BuildInputStamp {
    $roots = @((Join-Path $Project 'Scripts'), (Join-Path $Project 'Config'), (Join-Path $repo 'Engine/Managed'), $PSScriptRoot)
    $inputs = foreach ($root in $roots) {
        Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notmatch '[\\/](bin|obj|artifacts)[\\/]' -and $_.Extension -in @('.cs','.csproj','.props','.targets','.json','.hemodule','.cmake','.py','.ps1') }
    }
    return (($inputs | Sort-Object FullName -Unique | ForEach-Object { $_.FullName + ':' + (Get-ManagedHash $_.FullName) }) -join "`n")
}
$initialInputStamp = Get-BuildInputStamp
$projectFile = Join-Path ([IO.Path]::GetFullPath($Project)) $ManagedProject
$moduleManifest = $null
$moduleReferences = ''
if (@(Get-ChildItem -LiteralPath (Join-Path $Project 'Scripts') -Filter '*.hemodule' -Recurse -File -ErrorAction SilentlyContinue).Count -gt 0) {
    if (!$Python) {
        if ($env:HE_BUILD_PYTHON) { $Python = $env:HE_BUILD_PYTHON }
        elseif (($PSVersionTable.PSEdition -eq 'Core') -and $IsMacOS) { $Python = (Get-Command python3 -ErrorAction Stop).Source }
        else { $Python = (& py -3.11 -c 'import sys; print(sys.executable)').Trim() }
    }
    $moduleWork = Join-Path $work 'modules'
    $moduleArguments = @((Join-Path $PSScriptRoot 'Modules.py'),'--project',[IO.Path]::GetFullPath($Project),'--engine',$repo,'--work',$moduleWork,'--platform',$Platform,'--configuration',$Configuration,'--dotnet',$Dotnet)
    if ($EditorHost) { $moduleArguments += @('--category','Runtime','--category','Editor') }
    & $Python @moduleArguments
    if ($LASTEXITCODE -ne 0) { throw 'Script module graph/build failed; previous generation remains active.' }
    $moduleManifest = Get-Content -Raw (Join-Path $moduleWork 'modules.json') | ConvertFrom-Json
    $references = ($moduleManifest.modules | ForEach-Object {
        '<Reference Include="' + [Security.SecurityElement]::Escape($_.assembly) + '"><HintPath>' + [Security.SecurityElement]::Escape($_.dll) + '</HintPath></Reference>'
    }) -join "`n"
    $moduleReferences = Join-Path $moduleWork 'ModuleReferences.props'
    ('<Project><ItemGroup>' + $references + '</ItemGroup></Project>') | Set-Content $moduleReferences -Encoding UTF8
    $projectFile = Join-Path $moduleWork 'GameScripts.csproj'
    ('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><RuntimeFrameworkVersion>10.0.10</RuntimeFrameworkVersion><EnableDefaultCompileItems>false</EnableDefaultCompileItems><Deterministic>true</Deterministic></PropertyGroup><ItemGroup><ProjectReference Include="' + [Security.SecurityElement]::Escape((Join-Path $repo 'Engine/Managed/HEngine.ScriptCore/HEngine.ScriptCore.csproj')) + '" /></ItemGroup><Import Project="' + [Security.SecurityElement]::Escape($moduleReferences) + '" /></Project>') | Set-Content $projectFile -Encoding UTF8
    & $Dotnet restore $projectFile --use-lock-file --force-evaluate
    if ($LASTEXITCODE -ne 0) { throw 'Module umbrella restore failed.' }
}
if (!(Test-Path -LiteralPath $projectFile)) { throw "C# plugin enabled but GameScripts project is missing: $projectFile" }
New-Item -ItemType Directory -Path $work -Force | Out-Null
function Invoke-Managed([string[]]$Arguments) {
    & $Dotnet @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Managed build failed ($LASTEXITCODE): $($Arguments[0])" }
}
Push-Location (Join-Path $repo 'Engine/Managed')
try {
    $sdkVersion = (& $Dotnet --version).Trim()
    if ($LASTEXITCODE -ne 0 -or $sdkVersion -ne '10.0.302') { throw 'C# plugin requires the pinned .NET SDK 10.0.302.' }
    $common = @('-c',$Configuration,"-p:HEngineRoot=$repo","-p:HEQuality=$Quality","-p:HEPlatform=$Platform",'--artifacts-path',(Join-Path $work 'artifacts'),
        "-p:GeneratedApiDir=$(Join-Path $work 'api')","-p:CMakeExecutable=$CMake",'-p:RestorePackagesWithLockFile=true','-p:RestoreLockedMode=true')
    $common += "-maxcpucount:$Parallel"
    if ($IsolatedCompiler) { $common += @('--disable-build-servers','-p:UseSharedCompilation=false','-nodeReuse:false') }
    if ($EditorHost) { $common += '-p:HEEditorHost=true' }
    if ($moduleReferences) { $common += "-p:ModuleReferencesFile=$moduleReferences" }
    $gameArgs = @('build',$projectFile) + $common + @('-o',(Join-Path $work 'game'))
    Invoke-Managed $gameArgs
    # Query evaluated Compile inputs instead of assuming every .cs on disk is built.
    $evaluation = @('msbuild',$projectFile,'-getItem:Compile',"-p:HEngineRoot=$repo","-p:Configuration=$Configuration","-p:HEPlatform=$Platform","-p:HEQuality=$Quality","-p:GeneratedApiDir=$(Join-Path $work 'api')")
    if ($IsolatedCompiler) { $evaluation += '-nodeReuse:false' }
    $compileJson = & $Dotnet @evaluation
    if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate the gameplay compilation inputs.' }
    $compile = ($compileJson -join "`n") | ConvertFrom-Json
    $sourceRoot = [Uri]::new((([IO.Path]::GetDirectoryName($projectFile)).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar), [UriKind]::Absolute)
    $sourceInputs = @()
    foreach ($sourcePath in @($projectFile) + @($compile.Items.Compile | ForEach-Object { $_.FullPath })) {
        $relative = [Uri]::UnescapeDataString($sourceRoot.MakeRelativeUri([Uri]::new([IO.Path]::GetFullPath($sourcePath), [UriKind]::Absolute)).ToString())
        $sourceInputs += [ordered]@{path=$relative;sha256=(Get-ManagedHash $sourcePath)}
    }
    $sourceInputs = @($sourceInputs | Sort-Object { $_.path })
    if ($moduleManifest) {
        $sourceInputs += @($moduleManifest.modules | ForEach-Object { [ordered]@{path=('module:' + $_.id);sha256=$_.identity} })
    }
    $sourceText = ($sourceInputs | ForEach-Object { $_.path + ':' + $_.sha256 }) -join "`n"
    $sourceHasher = [Security.Cryptography.SHA256]::Create()
    try { $sourceIdentity = [BitConverter]::ToString($sourceHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($sourceText))).Replace('-','').ToLowerInvariant() }
    finally { $sourceHasher.Dispose() }
    Invoke-Managed (@('build',(Join-Path $repo 'Tools/Scripting/RegistryGenerator/RegistryGenerator.csproj')) + $common + @('-o',(Join-Path $work 'generator')))
    Invoke-Managed @((Join-Path $work 'generator/RegistryGenerator.dll'),(Join-Path $work 'game/GameScripts.dll'),(Join-Path $work 'registry'),$sourceIdentity)
    if ($moduleManifest) {
        $moduleIndex = 0
        foreach ($module in $moduleManifest.modules) {
            Invoke-Managed @((Join-Path $work 'generator/RegistryGenerator.dll'),$module.dll,(Join-Path $work "registry/$moduleIndex"),$module.identity,"Module${moduleIndex}Registry",(Join-Path $moduleWork 'modules.json'))
            ++$moduleIndex
        }
        & $Python (Join-Path $PSScriptRoot 'MergeModuleRegistries.py') (Join-Path $moduleWork 'modules.json') (Join-Path $work 'registry') $sourceIdentity (Get-ManagedHash (Join-Path $work 'game/GameScripts.dll'))
        if ($LASTEXITCODE -ne 0) { throw 'Module registration merge failed.' }
    }
    Invoke-Managed @((Join-Path $work 'generator/RegistryGenerator.dll'),'--resolve-order',(Join-Path $work 'registry'))
    $publish = @('publish',(Join-Path $repo 'Engine/Managed/HEngine.ScriptRuntime/HEngine.ScriptRuntime.csproj')) + $common +
        @("-p:GameScriptsProject=$projectFile","-p:ScriptRegistryDir=$(Join-Path $work 'registry')",'-o',$output)
    # Native hosting uses a framework-dependent component. Shipping bundles the
    # exact shared runtime and passes its root explicitly to hostfxr.
    $publish += @('--self-contained','false')
    Invoke-Managed $publish
    if ($EditorHost) {
        Invoke-Managed (@('publish',(Join-Path $repo 'Engine/Managed/HEngine.ScriptHost/HEngine.ScriptHost.csproj')) + $common + @('-o',$output,'--self-contained','false'))
    }
    if ($moduleManifest) {
        foreach ($module in $moduleManifest.modules) {
            $moduleName = $module.assembly + '.dll'
            if ((Get-ManagedHash (Join-Path $output $moduleName)) -ne $module.sha256) { throw "Module changed after registration: $($module.id)" }
        }
    }
    $metadataPath = Join-Path $work 'registry/GameScripts.metadata.json'
    $metadata = Get-Content -Raw -LiteralPath $metadataPath | ConvertFrom-Json
    $actualHash = Get-ManagedHash (Join-Path $output 'GameScripts.dll')
    if ($metadata.identity -ne $actualHash) { throw 'GameScripts changed after registry generation; refusing to publish stale script metadata.' }
    Copy-Item -LiteralPath $metadataPath -Destination (Join-Path $output 'GameScripts.metadata.json') -Force
    if (($Shipping -and $Platform -eq 'Windows') -or $Platform -eq 'macOS') {
        $pinned = & (Join-Path $PSScriptRoot 'GetPinnedRuntime.ps1') -Platform $Platform
        $runtime = Join-Path $output 'Runtime'
        foreach ($file in $pinned.Manifest.files) {
            $target = Join-Path $runtime $file.path
            New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
            Copy-Item -LiteralPath (Join-Path $pinned.Root $file.path) -Destination $target -Force
            if ((Get-ManagedHash $target) -ne $file.sha256) { throw "Runtime staging integrity failure: $($file.path)" }
        }
        Copy-Item -LiteralPath (Join-Path $pinned.Root 'runtime-manifest.json') -Destination (Join-Path $runtime 'runtime-manifest.json') -Force
    }

    $receipt = [ordered]@{ schema=1; sdk=$sdkVersion; platform=$Platform; runtime='10.0.10'; shipping=[bool]$Shipping;
        apiSchemaSha256=(Get-ManagedHash (Join-Path $PSScriptRoot 'Api.schema.json'));
        gameplaySourceIdentity=$sourceIdentity; gameplaySourceInputs=$sourceInputs;
        gameScriptsSha256=$actualHash; runtimeAssemblySha256=(Get-ManagedHash (Join-Path $output 'HEngine.ScriptRuntime.dll'));
        managedFiles=@() }
    foreach ($name in @('GameScripts.dll','GameScripts.metadata.json','HEngine.ScriptCore.dll','HEngine.ScriptRuntime.dll','HEngine.ScriptRuntime.deps.json','HEngine.ScriptRuntime.runtimeconfig.json')) {
        $receipt.managedFiles += [ordered]@{path=$name;sha256=(Get-ManagedHash (Join-Path $output $name))}
    }
    if ($EditorHost) {
        $receipt.managedFiles += [ordered]@{path='HEngine.ScriptHost.dll';sha256=(Get-ManagedHash (Join-Path $output 'HEngine.ScriptHost.dll'))}
    }
    if ($moduleManifest) {
        $receipt.modules = @($moduleManifest.modules | ForEach-Object { [ordered]@{id=$_.id;assembly=$_.assembly;identity=$_.identity;sha256=$_.sha256;category=$_.category;references=$_.references} })
        foreach ($module in $moduleManifest.modules) { $receipt.managedFiles += [ordered]@{path=($module.assembly + '.dll');sha256=$module.sha256} }
    }
    if ((Get-BuildInputStamp) -cne $initialInputStamp) { throw 'Script build inputs changed during compilation; candidate rejected. Compile again from a stable source generation.' }
    $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'build.json') -Encoding UTF8
    Write-Host "C# build PASS; GameScripts SHA256=$actualHash; output=$output"
} finally { Pop-Location }
