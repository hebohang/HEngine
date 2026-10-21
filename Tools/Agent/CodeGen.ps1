param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,

    [Parameter(Mandatory = $true)]
    [string]$GeneratedSourceDir,

    [ValidateSet("Status", "Check", "Run", "Ensure")]
    [string]$Action = "Run",

    [ValidateSet("Editor", "DevGame", "Tests", "Package")]
    [string]$CodeGenHost = "Editor",

    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "Debug",

    [string]$LibClangRoot = "",

    [switch]$UseManagedBuild,

    [switch]$Json,

    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Toolchain.ps1")

function Get-PathHash {
    param([string]$Value)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value.ToLowerInvariant())
        return ([System.BitConverter]::ToString($sha256.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
}

function Invoke-LoggedNative {
    param(
        [string]$Description,
        [string]$LogPath,
        [scriptblock]$Command
    )

    $parent = Split-Path -Parent $LogPath
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $exitCode = 1
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $Command *> $LogPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($exitCode -ne 0) {
        $primary = @(Get-Content -LiteralPath $LogPath -Tail 40 |
            Where-Object { $_ -match '(?i)(error|fatal|failed)' } |
            Select-Object -First 8)
        foreach ($line in $primary) {
            [Console]::Error.WriteLine($line)
        }
        throw "$Description failed with exit code $exitCode. Log: $LogPath"
    }
}

function Find-ParserExecutable {
    param([string]$BuildRoot)

    $candidates = @(Get-ChildItem -LiteralPath $BuildRoot -Filter HEngineParser.exe `
        -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '[\\/]CMakeFiles[\\/]' } |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($candidates.Count -eq 0) {
        return $null
    }
    return $candidates[0].FullName
}

function Find-ModernLibClang {
    param([string]$RequestedRoot)
    . (Join-Path $PSScriptRoot "LibClang.ps1")
    Resolve-HELibClang -Root $RequestedRoot
}
function Write-StateMissingResult {
    param(
        [string]$Reason,
        [int]$ExitCode
    )

    $result = [ordered]@{
        status = "dirty"
        configIdentity = ""
        outputRoot = $generatedSourceDir
        dirtyReasons = @($Reason)
        changed = 0
        unchanged = 0
        deleted = 0
        duration = 0
        logPath = $logPath
        manifestPath = Join-Path $generatedSourceDir ".he-codegen\manifest.json"
    }
    if ($Json) {
        Write-Output ($result | ConvertTo-Json -Compress -Depth 5)
    }
    elseif (-not $Quiet) {
        Write-Output ("codegen: dirty reason={0}; output={1}" -f $Reason, $generatedSourceDir)
    }
    exit $ExitCode
}

function Write-WrapperResult {
    param([string]$ResultText)

    $result = $ResultText | ConvertFrom-Json
    if ($Json) {
        Write-Output ($result | ConvertTo-Json -Compress -Depth 8)
    }
    elseif (-not $Quiet) {
        $reasonValues = @($result.dirtyReasons)
        $reasonText = if ($reasonValues.Count -gt 0) {
            $boundedReasons = @($reasonValues | Select-Object -First 3) -join ","
            if ($reasonValues.Count -gt 3) {
                $boundedReasons += ",+{0} more" -f ($reasonValues.Count - 3)
            }
            "; reasons=" + $boundedReasons
        }
        else {
            ""
        }
        Write-Output ("codegen: {0}; changed={1}; unchanged={2}; deleted={3}; duration={4}ms{5}; manifest={6}; log={7}" -f `
            $result.status, $result.changed, $result.unchanged, $result.deleted,
            $result.duration, $reasonText, $result.manifestPath, $result.logPath)
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\', '/')
$projectRoot = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/')
$generatedSourceDir = [System.IO.Path]::GetFullPath($GeneratedSourceDir).TrimEnd('\', '/')
$toolchain = Get-HeVisualStudio
$libClang = Find-ModernLibClang -RequestedRoot $LibClangRoot
$codegenRoot = Join-Path $repositoryRoot "out\agent\codegen"
$toolBuildDir = Join-Path $codegenRoot ("tool-v3-ninja-{0}-{1}-clang{2}" -f $toolchain.Toolset, $toolchain.ToolsVersion, $libClang.Major)
$projectHash = (Get-PathHash -Value $projectRoot).Substring(0, 12)
$outputHash = (Get-PathHash -Value $generatedSourceDir).Substring(0, 12)
$effectiveConfiguration = if ($CodeGenHost -eq "Package") { "Release" } else { $Configuration }
$stateKey = "{0}-{1}-{2}-{3}" -f $projectHash, $outputHash, $CodeGenHost.ToLowerInvariant(), $effectiveConfiguration.ToLowerInvariant()
$stateKey += "-$($toolchain.Toolset)-$($toolchain.ToolsVersion)"
$stateBuildDir = Join-Path $codegenRoot ("state\" + $stateKey)
$stateRuntimeDir = Join-Path $stateBuildDir "bin"
$stateContextPath = Join-Path $stateBuildDir ("codegen\context-{0}.txt" -f $effectiveConfiguration)
$stateInputList = Join-Path $stateBuildDir "codegen\input-headers.txt"
$stateResultPath = Join-Path $stateBuildDir ("codegen\result-{0}.json" -f $effectiveConfiguration)
$stateVerifyResultPath = Join-Path $stateBuildDir ("codegen\verify-result-{0}.json" -f $effectiveConfiguration)
$driver = Join-Path $repositoryRoot "Tools\Agent\CodeGenDriver.ps1"
$templateRoot = Join-Path $repositoryRoot "Engine\Source\Programs\HEngineParser\Template"
$logRoot = Join-Path $repositoryRoot "out\agent\logs"
$logPath = Join-Path $logRoot ("codegen-{0}-{1}-{2}.log" -f $CodeGenHost.ToLowerInvariant(), $effectiveConfiguration.ToLowerInvariant(), $outputHash)

if (-not (Test-Path -LiteralPath $driver -PathType Leaf)) {
    throw "Codegen driver was not found: $driver"
}

if ($UseManagedBuild) {
    $managedBuildRoot = if ($CodeGenHost -eq "Package") {
        [System.IO.Path]::GetDirectoryName(
            [System.IO.Path]::GetDirectoryName($generatedSourceDir))
    }
    else {
        [System.IO.Path]::GetDirectoryName($generatedSourceDir)
    }
    $managedContextPath = Join-Path $managedBuildRoot ("codegen\context-{0}.txt" -f $effectiveConfiguration)
    $managedInputList = Join-Path $managedBuildRoot "codegen\input-headers.txt"
    $managedResultPath = Join-Path $managedBuildRoot ("codegen\result-{0}.json" -f $effectiveConfiguration)
    $managedVerifyResultPath = Join-Path $managedBuildRoot ("codegen\verify-result-{0}.json" -f $effectiveConfiguration)
    $managedCachePath = Join-Path $managedBuildRoot "CMakeCache.txt"
    $managedParser = Find-ParserExecutable -BuildRoot $managedBuildRoot
    $bManagedConfigured =
        (Test-Path -LiteralPath $managedContextPath -PathType Leaf) -and
        (Test-Path -LiteralPath $managedInputList -PathType Leaf) -and
        (Test-Path -LiteralPath $managedCachePath -PathType Leaf)
    if ($bManagedConfigured) {
        $expectedGenerator = if ($CodeGenHost -eq 'Package') { $toolchain.Generator } else { 'Ninja' }
        $bManagedConfigured = @(Get-HeFreshConfigureArguments -BuildRoot $managedBuildRoot `
            -Compiler $toolchain.Compiler -Generator $expectedGenerator).Count -eq 0
    }
    if (-not $bManagedConfigured -and $Action -eq "Run") {
        $UseManagedBuild = $false
    }
    elseif (-not $bManagedConfigured -or
            ($Action -in @("Status", "Check") -and $null -eq $managedParser)) {
        Write-StateMissingResult -Reason "managedBuildStateMissing" `
            -ExitCode $(if ($Action -eq "Status") { 0 } else { 1 })
    }
}

if ($UseManagedBuild) {
    $managedDriverParameters = @{
        RepositoryRoot = $repositoryRoot
        ProjectRoot = $projectRoot
        OutputRoot = $generatedSourceDir
        ParserExecutable = $managedParser
        InputList = $managedInputList
        ContextFile = $managedContextPath
        TemplateRoot = $templateRoot
        Module = "HEngine"
        LogPath = $logPath
    }
    if ($Action -in @("Status", "Check")) {
        $managedDriverParameters.Action = $Action
        $managedDriverParameters.Json = $Json
        $managedDriverParameters.Quiet = $Quiet
        & $driver @managedDriverParameters
        exit $LASTEXITCODE
    }

    . (Join-Path $PSScriptRoot 'Storage.ps1')
    $managedCacheLease = Open-HECacheLease -CachePath $managedBuildRoot
    try {
    Import-HeToolchain -Toolchain $toolchain
    $managedResultTimeBefore = if (Test-Path -LiteralPath $managedResultPath -PathType Leaf) {
        (Get-Item -LiteralPath $managedResultPath).LastWriteTimeUtc.Ticks
    }
    else {
        [int64]0
    }
    $managedVerifyTimeBefore = if (Test-Path -LiteralPath $managedVerifyResultPath -PathType Leaf) {
        (Get-Item -LiteralPath $managedVerifyResultPath).LastWriteTimeUtc.Ticks
    }
    else {
        [int64]0
    }
    $managedGenerator = Select-String -LiteralPath $managedCachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' |
        Select-Object -First 1
    $managedBuildLog = Join-Path $logRoot ("codegen-managed-{0}-{1}.log" -f $CodeGenHost.ToLowerInvariant(), $effectiveConfiguration.ToLowerInvariant())
    Invoke-LoggedNative -Description "managed HEngineCodeGen build node" -LogPath $managedBuildLog -Command {
        if ($null -ne $managedGenerator -and $managedGenerator.Matches[0].Groups[1].Value -notlike 'Ninja*') {
            & cmake --build $managedBuildRoot --config $effectiveConfiguration --target HEngineCodeGen --parallel 4
        }
        else {
            & cmake --build $managedBuildRoot --target HEngineCodeGen --parallel 4
        }
    }
    if ($null -eq $managedParser) {
        $managedParser = Find-ParserExecutable -BuildRoot $managedBuildRoot
        if ($null -eq $managedParser) {
            throw "Managed HEngineCodeGen build completed without an HEngineParser executable under $managedBuildRoot"
        }
        $managedDriverParameters.ParserExecutable = $managedParser
    }
    $managedResultTimeAfter = if (Test-Path -LiteralPath $managedResultPath -PathType Leaf) {
        (Get-Item -LiteralPath $managedResultPath).LastWriteTimeUtc.Ticks
    }
    else {
        [int64]0
    }
    $managedVerifyTimeAfter = if (Test-Path -LiteralPath $managedVerifyResultPath -PathType Leaf) {
        (Get-Item -LiteralPath $managedVerifyResultPath).LastWriteTimeUtc.Ticks
    }
    else {
        [int64]0
    }
    if ($managedResultTimeAfter -ne 0 -and $managedResultTimeAfter -ne $managedResultTimeBefore) {
        $managedResultText = (Get-Content -LiteralPath $managedResultPath -Raw -Encoding UTF8).Trim()
    }
    elseif ($managedVerifyTimeAfter -ne 0 -and $managedVerifyTimeAfter -ne $managedVerifyTimeBefore) {
        $managedResultText = (Get-Content -LiteralPath $managedVerifyResultPath -Raw -Encoding UTF8).Trim()
    }
    else {
        $managedDriverParameters.Action = "Status"
        $managedDriverParameters.Json = $true
        $managedResultText = ((& $driver @managedDriverParameters) -join "").Trim()
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
    Write-WrapperResult -ResultText $managedResultText
    exit 0
    }
    finally { $managedCacheLease.Dispose() }
}

$parserExecutable = Find-ParserExecutable -BuildRoot $toolBuildDir
$bStateReady = $null -ne $parserExecutable -and
    (Test-Path -LiteralPath $stateContextPath -PathType Leaf) -and
    (Test-Path -LiteralPath $stateInputList -PathType Leaf)

if ($Action -in @("Status", "Check")) {
    if (-not $bStateReady) {
        Write-StateMissingResult -Reason "generationStateMissing" `
            -ExitCode $(if ($Action -eq "Check") { 1 } else { 0 })
    }
    $driverParameters = @{
        Action = $Action
        RepositoryRoot = $repositoryRoot
        ProjectRoot = $projectRoot
        OutputRoot = $generatedSourceDir
        ParserExecutable = $parserExecutable
        InputList = $stateInputList
        ContextFile = $stateContextPath
        TemplateRoot = $templateRoot
        Module = "HEngine"
        LogPath = $logPath
        Json = $Json
        Quiet = $Quiet
    }
    & $driver @driverParameters
    exit $LASTEXITCODE
}

New-Item -ItemType Directory -Force -Path $toolBuildDir, $stateBuildDir, $logRoot | Out-Null
Import-HeToolchain -Toolchain $toolchain
$toolCachePath = Join-Path $toolBuildDir "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $toolCachePath -PathType Leaf)) {
    Invoke-LoggedNative -Description "HEngineParser tool configure" `
        -LogPath (Join-Path $logRoot "codegen-tool-configure.log") -Command {
        & cmake -S $repositoryRoot -B $toolBuildDir `
            "-DCMAKE_C_COMPILER=$($toolchain.Compiler)" `
            "-DCMAKE_CXX_COMPILER=$($toolchain.Compiler)" `
            -DHE_CODEGEN_TOOL_ONLY=ON `
            "-DHE_CODEGEN_LIBCLANG_ROOT=$($libClang.Root)" `
            -DCPP_CODE_GEN=OFF `
            -DCMAKE_BUILD_TYPE=Debug `
            -G Ninja
    }
}
Invoke-LoggedNative -Description "HEngineParser tool build" `
    -LogPath (Join-Path $logRoot "codegen-tool-build.log") -Command {
    & cmake --build $toolBuildDir --target HEngineParser --parallel 4
}
$parserExecutable = Find-ParserExecutable -BuildRoot $toolBuildDir
if ($null -eq $parserExecutable) {
    throw "HEngineParser build completed without an executable under $toolBuildDir"
}

$packGame = if ($CodeGenHost -eq "Package") { "ON" } else { "OFF" }
$devGame = if ($CodeGenHost -in @("DevGame", "Tests")) { "ON" } else { "OFF" }
$enableGm = if ($CodeGenHost -eq "Package") { "OFF" } else { "ON" }
$buildTests = if ($CodeGenHost -eq "Tests") { "ON" } else { "OFF" }

$stateCachePath = Join-Path $stateBuildDir "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $stateCachePath -PathType Leaf)) {
    Invoke-LoggedNative -Description "codegen state configure" `
        -LogPath (Join-Path $logRoot ("codegen-state-configure-{0}.log" -f $stateKey)) -Command {
        & cmake -S $repositoryRoot -B $stateBuildDir `
            "-DCMAKE_C_COMPILER=$($toolchain.Compiler)" `
            "-DCMAKE_CXX_COMPILER=$($toolchain.Compiler)" `
            "-DHE_PROJECT_DIR=$projectRoot" `
            "-DHE_GENERATED_SOURCE_DIR=$generatedSourceDir" `
            "-DHE_CODEGEN_PARSER_EXECUTABLE=$parserExecutable" `
            "-DHE_CODEGEN_LIBCLANG_ROOT=$($libClang.Root)" `
            "-DHE_RUNTIME_OUTPUT_DIR=$stateRuntimeDir" `
            "-DPACK_GAME=$packGame" `
            "-DHE_BUILD_DEV_GAME=$devGame" `
            "-DHE_ENABLE_GM=$enableGm" `
            "-DHE_BUILD_TESTS=$buildTests" `
            "-DHE_CODEGEN_HOST_OVERRIDE=$CodeGenHost" `
            -DCPP_CODE_GEN=OFF `
            -DHE_ENABLE_TRACY=OFF `
            -DHE_TRACY_ON_DEMAND=ON `
            "-DCMAKE_BUILD_TYPE=$effectiveConfiguration" `
            -G Ninja
    }
}

$resultWriteTimeBefore = if (Test-Path -LiteralPath $stateResultPath -PathType Leaf) {
    (Get-Item -LiteralPath $stateResultPath).LastWriteTimeUtc.Ticks
}
else {
    [int64]0
}
$verifyResultWriteTimeBefore = if (Test-Path -LiteralPath $stateVerifyResultPath -PathType Leaf) {
    (Get-Item -LiteralPath $stateVerifyResultPath).LastWriteTimeUtc.Ticks
}
else {
    [int64]0
}
Invoke-LoggedNative -Description "HEngineCodeGen build node" -LogPath $logPath -Command {
    & cmake --build $stateBuildDir --target HEngineCodeGen --parallel 4
}

if (-not (Test-Path -LiteralPath $stateContextPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $stateInputList -PathType Leaf)) {
    throw "Codegen state files were not produced under $stateBuildDir"
}

$resultWriteTimeAfter = if (Test-Path -LiteralPath $stateResultPath -PathType Leaf) {
    (Get-Item -LiteralPath $stateResultPath).LastWriteTimeUtc.Ticks
}
else {
    [int64]0
}
$verifyResultWriteTimeAfter = if (Test-Path -LiteralPath $stateVerifyResultPath -PathType Leaf) {
    (Get-Item -LiteralPath $stateVerifyResultPath).LastWriteTimeUtc.Ticks
}
else {
    [int64]0
}
if ($resultWriteTimeAfter -ne 0 -and $resultWriteTimeAfter -ne $resultWriteTimeBefore) {
    $resultText = (Get-Content -LiteralPath $stateResultPath -Raw -Encoding UTF8).Trim()
}
elseif ($verifyResultWriteTimeAfter -ne 0 -and $verifyResultWriteTimeAfter -ne $verifyResultWriteTimeBefore) {
    $resultText = (Get-Content -LiteralPath $stateVerifyResultPath -Raw -Encoding UTF8).Trim()
}
else {
    $statusParameters = @{
        Action = "Status"
        RepositoryRoot = $repositoryRoot
        ProjectRoot = $projectRoot
        OutputRoot = $generatedSourceDir
        ParserExecutable = $parserExecutable
        InputList = $stateInputList
        ContextFile = $stateContextPath
        TemplateRoot = $templateRoot
        Module = "HEngine"
        LogPath = $logPath
        Json = $true
    }
    $resultText = ((& $driver @statusParameters) -join "").Trim()
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
Write-WrapperResult -ResultText $resultText
exit 0
