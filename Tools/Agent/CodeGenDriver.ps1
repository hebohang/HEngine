[CmdletBinding()]
param(
    [ValidateSet('Status', 'Check', 'Run', 'Ensure')]
    [string]$Action = 'Ensure',

    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $true)]
    [string]$ParserExecutable,

    [Parameter(Mandatory = $true)]
    [string]$InputList,

    [Parameter(Mandatory = $true)]
    [string]$ContextFile,

    [Parameter(Mandatory = $true)]
    [string]$TemplateRoot,

    [string]$Module = 'HEngine',

    [switch]$Json,

    [switch]$Quiet,

    [string]$LogPath,

    [string]$ResultPath,

    [string]$StampPath,

    [ValidateSet('', 'write', 'flush', 'rename')]
    [string]$TestOnlyPublishFault = '',

    [Parameter(ValueFromRemainingArguments = $true)]
    [object[]]$RemainingArguments
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:ManifestSchemaVersion = 1
$script:DriverVersion = '2'
$script:ExpectedIndexSchemaVersion = 1
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:PathComparison = if ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT) {
    [System.StringComparison]::OrdinalIgnoreCase
} else {
    [System.StringComparison]::Ordinal
}
$script:ArgumentNormalizationError = $null
$script:TestOnlyPublishFault = $TestOnlyPublishFault
$script:TestOnlyPublishFaultArmed = $false
$script:TestOnlyPublishFaultStageCount = 0
$script:TestOnlyForwardPublish = $false

foreach ($remainingArgument in @($RemainingArguments)) {
    if ([string]::IsNullOrWhiteSpace([string]$remainingArgument)) {
        continue
    }
    switch ([string]$remainingArgument) {
        '--json' { $Json = $true; continue }
        '--quiet' { $Quiet = $true; continue }
        default { $script:ArgumentNormalizationError = "Unknown argument '$remainingArgument'."; continue }
    }
}

function Sort-StringsOrdinal {
    param(
        [object[]]$Values,
        [switch]$Unique
    )

    $strings = New-Object 'System.Collections.Generic.List[string]'
    foreach ($value in @($Values)) {
        [void]$strings.Add([string]$value)
    }
    $strings.Sort([System.StringComparer]::Ordinal)
    if (-not $Unique) {
        return @($strings.ToArray())
    }
    $result = New-Object 'System.Collections.Generic.List[string]'
    $previous = $null
    $bHasPrevious = $false
    foreach ($value in $strings) {
        if (-not $bHasPrevious -or
            -not [string]::Equals($previous, $value, [System.StringComparison]::Ordinal)) {
            [void]$result.Add($value)
            $previous = $value
            $bHasPrevious = $true
        }
    }
    return @($result.ToArray())
}

function Sort-ObjectsOrdinal {
    param(
        [object[]]$Values,
        [Parameter(Mandatory = $true)][scriptblock]$KeySelector
    )

    $entries = New-Object 'System.Collections.Generic.List[object]'
    $index = 0
    foreach ($value in @($Values)) {
        [void]$entries.Add([pscustomobject]@{
            key = [string](& $KeySelector $value)
            index = $index
            value = $value
        })
        $index++
    }
    $comparison = [System.Comparison[object]]{
        param($left, $right)
        $result = [System.StringComparer]::Ordinal.Compare([string]$left.key, [string]$right.key)
        if ($result -ne 0) {
            return $result
        }
        return [int]$left.index - [int]$right.index
    }
    $entries.Sort($comparison)
    return @($entries | ForEach-Object { $_.value })
}

function Get-FullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [string]$BasePath
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'A required path is empty.'
    }

    if ([System.IO.Path]::IsPathRooted($Path)) {
        $fullPath = [System.IO.Path]::GetFullPath($Path)
    }
    else {
        if ([string]::IsNullOrWhiteSpace($BasePath)) {
            $BasePath = (Get-Location).Path
        }
        $fullPath = [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
    }

    $pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
    if ([string]::Equals($fullPath, $pathRoot, $script:PathComparison)) {
        return $pathRoot
    }
    return $fullPath.TrimEnd('\', '/')
}

function ConvertTo-LogicalPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return $Path.Replace('\', '/')
}

function Test-PathEqual {
    param(
        [Parameter(Mandatory = $true)][string]$Left,
        [Parameter(Mandatory = $true)][string]$Right
    )

    return [string]::Equals(
        (Get-FullPath -Path $Left),
        (Get-FullPath -Path $Right),
        $script:PathComparison)
}

function Test-PathContained {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$AllowRoot
    )

    $fullRoot = Get-FullPath -Path $Root
    $fullPath = Get-FullPath -Path $Path
    if ([string]::Equals($fullRoot, $fullPath, $script:PathComparison)) {
        return [bool]$AllowRoot
    }

    $prefix = $fullRoot + [System.IO.Path]::DirectorySeparatorChar
    return $fullPath.StartsWith($prefix, $script:PathComparison)
}

function Get-RelativePathStrict {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $fullRoot = Get-FullPath -Path $Root
    $fullPath = Get-FullPath -Path $Path
    if (-not (Test-PathContained -Root $fullRoot -Path $fullPath)) {
        throw "Path is outside the required root: '$fullPath' (root '$fullRoot')."
    }

    $relative = $fullPath.Substring($fullRoot.Length + 1)
    if ([string]::IsNullOrWhiteSpace($relative)) {
        throw "A file path was expected below '$fullRoot'."
    }
    return ConvertTo-LogicalPath -Path $relative
}

function Resolve-RelativeOutputPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "Generated output path must be a non-empty relative path: '$RelativePath'."
    }

    $normalizedRelative = $RelativePath.Replace('/', '\').TrimStart('\')
    if ($normalizedRelative -eq '.he-codegen-index.json' -or
        $normalizedRelative.StartsWith('.he-codegen\', $script:PathComparison) -or
        $normalizedRelative -eq '.he-codegen') {
        throw "Generated output uses the reserved .he-codegen path: '$RelativePath'."
    }

    $candidate = Get-FullPath -Path (Join-Path $Root $normalizedRelative)
    if (-not (Test-PathContained -Root $Root -Path $candidate)) {
        throw "Generated output escapes its output root: '$RelativePath'."
    }
    return $candidate
}

function Assert-NoReparsePoint {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = Get-FullPath -Path $Path
    $root = [System.IO.Path]::GetPathRoot($fullPath)
    $remaining = $fullPath.Substring($root.Length)
    $current = $root
    foreach ($part in @($remaining -split '[\\/]' | Where-Object { $_ -ne '' })) {
        $current = Join-Path $current $part
        if (-not [System.IO.File]::Exists($current) -and
            -not [System.IO.Directory]::Exists($current)) {
            continue
        }
        $attributes = [System.IO.File]::GetAttributes($current)
        if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse points are not allowed in a codegen transaction path: '$current'."
        }
    }
}

function Assert-SafeOutputRoot {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedRepositoryRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot
    )

    $volumeRoot = [System.IO.Path]::GetPathRoot($ResolvedOutputRoot)
    if ([string]::Equals($ResolvedOutputRoot, $volumeRoot, $script:PathComparison)) {
        throw "OutputRoot cannot be a filesystem root: '$ResolvedOutputRoot'."
    }
    if (Test-PathEqual -Left $ResolvedOutputRoot -Right $ResolvedRepositoryRoot) {
        throw 'OutputRoot cannot equal RepositoryRoot.'
    }
    if (Test-PathEqual -Left $ResolvedOutputRoot -Right $ResolvedProjectRoot) {
        throw 'OutputRoot cannot equal ProjectRoot.'
    }
    $projectBuildRoot = Get-FullPath -Path (Join-Path $ResolvedProjectRoot 'build')
    $projectOutRoot = Get-FullPath -Path (Join-Path $ResolvedProjectRoot 'out')
    $bAllowedProjectOutput =
        (Test-PathContained -Root $projectBuildRoot -Path $ResolvedOutputRoot -AllowRoot) -or
        (Test-PathContained -Root $projectOutRoot -Path $ResolvedOutputRoot -AllowRoot)
    if ((Test-PathContained -Root $ResolvedProjectRoot -Path $ResolvedOutputRoot -AllowRoot) -and
        -not $bAllowedProjectOutput) {
        throw 'OutputRoot inside ProjectRoot must be under its build/ or out/ directory.'
    }
    if (Test-PathContained -Root $ResolvedOutputRoot -Path $ResolvedRepositoryRoot -AllowRoot) {
        throw 'OutputRoot cannot contain RepositoryRoot.'
    }
    if (Test-PathContained -Root $ResolvedOutputRoot -Path $ResolvedProjectRoot -AllowRoot) {
        throw 'OutputRoot cannot contain ProjectRoot.'
    }
    if (Test-PathContained -Root $ResolvedRepositoryRoot -Path $ResolvedOutputRoot -AllowRoot) {
        $allowedBuildRoot = Get-FullPath -Path (Join-Path $ResolvedRepositoryRoot 'build')
        $allowedOutRoot = Get-FullPath -Path (Join-Path $ResolvedRepositoryRoot 'out')
        if (-not (Test-PathContained -Root $allowedBuildRoot -Path $ResolvedOutputRoot -AllowRoot) -and
            -not (Test-PathContained -Root $allowedOutRoot -Path $ResolvedOutputRoot -AllowRoot) -and
            -not $bAllowedProjectOutput) {
            throw "OutputRoot inside RepositoryRoot must be under build/ or out/: '$ResolvedOutputRoot'."
        }
    }
    Assert-NoReparsePoint -Path $ResolvedOutputRoot
}

function Assert-SafeAuxiliaryPath {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedPath,
        [Parameter(Mandatory = $true)][string]$ResolvedRepositoryRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $projectBuildRoot = Get-FullPath -Path (Join-Path $ResolvedProjectRoot 'build')
    $projectOutRoot = Get-FullPath -Path (Join-Path $ResolvedProjectRoot 'out')
    $bAllowedProjectPath =
        (Test-PathContained -Root $projectBuildRoot -Path $ResolvedPath -AllowRoot) -or
        (Test-PathContained -Root $projectOutRoot -Path $ResolvedPath -AllowRoot)
    if ((Test-PathContained -Root $ResolvedProjectRoot -Path $ResolvedPath -AllowRoot) -and
        -not $bAllowedProjectPath) {
        throw "$Label inside ProjectRoot must be under its build/ or out/ directory: '$ResolvedPath'."
    }
    if (Test-PathContained -Root $ResolvedRepositoryRoot -Path $ResolvedPath -AllowRoot) {
        $allowedBuildRoot = Get-FullPath -Path (Join-Path $ResolvedRepositoryRoot 'build')
        $allowedOutRoot = Get-FullPath -Path (Join-Path $ResolvedRepositoryRoot 'out')
        if (-not (Test-PathContained -Root $allowedBuildRoot -Path $ResolvedPath -AllowRoot) -and
            -not (Test-PathContained -Root $allowedOutRoot -Path $ResolvedPath -AllowRoot) -and
            -not $bAllowedProjectPath) {
            throw "$Label inside RepositoryRoot must be under build/ or out/: '$ResolvedPath'."
        }
    }
    Assert-NoReparsePoint -Path $ResolvedPath
}

function Get-Sha256Bytes {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha256.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
}

function Get-Sha256Text {
    param([AllowEmptyString()][string]$Text)

    return Get-Sha256Bytes -Bytes $script:Utf8NoBom.GetBytes($Text)
}

function Get-FileHashRecord {
    param(
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$DisplayPath
    )

    $fullPath = Get-FullPath -Path $Path
    if ([string]::IsNullOrWhiteSpace($DisplayPath)) {
        $DisplayPath = ConvertTo-LogicalPath -Path $fullPath
    }

    if (-not [System.IO.File]::Exists($fullPath)) {
        return [pscustomobject][ordered]@{
            kind = $Kind
            path = ConvertTo-LogicalPath -Path $fullPath
            displayPath = $DisplayPath
            exists = $false
            size = $null
            hash = $null
        }
    }

    $stream = New-Object System.IO.FileStream(
        $fullPath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = ([System.BitConverter]::ToString($sha256.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
        return [pscustomobject][ordered]@{
            kind = $Kind
            path = ConvertTo-LogicalPath -Path $fullPath
            displayPath = $DisplayPath
            exists = $true
            size = [int64]$stream.Length
            hash = $hash
        }
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $record = Get-FileHashRecord -Kind 'file' -Path $Path
    if (-not $record.exists) {
        throw "File does not exist: '$Path'."
    }
    return $record.hash
}

function ConvertTo-StableValue {
    param($Value)

    if ($null -eq $Value) {
        return $null
    }
    if ($Value -is [string] -or
        $Value -is [char] -or
        $Value -is [bool] -or
        $Value -is [byte] -or
        $Value -is [sbyte] -or
        $Value -is [int16] -or
        $Value -is [uint16] -or
        $Value -is [int32] -or
        $Value -is [uint32] -or
        $Value -is [int64] -or
        $Value -is [uint64] -or
        $Value -is [single] -or
        $Value -is [double] -or
        $Value -is [decimal]) {
        return $Value
    }

    if ($Value -is [System.Collections.IDictionary]) {
        $ordered = [ordered]@{}
        foreach ($key in @(Sort-StringsOrdinal -Values @($Value.Keys | ForEach-Object { [string]$_ }))) {
            $ordered[$key] = ConvertTo-StableValue -Value $Value[$key]
        }
        return $ordered
    }

    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string]) {
        $items = New-Object System.Collections.ArrayList
        foreach ($item in $Value) {
            [void]$items.Add((ConvertTo-StableValue -Value $item))
        }
        return ,([object[]]$items.ToArray())
    }

    $properties = @(Sort-ObjectsOrdinal `
        -Values @($Value.PSObject.Properties | Where-Object { $_.MemberType -match 'Property' }) `
        -KeySelector { param($property) [string]$property.Name })
    if ($properties.Count -gt 0) {
        $ordered = [ordered]@{}
        foreach ($property in $properties) {
            $ordered[$property.Name] = ConvertTo-StableValue -Value $property.Value
        }
        return $ordered
    }

    return [string]$Value
}

function ConvertTo-DeterministicJson {
    param([Parameter(Mandatory = $true)]$Value)

    return ($Value | ConvertTo-Json -Depth 100 -Compress) + [char]10
}

function Read-ContextProtocol {
    param([Parameter(Mandatory = $true)][string]$Path)

    $entries = New-Object System.Collections.ArrayList
    if (-not [System.IO.File]::Exists($Path)) {
        return @($entries.ToArray())
    }

    $lineNumber = 0
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        $lineNumber++
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
            continue
        }
        $separator = $trimmed.IndexOf('=')
        if ($separator -le 0) {
            throw "Invalid codegen context at ${Path}:${lineNumber}; expected key=value."
        }
        $key = $trimmed.Substring(0, $separator).Trim().ToLowerInvariant()
        $value = $trimmed.Substring($separator + 1).Trim()
        if ($key -notmatch '^[a-z][a-z0-9_.-]*$') {
            throw "Invalid codegen context key '$key' at ${Path}:${lineNumber}."
        }
        if ([string]::IsNullOrWhiteSpace($value)) {
            throw "Empty codegen context value for '$key' at ${Path}:${lineNumber}."
        }
        [void]$entries.Add([pscustomobject][ordered]@{
            key = $key
            value = $value
            line = $lineNumber
        })
    }
    return @($entries.ToArray())
}

function Get-SafeFilesRecursive {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string[]]$ExcludedTopLevelNames = @()
    )

    $fullRoot = Get-FullPath -Path $Root
    if (-not [System.IO.Directory]::Exists($fullRoot)) {
        return @()
    }
    Assert-NoReparsePoint -Path $fullRoot

    $excluded = @{}
    foreach ($name in $ExcludedTopLevelNames) {
        $excluded[$name.ToLowerInvariant()] = $true
    }

    $files = New-Object System.Collections.ArrayList
    $directories = New-Object System.Collections.Stack
    $directories.Push($fullRoot)
    while ($directories.Count -gt 0) {
        $directory = [string]$directories.Pop()
        foreach ($childDirectory in @(Sort-StringsOrdinal -Values @([System.IO.Directory]::GetDirectories($directory)))) {
            $relative = Get-RelativePathStrict -Root $fullRoot -Path $childDirectory
            $topLevel = ($relative -split '/')[0].ToLowerInvariant()
            if ($excluded.ContainsKey($topLevel)) {
                continue
            }
            $attributes = [System.IO.File]::GetAttributes($childDirectory)
            if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse points are not allowed below '$fullRoot': '$childDirectory'."
            }
            $directories.Push($childDirectory)
        }
        foreach ($file in @(Sort-StringsOrdinal -Values @([System.IO.Directory]::GetFiles($directory)))) {
            $relative = Get-RelativePathStrict -Root $fullRoot -Path $file
            $topLevel = ($relative -split '/')[0].ToLowerInvariant()
            if ($excluded.ContainsKey($topLevel)) {
                continue
            }
            $attributes = [System.IO.File]::GetAttributes($file)
            if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse points are not allowed below '$fullRoot': '$file'."
            }
            [void]$files.Add($file)
        }
    }
    return @(Sort-StringsOrdinal -Values @($files.ToArray()))
}

function Get-InputListHeaders {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ResolvedRepositoryRoot
    )

    if (-not [System.IO.File]::Exists($Path)) {
        return @()
    }
    $basePath = [System.IO.Path]::GetDirectoryName($Path)
    $headers = @{}
    foreach ($rawItem in ([System.IO.File]::ReadAllText($Path) -split ';')) {
        $item = $rawItem.Trim()
        if ($item.Length -ge 2 -and
            (($item.StartsWith('"') -and $item.EndsWith('"')) -or
             ($item.StartsWith("'") -and $item.EndsWith("'")))) {
            $item = $item.Substring(1, $item.Length - 2)
        }
        if ([string]::IsNullOrWhiteSpace($item)) {
            continue
        }

        if ([System.IO.Path]::IsPathRooted($item)) {
            $headerPath = Get-FullPath -Path $item
        }
        else {
            $headerPath = Get-FullPath -Path $item -BasePath $basePath
            if (-not [System.IO.File]::Exists($headerPath)) {
                $repositoryCandidate = Get-FullPath -Path $item -BasePath $ResolvedRepositoryRoot
                if ([System.IO.File]::Exists($repositoryCandidate)) {
                    $headerPath = $repositoryCandidate
                }
            }
        }
        $headers[$headerPath] = $headerPath
    }
    return @(Sort-StringsOrdinal -Values @($headers.Values))
}

function Resolve-TrackedPath {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$ResolvedContextFile,
        [Parameter(Mandatory = $true)][string]$ResolvedRepositoryRoot
    )

    if ([System.IO.Path]::IsPathRooted($Value)) {
        return Get-FullPath -Path $Value
    }

    $contextBase = [System.IO.Path]::GetDirectoryName($ResolvedContextFile)
    $contextCandidate = Get-FullPath -Path $Value -BasePath $contextBase
    if ([System.IO.File]::Exists($contextCandidate) -or
        [System.IO.Directory]::Exists($contextCandidate)) {
        return $contextCandidate
    }
    return Get-FullPath -Path $Value -BasePath $ResolvedRepositoryRoot
}

function Add-InputRecord {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.ArrayList]$Records,
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$DisplayPath
    )

    [void]$Records.Add((Get-FileHashRecord -Kind $Kind -Path $Path -DisplayPath $DisplayPath))
}

function Get-CodeGenFingerprint {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedRepositoryRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedParserExecutable,
        [Parameter(Mandatory = $true)][string]$ResolvedInputList,
        [Parameter(Mandatory = $true)][string]$ResolvedContextFile,
        [Parameter(Mandatory = $true)][string]$ResolvedTemplateRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedModule,
        [string[]]$IncludedFiles = @()
    )

    $records = New-Object System.Collections.ArrayList
    $issues = New-Object System.Collections.ArrayList

    Add-InputRecord -Records $records -Kind 'driver' -Path $PSCommandPath -DisplayPath 'codegen-driver'
    Add-InputRecord -Records $records -Kind 'input-list' -Path $ResolvedInputList -DisplayPath 'header-list'
    if (-not [System.IO.File]::Exists($ResolvedInputList)) {
        [void]$issues.Add('input-list-missing')
    }

    $headers = @(Get-InputListHeaders -Path $ResolvedInputList -ResolvedRepositoryRoot $ResolvedRepositoryRoot)
    foreach ($header in $headers) {
        $display = ConvertTo-LogicalPath -Path $header
        if (Test-PathContained -Root $ResolvedRepositoryRoot -Path $header -AllowRoot) {
            $display = Get-RelativePathStrict -Root $ResolvedRepositoryRoot -Path $header
        }
        elseif (Test-PathContained -Root $ResolvedProjectRoot -Path $header -AllowRoot) {
            $display = 'project/' + (Get-RelativePathStrict -Root $ResolvedProjectRoot -Path $header)
        }
        Add-InputRecord -Records $records -Kind 'header' -Path $header -DisplayPath $display
        if (-not [System.IO.File]::Exists($header)) {
            [void]$issues.Add("header-missing:$display")
        }
    }

    Add-InputRecord -Records $records -Kind 'parser' -Path $ResolvedParserExecutable -DisplayPath 'parser-executable'
    if (-not [System.IO.File]::Exists($ResolvedParserExecutable)) {
        [void]$issues.Add('parser-missing')
    }
    $libClangPath = Join-Path ([System.IO.Path]::GetDirectoryName($ResolvedParserExecutable)) 'libclang.dll'
    if ([System.IO.File]::Exists($libClangPath)) {
        Add-InputRecord -Records $records -Kind 'parser-runtime' -Path $libClangPath -DisplayPath 'libclang.dll'
    }

    Add-InputRecord -Records $records -Kind 'context' -Path $ResolvedContextFile -DisplayPath 'compile-context'
    if (-not [System.IO.File]::Exists($ResolvedContextFile)) {
        [void]$issues.Add('context-missing')
        $contextEntries = @()
    }
    else {
        $contextEntries = @(Read-ContextProtocol -Path $ResolvedContextFile)
    }

    $templateCount = 0
    if (-not [System.IO.Directory]::Exists($ResolvedTemplateRoot)) {
        [void]$issues.Add('template-root-missing')
    }
    else {
        foreach ($template in @(Get-SafeFilesRecursive -Root $ResolvedTemplateRoot)) {
            $templateCount++
            $relativeTemplate = Get-RelativePathStrict -Root $ResolvedTemplateRoot -Path $template
            Add-InputRecord -Records $records -Kind 'template' -Path $template -DisplayPath $relativeTemplate
        }
        if ($templateCount -eq 0) {
            [void]$issues.Add('template-root-empty')
        }
    }

    $trackedPaths = @{}
    foreach ($entry in @($contextEntries | Where-Object { $_.key -eq 'track' })) {
        $trackedPath = Resolve-TrackedPath -Value $entry.value -ResolvedContextFile $ResolvedContextFile -ResolvedRepositoryRoot $ResolvedRepositoryRoot
        $trackedPaths[$trackedPath] = $trackedPath
    }
    foreach ($trackedPath in @(Sort-StringsOrdinal -Values @($trackedPaths.Values))) {
        $displayTrack = ConvertTo-LogicalPath -Path $trackedPath
        if ([System.IO.File]::Exists($trackedPath)) {
            Add-InputRecord -Records $records -Kind 'track' -Path $trackedPath -DisplayPath $displayTrack
            continue
        }
        if ([System.IO.Directory]::Exists($trackedPath)) {
            $trackedFileRecords = New-Object System.Collections.ArrayList
            foreach ($trackedFile in @(Get-SafeFilesRecursive -Root $trackedPath)) {
                $relativeTrackedFile = Get-RelativePathStrict -Root $trackedPath -Path $trackedFile
                $record = Get-FileHashRecord -Kind 'track-file' -Path $trackedFile -DisplayPath ($displayTrack + '/' + $relativeTrackedFile)
                [void]$records.Add($record)
                [void]$trackedFileRecords.Add(($relativeTrackedFile + '=' + $record.hash))
            }
            $aggregate = Get-Sha256Text -Text (((Sort-StringsOrdinal -Values @($trackedFileRecords.ToArray())) -join "`n") + "`n")
            [void]$records.Add([pscustomobject][ordered]@{
                kind = 'track-directory'
                path = ConvertTo-LogicalPath -Path $trackedPath
                displayPath = $displayTrack
                exists = $true
                size = [int64]$trackedFileRecords.Count
                hash = $aggregate
            })
            continue
        }

        Add-InputRecord -Records $records -Kind 'track' -Path $trackedPath -DisplayPath $displayTrack
        [void]$issues.Add("track-missing:$displayTrack")
    }

    foreach ($includedFile in @(Sort-StringsOrdinal -Values @($IncludedFiles) -Unique)) {
        Add-InputRecord -Records $records -Kind 'include' -Path $includedFile -DisplayPath (ConvertTo-LogicalPath -Path $includedFile)
    }

    $sortedRecords = @(Sort-ObjectsOrdinal -Values @($records.ToArray()) -KeySelector {
        param($record)
        ([string]$record.kind) + [char]0 + ([string]$record.path)
    })
    $identityLines = New-Object System.Collections.ArrayList
    foreach ($line in @(
        "driverVersion=$($script:DriverVersion)",
        "schemaVersion=$($script:ManifestSchemaVersion)",
        'repositoryRoot=' + (ConvertTo-LogicalPath -Path $ResolvedRepositoryRoot),
        'projectRoot=' + (ConvertTo-LogicalPath -Path $ResolvedProjectRoot),
        'parserExecutable=' + (ConvertTo-LogicalPath -Path $ResolvedParserExecutable),
        'inputList=' + (ConvertTo-LogicalPath -Path $ResolvedInputList),
        'contextFile=' + (ConvertTo-LogicalPath -Path $ResolvedContextFile),
        'templateRoot=' + (ConvertTo-LogicalPath -Path $ResolvedTemplateRoot),
        "module=$ResolvedModule")) {
        [void]$identityLines.Add($line)
    }
    foreach ($entry in $contextEntries) {
        [void]$identityLines.Add(('context.' + $entry.key + '=' + $entry.value))
    }
    $configIdentity = Get-Sha256Text -Text ((@($identityLines.ToArray()) -join "`n") + "`n")

    return [pscustomobject][ordered]@{
        configIdentity = $configIdentity
        inputs = [object[]]$sortedRecords
        issues = [object[]]@(Sort-StringsOrdinal -Values @($issues.ToArray()) -Unique)
        contextEntries = [object[]]$contextEntries
    }
}

function Get-InputRecordKey {
    param([Parameter(Mandatory = $true)]$Record)

    return ([string]$Record.kind).ToLowerInvariant() + '|' + ([string]$Record.path).ToLowerInvariant()
}

function Get-InputDirtyReasons {
    param(
        [object[]]$PreviousInputs,
        [object[]]$CurrentInputs
    )

    $previousByKey = @{}
    foreach ($record in @($PreviousInputs)) {
        $key = Get-InputRecordKey -Record $record
        if ($previousByKey.ContainsKey($key)) {
            throw "Manifest contains duplicate input '$key'."
        }
        $previousByKey[$key] = $record
    }

    $currentByKey = @{}
    foreach ($record in @($CurrentInputs)) {
        $key = Get-InputRecordKey -Record $record
        $currentByKey[$key] = $record
    }

    $reasons = New-Object System.Collections.ArrayList
    $allKeys = @(Sort-StringsOrdinal -Values @(@($previousByKey.Keys) + @($currentByKey.Keys)) -Unique)
    foreach ($key in $allKeys) {
        $previous = $previousByKey[$key]
        $current = $currentByKey[$key]
        if ($null -eq $previous) {
            $label = [string]$current.displayPath
            if (-not $current.exists) {
                [void]$reasons.Add("$($current.kind)-missing:$label")
            }
            else {
                [void]$reasons.Add("$($current.kind)-added:$label")
            }
            continue
        }
        if ($null -eq $current) {
            [void]$reasons.Add("$($previous.kind)-removed:$($previous.displayPath)")
            continue
        }
        if (-not [string]::Equals([string]$previous.hash, [string]$current.hash, [System.StringComparison]::Ordinal) -or
            [bool]$previous.exists -ne [bool]$current.exists) {
            if (-not $current.exists) {
                [void]$reasons.Add("$($current.kind)-missing:$($current.displayPath)")
            }
            else {
                [void]$reasons.Add("$($current.kind)-changed:$($current.displayPath)")
            }
        }
    }
    return @(Sort-StringsOrdinal -Values @($reasons.ToArray()) -Unique)
}

function Get-ObjectProperty {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$Required
    )

    if ($null -eq $Object) {
        if ($Required) {
            throw "Required property '$Name' is missing."
        }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        if ($Required) {
            throw "Required property '$Name' is missing."
        }
        return $null
    }
    return $property.Value
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    try {
        $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
        if ([string]::IsNullOrWhiteSpace($text)) {
            throw 'file is empty'
        }
        return $text | ConvertFrom-Json
    }
    catch {
        throw "Invalid JSON file '$Path': $($_.Exception.Message)"
    }
}

function ConvertTo-CanonicalOutputPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $resolved = Resolve-RelativeOutputPath -Root $Root -RelativePath $Path
    return Get-RelativePathStrict -Root $Root -Path $resolved
}

function ConvertTo-CanonicalTypeRecord {
    param(
        [Parameter(Mandatory = $true)]$Type,
        [Parameter(Mandatory = $true)][string]$StagingRoot,
        [Parameter(Mandatory = $true)]$KnownOutputs
    )

    $identity = [string](Get-ObjectProperty -Object $Type -Name 'identity' -Required)
    $source = [string](Get-ObjectProperty -Object $Type -Name 'source' -Required)
    if ([string]::IsNullOrWhiteSpace($identity)) {
        throw 'Parser index contains a type with an empty identity.'
    }
    if ([string]::IsNullOrWhiteSpace($source)) {
        throw "Parser index type '$identity' has an empty source."
    }

    $rawOutputs = @(Get-ObjectProperty -Object $Type -Name 'outputs' -Required)
    $outputs = New-Object System.Collections.ArrayList
    foreach ($rawOutput in $rawOutputs) {
        if ($rawOutput -isnot [string]) {
            throw "Parser index type '$identity' contains a non-string output."
        }
        $output = ConvertTo-CanonicalOutputPath -Root $StagingRoot -Path ([string]$rawOutput)
        if (-not $KnownOutputs.ContainsKey($output)) {
            throw "Parser index type '$identity' references undeclared output '$output'."
        }
        [void]$outputs.Add($output)
    }
    $outputs = @(Sort-StringsOrdinal -Values @($outputs.ToArray()) -Unique)

    $knownPropertyOrder = @(
        'identity', 'usr', 'qualifiedName', 'legacyName', 'source',
        'line', 'column', 'outputs', 'dependencies')
    $record = [ordered]@{}
    foreach ($name in $knownPropertyOrder) {
        $property = $Type.PSObject.Properties[$name]
        if ($null -eq $property) {
            continue
        }
        if ($name -eq 'outputs') {
            $record[$name] = [object[]]$outputs
            continue
        }
        if ($name -eq 'dependencies') {
            $dependencies = New-Object System.Collections.ArrayList
            foreach ($dependency in @($property.Value)) {
                [void]$dependencies.Add((ConvertTo-StableValue -Value $dependency))
            }
            $sortedDependencies = @(Sort-ObjectsOrdinal -Values @($dependencies.ToArray()) -KeySelector {
                param($dependency)
                ConvertTo-DeterministicJson -Value $dependency
            })
            $record[$name] = [object[]]$sortedDependencies
            continue
        }
        $record[$name] = ConvertTo-StableValue -Value $property.Value
    }
    foreach ($property in @(Sort-ObjectsOrdinal -Values @($Type.PSObject.Properties) -KeySelector {
        param($typeProperty)
        [string]$typeProperty.Name
    })) {
        if ($knownPropertyOrder -contains $property.Name) {
            continue
        }
        $record[$property.Name] = ConvertTo-StableValue -Value $property.Value
    }
    return [pscustomobject]$record
}

function Read-AndValidateParserIndex {
    param(
        [Parameter(Mandatory = $true)][string]$IndexPath,
        [Parameter(Mandatory = $true)][string]$StagingRoot
    )

    if (-not [System.IO.File]::Exists($IndexPath)) {
        throw "Parser did not write its required index: '$IndexPath'."
    }
    $index = Read-JsonFile -Path $IndexPath
    $schemaVersion = Get-ObjectProperty -Object $index -Name 'schemaVersion' -Required
    if ([int]$schemaVersion -ne $script:ExpectedIndexSchemaVersion) {
        throw "Unsupported parser index schema '$schemaVersion'; expected '$($script:ExpectedIndexSchemaVersion)'."
    }
    $generatorVersion = [string](Get-ObjectProperty -Object $index -Name 'generatorVersion' -Required)
    if ([string]::IsNullOrWhiteSpace($generatorVersion)) {
        throw 'Parser index generatorVersion is empty.'
    }

    $declaredOutputs = @{}
    foreach ($rawOutput in @(Get-ObjectProperty -Object $index -Name 'outputs' -Required)) {
        if ($rawOutput -isnot [string]) {
            throw 'Parser index outputs must be strings.'
        }
        $output = ConvertTo-CanonicalOutputPath -Root $StagingRoot -Path ([string]$rawOutput)
        if ($declaredOutputs.ContainsKey($output)) {
            throw "Parser index contains duplicate output '$output'."
        }
        $declaredOutputs[$output] = $true
    }

    $physicalOutputs = @{}
    foreach ($file in @(Get-SafeFilesRecursive -Root $StagingRoot -ExcludedTopLevelNames @('.he-codegen', '.he-codegen-index.json'))) {
        $relative = Get-RelativePathStrict -Root $StagingRoot -Path $file
        $physicalOutputs[$relative] = $file
    }
    foreach ($declaredOutput in @(Sort-StringsOrdinal -Values @($declaredOutputs.Keys))) {
        if (-not $physicalOutputs.ContainsKey($declaredOutput)) {
            throw "Parser declared output '$declaredOutput' but did not create it."
        }
    }
    foreach ($physicalOutput in @(Sort-StringsOrdinal -Values @($physicalOutputs.Keys))) {
        if (-not $declaredOutputs.ContainsKey($physicalOutput)) {
            throw "Parser created undeclared output '$physicalOutput'."
        }
    }

    $types = New-Object System.Collections.ArrayList
    $typeIdentities = @{}
    foreach ($type in @(Get-ObjectProperty -Object $index -Name 'types' -Required)) {
        $canonicalType = ConvertTo-CanonicalTypeRecord -Type $type -StagingRoot $StagingRoot -KnownOutputs $declaredOutputs
        $identity = [string]$canonicalType.identity
        if ($typeIdentities.ContainsKey($identity)) {
            throw "Parser index contains duplicate type identity '$identity'."
        }
        $typeIdentities[$identity] = $true
        [void]$types.Add($canonicalType)
    }
    $sortedTypes = @(Sort-ObjectsOrdinal -Values @($types.ToArray()) -KeySelector {
        param($type)
        ([string]$type.identity) + [char]0 + ([string]$type.source)
    })

    $outputRecords = New-Object System.Collections.ArrayList
    foreach ($relative in @(Sort-StringsOrdinal -Values @($physicalOutputs.Keys))) {
        $fileRecord = Get-FileHashRecord -Kind 'output' -Path $physicalOutputs[$relative] -DisplayPath $relative
        [void]$outputRecords.Add([pscustomobject][ordered]@{
            path = $relative
            size = [int64]$fileRecord.size
            hash = [string]$fileRecord.hash
        })
    }

    $includedFiles = @()
    $rawIncludes = Get-ObjectProperty -Object $index -Name 'includeDependencies'
    if ($null -ne $rawIncludes) {
        foreach ($dependency in @($rawIncludes)) {
            if ($dependency -isnot [string] -or ![IO.Path]::IsPathRooted($dependency)) {
                throw 'Parser include dependency must be an absolute file path.'
            }
            $includedFiles += Get-FullPath -Path $dependency
        }
    }
    return [pscustomobject][ordered]@{
        includeDependencies = [object[]]@(Sort-StringsOrdinal -Values @($includedFiles) -Unique)
        schemaVersion = [int]$schemaVersion
        generatorVersion = $generatorVersion
        types = [object[]]$sortedTypes
        outputs = [object[]]$outputRecords.ToArray()
    }
}

function New-ManifestObject {
    param(
        [Parameter(Mandatory = $true)]$Fingerprint,
        [Parameter(Mandatory = $true)]$ParserIndex,
        [Parameter(Mandatory = $true)][string]$ResolvedRepositoryRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedParserExecutable,
        [Parameter(Mandatory = $true)][string]$ResolvedInputList,
        [Parameter(Mandatory = $true)][string]$ResolvedContextFile,
        [Parameter(Mandatory = $true)][string]$ResolvedTemplateRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedModule
    )

    return [pscustomobject][ordered]@{
        schemaVersion = $script:ManifestSchemaVersion
        driverVersion = $script:DriverVersion
        generatorVersion = [string]$ParserIndex.generatorVersion
        configIdentity = [string]$Fingerprint.configIdentity
        configuration = [pscustomobject][ordered]@{
            module = $ResolvedModule
            repositoryRoot = ConvertTo-LogicalPath -Path $ResolvedRepositoryRoot
            projectRoot = ConvertTo-LogicalPath -Path $ResolvedProjectRoot
            outputRoot = ConvertTo-LogicalPath -Path $ResolvedOutputRoot
            parserExecutable = ConvertTo-LogicalPath -Path $ResolvedParserExecutable
            inputList = ConvertTo-LogicalPath -Path $ResolvedInputList
            contextFile = ConvertTo-LogicalPath -Path $ResolvedContextFile
            templateRoot = ConvertTo-LogicalPath -Path $ResolvedTemplateRoot
            context = [object[]]$Fingerprint.contextEntries
        }
        inputs = [object[]]$Fingerprint.inputs
        types = [object[]]$ParserIndex.types
        outputs = [object[]]$ParserIndex.outputs
    }
}

function Read-ManifestState {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot
    )

    if (-not [System.IO.File]::Exists($ManifestPath)) {
        return [pscustomobject]@{ exists = $false; valid = $false; manifest = $null; error = $null }
    }

    try {
        $manifest = Read-JsonFile -Path $ManifestPath
        $manifestSchemaVersion = [int](Get-ObjectProperty -Object $manifest -Name 'schemaVersion' -Required)
        if ($manifestSchemaVersion -ne $script:ManifestSchemaVersion) {
            throw "Unsupported manifest schema '$manifestSchemaVersion'; expected '$($script:ManifestSchemaVersion)'."
        }
        if ([string]::IsNullOrWhiteSpace([string](Get-ObjectProperty -Object $manifest -Name 'driverVersion' -Required))) {
            throw 'Manifest driverVersion is empty.'
        }
        if ([string]::IsNullOrWhiteSpace([string](Get-ObjectProperty -Object $manifest -Name 'generatorVersion' -Required))) {
            throw 'Manifest generatorVersion is empty.'
        }
        $configIdentity = [string](Get-ObjectProperty -Object $manifest -Name 'configIdentity' -Required)
        if ($configIdentity -notmatch '^[0-9a-f]{64}$') {
            throw 'Manifest configIdentity is not a SHA-256 value.'
        }
        $configuration = Get-ObjectProperty -Object $manifest -Name 'configuration' -Required
        if ([string]::IsNullOrWhiteSpace([string](Get-ObjectProperty -Object $configuration -Name 'outputRoot' -Required))) {
            throw 'Manifest configuration.outputRoot is empty.'
        }
        $inputs = @(Get-ObjectProperty -Object $manifest -Name 'inputs' -Required)
        $inputKeys = @{}
        foreach ($input in $inputs) {
            $kind = [string](Get-ObjectProperty -Object $input -Name 'kind' -Required)
            $path = [string](Get-ObjectProperty -Object $input -Name 'path' -Required)
            [void](Get-ObjectProperty -Object $input -Name 'displayPath' -Required)
            $exists = [bool](Get-ObjectProperty -Object $input -Name 'exists' -Required)
            [void](Get-ObjectProperty -Object $input -Name 'size' -Required)
            $hash = Get-ObjectProperty -Object $input -Name 'hash' -Required
            if ([string]::IsNullOrWhiteSpace($kind) -or [string]::IsNullOrWhiteSpace($path)) {
                throw 'Manifest input kind/path is empty.'
            }
            if ($exists -and [string]$hash -notmatch '^[0-9a-f]{64}$') {
                throw "Manifest input has an invalid SHA-256 hash: '$kind|$path'."
            }
            $inputKey = $kind.ToLowerInvariant() + '|' + $path.ToLowerInvariant()
            if ($inputKeys.ContainsKey($inputKey)) {
                throw "Manifest contains duplicate input '$inputKey'."
            }
            $inputKeys[$inputKey] = $true
        }
        $types = @(Get-ObjectProperty -Object $manifest -Name 'types' -Required)
        $outputs = @(Get-ObjectProperty -Object $manifest -Name 'outputs' -Required)
        $outputPaths = @{}
        foreach ($output in $outputs) {
            $path = [string](Get-ObjectProperty -Object $output -Name 'path' -Required)
            $hash = [string](Get-ObjectProperty -Object $output -Name 'hash' -Required)
            $size = [int64](Get-ObjectProperty -Object $output -Name 'size' -Required)
            $canonicalPath = ConvertTo-CanonicalOutputPath -Root $ResolvedOutputRoot -Path $path
            if ($canonicalPath -cne $path.Replace('\', '/')) {
                throw "Manifest output path is not canonical: '$path'."
            }
            if ($hash -notmatch '^[0-9a-f]{64}$') {
                throw "Manifest output has an invalid SHA-256 hash: '$path'."
            }
            if ($size -lt 0) {
                throw "Manifest output has a negative size: '$path'."
            }
            if ($outputPaths.ContainsKey($canonicalPath)) {
                throw "Manifest contains duplicate output '$canonicalPath'."
            }
            $outputPaths[$canonicalPath] = $true
        }
        $typeIdentities = @{}
        foreach ($type in $types) {
            $identity = [string](Get-ObjectProperty -Object $type -Name 'identity' -Required)
            $source = [string](Get-ObjectProperty -Object $type -Name 'source' -Required)
            $typeOutputs = @(Get-ObjectProperty -Object $type -Name 'outputs' -Required)
            if ([string]::IsNullOrWhiteSpace($identity) -or [string]::IsNullOrWhiteSpace($source)) {
                throw 'Manifest type identity/source is empty.'
            }
            if ($typeIdentities.ContainsKey($identity)) {
                throw "Manifest contains duplicate type identity '$identity'."
            }
            $typeIdentities[$identity] = $true
            if ($typeOutputs.Count -eq 0) {
                throw "Manifest type '$identity' has no owned outputs."
            }
            foreach ($typeOutput in $typeOutputs) {
                $canonicalTypeOutput = ConvertTo-CanonicalOutputPath -Root $ResolvedOutputRoot -Path ([string]$typeOutput)
                if (-not $outputPaths.ContainsKey($canonicalTypeOutput)) {
                    throw "Manifest type '$identity' references unknown output '$typeOutput'."
                }
            }
        }
        return [pscustomobject]@{ exists = $true; valid = $true; manifest = $manifest; error = $null }
    }
    catch {
        return [pscustomobject]@{ exists = $true; valid = $false; manifest = $null; error = $_.Exception.Message }
    }
}

function Get-CodeGenStatus {
    param(
        [Parameter(Mandatory = $true)]$ManifestState,
        [Parameter(Mandatory = $true)]$Fingerprint,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot
    )

    $reasons = New-Object System.Collections.ArrayList
    foreach ($issue in @($Fingerprint.issues)) {
        [void]$reasons.Add([string]$issue)
    }
    $matchingOutputs = 0

    if (-not $ManifestState.exists) {
        [void]$reasons.Add('manifest-missing')
    }
    elseif (-not $ManifestState.valid) {
        [void]$reasons.Add('manifest-invalid')
    }
    else {
        $manifest = $ManifestState.manifest
        if ([int]$manifest.schemaVersion -ne $script:ManifestSchemaVersion) {
            [void]$reasons.Add('manifest-schema-changed')
        }
        if ([string]$manifest.driverVersion -cne $script:DriverVersion) {
            [void]$reasons.Add('driver-version-changed')
        }
        if ([string]$manifest.configIdentity -cne [string]$Fingerprint.configIdentity) {
            [void]$reasons.Add('config-identity-changed')
        }
        $manifestOutputRoot = [string](Get-ObjectProperty -Object $manifest.configuration -Name 'outputRoot')
        if (-not [string]::Equals(
            $manifestOutputRoot,
            (ConvertTo-LogicalPath -Path $ResolvedOutputRoot),
            $script:PathComparison)) {
            [void]$reasons.Add('output-root-changed')
        }
        foreach ($reason in @(Get-InputDirtyReasons -PreviousInputs @($manifest.inputs) -CurrentInputs @($Fingerprint.inputs))) {
            [void]$reasons.Add($reason)
        }

        foreach ($output in @($manifest.outputs)) {
            $relative = [string]$output.path
            $path = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath $relative
            Assert-NoReparsePoint -Path $path
            if (-not [System.IO.File]::Exists($path)) {
                [void]$reasons.Add("output-missing:$relative")
                continue
            }
            $fileInfo = New-Object System.IO.FileInfo($path)
            if ([int64]$fileInfo.Length -ne [int64]$output.size) {
                [void]$reasons.Add("output-changed:$relative")
                continue
            }
            if ((Get-FileSha256 -Path $path) -cne [string]$output.hash) {
                [void]$reasons.Add("output-changed:$relative")
                continue
            }
            $matchingOutputs++
        }
    }

    $dirtyReasons = @(Sort-StringsOrdinal -Values @($reasons.ToArray()) -Unique)
    return [pscustomobject][ordered]@{
        status = $(if ($dirtyReasons.Count -eq 0) { 'clean' } else { 'dirty' })
        dirtyReasons = [object[]]$dirtyReasons
        unchanged = [int]$matchingOutputs
    }
}

function Test-FileBytesEqual {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][byte[]]$Bytes
    )

    if (-not [System.IO.File]::Exists($Path)) {
        return $false
    }
    $fileInfo = New-Object System.IO.FileInfo($Path)
    if ($fileInfo.Length -ne $Bytes.Length) {
        return $false
    }
    return (Get-FileSha256 -Path $Path) -ceq (Get-Sha256Bytes -Bytes $Bytes)
}

function Invoke-TestOnlyPublishFault {
    param(
        [ValidateSet('write', 'flush', 'rename')]
        [string]$Stage
    )

    if (-not $script:TestOnlyForwardPublish -or
        -not $script:TestOnlyPublishFaultArmed -or
        $script:TestOnlyPublishFault -cne $Stage) {
        return
    }

    $script:TestOnlyPublishFaultStageCount++
    # Let the first forward file publish successfully so every injected fault
    # exercises rollback from a genuinely mixed in-progress tree.
    if ($script:TestOnlyPublishFaultStageCount -lt 2) {
        return
    }

    $script:TestOnlyPublishFaultArmed = $false
    throw "Injected test-only codegen publish $Stage failure."
}

function Write-NewFileVerified {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][byte[]]$Bytes
    )

    $directory = [System.IO.Path]::GetDirectoryName($Path)
    if (-not [System.IO.Directory]::Exists($directory)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }
    Assert-NoReparsePoint -Path $directory

    $stream = New-Object System.IO.FileStream(
        $Path,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None,
        65536,
        [System.IO.FileOptions]::WriteThrough)
    try {
        Invoke-TestOnlyPublishFault -Stage 'write'
        $stream.Write($Bytes, 0, $Bytes.Length)
        Invoke-TestOnlyPublishFault -Stage 'flush'
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
    $expectedHash = Get-Sha256Bytes -Bytes $Bytes
    if ((Get-FileSha256 -Path $Path) -cne $expectedHash) {
        throw "File verification failed after write: '$Path'."
    }
}

function Copy-NewFileVerified {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ExpectedHash
    )

    $directory = [System.IO.Path]::GetDirectoryName($Destination)
    if (-not [System.IO.Directory]::Exists($directory)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }
    Assert-NoReparsePoint -Path $directory

    $input = New-Object System.IO.FileStream(
        $Source,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
    $output = New-Object System.IO.FileStream(
        $Destination,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None,
        65536,
        [System.IO.FileOptions]::WriteThrough)
    try {
        Invoke-TestOnlyPublishFault -Stage 'write'
        $input.CopyTo($output)
        Invoke-TestOnlyPublishFault -Stage 'flush'
        $output.Flush($true)
    }
    finally {
        $output.Dispose()
        $input.Dispose()
    }
    if ((Get-FileSha256 -Path $Destination) -cne $ExpectedHash) {
        throw "File verification failed after copy: '$Destination'."
    }
}

function Replace-FileAtomically {
    param(
        [Parameter(Mandatory = $true)][string]$TemporaryPath,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ExpectedHash
    )

    Assert-NoReparsePoint -Path ([System.IO.Path]::GetDirectoryName($Destination))
    Invoke-TestOnlyPublishFault -Stage 'rename'
    if ([System.IO.Directory]::Exists($Destination)) {
        throw "Cannot publish a generated file over a directory: '$Destination'."
    }
    if ([System.IO.File]::Exists($Destination)) {
        Assert-NoReparsePoint -Path $Destination
        $destinationKey = (Get-Sha256Text -Text ((ConvertTo-LogicalPath -Path $Destination).ToLowerInvariant())).Substring(0, 8)
        $replaceBackup = Join-Path ([System.IO.Path]::GetDirectoryName($Destination)) ('.hcg-r-' + $destinationKey + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.bak')
        try {
            [System.IO.File]::Replace($TemporaryPath, $Destination, $replaceBackup, $true)
        }
        finally {
            if ([System.IO.File]::Exists($replaceBackup)) {
                [System.IO.File]::Delete($replaceBackup)
            }
        }
    }
    else {
        [System.IO.File]::Move($TemporaryPath, $Destination)
    }
    if (-not [System.IO.File]::Exists($Destination) -or
        (Get-FileSha256 -Path $Destination) -cne $ExpectedHash) {
        throw "Published file verification failed: '$Destination'."
    }
}

function Publish-BytesAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][byte[]]$Bytes
    )

    if (Test-FileBytesEqual -Path $Destination -Bytes $Bytes) {
        return $false
    }
    $directory = [System.IO.Path]::GetDirectoryName($Destination)
    if (-not [System.IO.Directory]::Exists($directory)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }
    $destinationKey = (Get-Sha256Text -Text ((ConvertTo-LogicalPath -Path $Destination).ToLowerInvariant())).Substring(0, 8)
    $temporaryPath = Join-Path $directory ('.hcg-w-' + $destinationKey + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.tmp')
    try {
        Write-NewFileVerified -Path $temporaryPath -Bytes $Bytes
        $hash = Get-Sha256Bytes -Bytes $Bytes
        Replace-FileAtomically -TemporaryPath $temporaryPath -Destination $Destination -ExpectedHash $hash
        return $true
    }
    finally {
        if ([System.IO.File]::Exists($temporaryPath)) {
            [System.IO.File]::Delete($temporaryPath)
        }
    }
}

function Publish-FileAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ExpectedHash
    )

    $directory = [System.IO.Path]::GetDirectoryName($Destination)
    if (-not [System.IO.Directory]::Exists($directory)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }
    $destinationKey = (Get-Sha256Text -Text ((ConvertTo-LogicalPath -Path $Destination).ToLowerInvariant())).Substring(0, 8)
    $temporaryPath = Join-Path $directory ('.hcg-w-' + $destinationKey + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.tmp')
    try {
        Copy-NewFileVerified -Source $Source -Destination $temporaryPath -ExpectedHash $ExpectedHash
        Replace-FileAtomically -TemporaryPath $temporaryPath -Destination $Destination -ExpectedHash $ExpectedHash
    }
    finally {
        if ([System.IO.File]::Exists($temporaryPath)) {
            [System.IO.File]::Delete($temporaryPath)
        }
    }
}

function Get-TransactionScope {
    param([Parameter(Mandatory = $true)][string]$ResolvedOutputRoot)

    $parent = [System.IO.Path]::GetDirectoryName($ResolvedOutputRoot)
    $identity = Get-Sha256Text -Text ((ConvertTo-LogicalPath -Path $ResolvedOutputRoot).ToLowerInvariant())
    $prefix = '.hcg-' + $identity.Substring(0, 12)
    return [pscustomobject][ordered]@{
        parent = $parent
        prefix = $prefix
        lockPath = Join-Path $parent ($prefix + '.lock')
        journalPath = Join-Path $parent ($prefix + '.journal.json')
    }
}

function Assert-SafeTransactionArtifact {
    param(
        [Parameter(Mandatory = $true)]$Scope,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$AllowedSuffixPrefixes
    )

    $fullPath = Get-FullPath -Path $Path
    $parent = [System.IO.Path]::GetDirectoryName($fullPath)
    if (-not [string]::Equals($parent, $Scope.parent, $script:PathComparison)) {
        throw "Transaction artifact is outside its sibling scope: '$fullPath'."
    }
    $leaf = [System.IO.Path]::GetFileName($fullPath)
    $allowed = $false
    foreach ($suffixPrefix in $AllowedSuffixPrefixes) {
        if ($leaf.StartsWith($Scope.prefix + $suffixPrefix, $script:PathComparison)) {
            $allowed = $true
            break
        }
    }
    if (-not $allowed) {
        throw "Transaction artifact has an unexpected name: '$fullPath'."
    }
    return $fullPath
}

function Remove-SafeTransactionDirectory {
    param(
        [Parameter(Mandatory = $true)]$Scope,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $fullPath = Assert-SafeTransactionArtifact -Scope $Scope -Path $Path -AllowedSuffixPrefixes @('.s-', '.b-')
    if (-not [System.IO.Directory]::Exists($fullPath)) {
        return
    }
    Assert-NoReparsePoint -Path $fullPath
    Remove-DirectoryTreeStrict -Path $fullPath
}

function Remove-DirectoryTreeStrict {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = Get-FullPath -Path $Path
    Assert-NoReparsePoint -Path $fullPath
    foreach ($file in @([System.IO.Directory]::GetFiles($fullPath))) {
        $attributes = [System.IO.File]::GetAttributes($file)
        if (($attributes -band [System.IO.FileAttributes]::ReadOnly) -ne 0) {
            [System.IO.File]::SetAttributes($file, ($attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly)))
        }
        [System.IO.File]::Delete($file)
    }
    foreach ($directory in @([System.IO.Directory]::GetDirectories($fullPath))) {
        $attributes = [System.IO.File]::GetAttributes($directory)
        if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            [System.IO.Directory]::Delete($directory, $false)
            continue
        }
        Remove-DirectoryTreeStrict -Path $directory
    }
    [System.IO.Directory]::Delete($fullPath, $false)
}

function Remove-DriverTemporaryFiles {
    param([Parameter(Mandatory = $true)][string]$Root)

    if (-not [System.IO.Directory]::Exists($Root)) {
        return
    }
    foreach ($file in @(Get-SafeFilesRecursive -Root $Root)) {
        $name = [System.IO.Path]::GetFileName($file)
        if ($name -cmatch '^\.hcg-w-[0-9a-f]{8}-[0-9a-f]{8}\.tmp$' -or
            $name -cmatch '^\.hcg-r-[0-9a-f]{8}-[0-9a-f]{8}\.bak$') {
            Assert-NoReparsePoint -Path $file
            [System.IO.File]::Delete($file)
        }
    }
}

function Remove-SafeTransactionFile {
    param(
        [Parameter(Mandatory = $true)]$Scope,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$AllowedSuffixPrefixes
    )

    $fullPath = Assert-SafeTransactionArtifact -Scope $Scope -Path $Path -AllowedSuffixPrefixes $AllowedSuffixPrefixes
    if ([System.IO.File]::Exists($fullPath)) {
        Assert-NoReparsePoint -Path $fullPath
        [System.IO.File]::Delete($fullPath)
    }
}

function Write-Journal {
    param(
        [Parameter(Mandatory = $true)]$Scope,
        [Parameter(Mandatory = $true)]$Journal
    )

    $bytes = $script:Utf8NoBom.GetBytes((ConvertTo-DeterministicJson -Value $Journal))
    [void](Publish-BytesAtomic -Destination $Scope.journalPath -Bytes $bytes)
}

function Remove-OwnedOutputFile {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $path = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath $RelativePath
    Assert-NoReparsePoint -Path $path
    if ([System.IO.Directory]::Exists($path)) {
        throw "Manifest-owned output is unexpectedly a directory: '$path'."
    }
    if ([System.IO.File]::Exists($path)) {
        [System.IO.File]::Delete($path)
    }
}

function Restore-Transaction {
    param(
        [Parameter(Mandatory = $true)]$Scope,
        [Parameter(Mandatory = $true)]$Journal,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    $backupRoot = Assert-SafeTransactionArtifact -Scope $Scope -Path ([string]$Journal.backupRoot) -AllowedSuffixPrefixes @('.b-')
    $stagingRoot = Assert-SafeTransactionArtifact -Scope $Scope -Path ([string]$Journal.stagingRoot) -AllowedSuffixPrefixes @('.s-')

    if ([string]$Journal.phase -eq 'committed') {
        Remove-SafeTransactionDirectory -Scope $Scope -Path $stagingRoot
        Remove-SafeTransactionDirectory -Scope $Scope -Path $backupRoot
        Remove-SafeTransactionFile -Scope $Scope -Path $Scope.journalPath -AllowedSuffixPrefixes @('.journal.json')
        return
    }

    if ([bool]$Journal.manifestChanged -and [System.IO.File]::Exists($ManifestPath)) {
        Assert-NoReparsePoint -Path $ManifestPath
        [System.IO.File]::Delete($ManifestPath)
    }

    foreach ($entry in @($Journal.changed)) {
        $relative = [string]$entry.path
        $destination = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath $relative
        if ([bool]$entry.hadOriginal) {
            $backup = Resolve-RelativeOutputPath -Root $backupRoot -RelativePath $relative
            if (-not [System.IO.File]::Exists($backup)) {
                throw "Rollback backup is missing: '$backup'."
            }
            Publish-FileAtomic -Source $backup -Destination $destination -ExpectedHash ([string]$entry.originalHash)
            if ($null -ne $entry.PSObject.Properties['originalLastWriteTimeUtcTicks']) {
                [System.IO.File]::SetLastWriteTimeUtc($destination, (New-Object System.DateTime([int64]$entry.originalLastWriteTimeUtcTicks, [System.DateTimeKind]::Utc)))
            }
        }
        else {
            Remove-OwnedOutputFile -ResolvedOutputRoot $ResolvedOutputRoot -RelativePath $relative
        }
    }
    foreach ($entry in @($Journal.deleted)) {
        $relative = [string]$entry.path
        $destination = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath $relative
        $backup = Resolve-RelativeOutputPath -Root $backupRoot -RelativePath $relative
        if (-not [System.IO.File]::Exists($backup)) {
            throw "Rollback backup is missing: '$backup'."
        }
        Publish-FileAtomic -Source $backup -Destination $destination -ExpectedHash ([string]$entry.originalHash)
        if ($null -ne $entry.PSObject.Properties['originalLastWriteTimeUtcTicks']) {
            [System.IO.File]::SetLastWriteTimeUtc($destination, (New-Object System.DateTime([int64]$entry.originalLastWriteTimeUtcTicks, [System.DateTimeKind]::Utc)))
        }
    }

    if ([bool]$Journal.manifestChanged -and [bool]$Journal.manifestHadOriginal) {
        $manifestBackup = Join-Path $backupRoot '.he-codegen\manifest.json'
        if (-not [System.IO.File]::Exists($manifestBackup)) {
            throw "Rollback manifest backup is missing: '$manifestBackup'."
        }
        Publish-FileAtomic -Source $manifestBackup -Destination $ManifestPath -ExpectedHash ([string]$Journal.manifestOriginalHash)
        if ($null -ne $Journal.PSObject.Properties['manifestOriginalLastWriteTimeUtcTicks']) {
            [System.IO.File]::SetLastWriteTimeUtc($ManifestPath, (New-Object System.DateTime([int64]$Journal.manifestOriginalLastWriteTimeUtcTicks, [System.DateTimeKind]::Utc)))
        }
    }

    Remove-SafeTransactionDirectory -Scope $Scope -Path $stagingRoot
    Remove-SafeTransactionDirectory -Scope $Scope -Path $backupRoot
    Remove-SafeTransactionFile -Scope $Scope -Path $Scope.journalPath -AllowedSuffixPrefixes @('.journal.json')
}

function Recover-CodeGenTransaction {
    param(
        [Parameter(Mandatory = $true)]$Scope,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    $hadJournal = [System.IO.File]::Exists($Scope.journalPath)
    if ($hadJournal) {
        Assert-NoReparsePoint -Path $Scope.journalPath
        $journal = Read-JsonFile -Path $Scope.journalPath
        $journalOutputRoot = [string](Get-ObjectProperty -Object $journal -Name 'outputRoot' -Required)
        if (-not [string]::Equals(
            $journalOutputRoot,
            (ConvertTo-LogicalPath -Path $ResolvedOutputRoot),
            $script:PathComparison)) {
            throw "Transaction journal belongs to a different output root: '$($Scope.journalPath)'."
        }
        foreach ($requiredName in @('phase', 'stagingRoot', 'backupRoot', 'changed', 'deleted', 'manifestChanged', 'manifestHadOriginal')) {
            [void](Get-ObjectProperty -Object $journal -Name $requiredName -Required)
        }
        if (@('prepared', 'publishing', 'committed') -notcontains [string]$journal.phase) {
            throw "Transaction journal has an invalid phase '$($journal.phase)'."
        }
        Restore-Transaction -Scope $Scope -Journal $journal -ResolvedOutputRoot $ResolvedOutputRoot -ManifestPath $ManifestPath
    }

    foreach ($directory in @([System.IO.Directory]::GetDirectories($Scope.parent, $Scope.prefix + '.s-*'))) {
        Remove-SafeTransactionDirectory -Scope $Scope -Path $directory
    }
    foreach ($directory in @([System.IO.Directory]::GetDirectories($Scope.parent, $Scope.prefix + '.b-*'))) {
        Remove-SafeTransactionDirectory -Scope $Scope -Path $directory
    }
    foreach ($file in @([System.IO.Directory]::GetFiles($Scope.parent, $Scope.prefix + '.journal.json.write-*'))) {
        Remove-SafeTransactionFile -Scope $Scope -Path $file -AllowedSuffixPrefixes @('.journal.json.write-')
    }
}

function Assert-FingerprintUsable {
    param([Parameter(Mandatory = $true)]$Fingerprint)

    if (@($Fingerprint.issues).Count -gt 0) {
        $firstIssues = @($Fingerprint.issues | Select-Object -First 8)
        throw ('Codegen inputs are incomplete: ' + ($firstIssues -join ', '))
    }
}

function Test-FingerprintsEqual {
    param(
        [Parameter(Mandatory = $true)]$Before,
        [Parameter(Mandatory = $true)]$After
    )

    if ([string]$Before.configIdentity -cne [string]$After.configIdentity) {
        return $false
    }
    $beforeJson = ConvertTo-DeterministicJson -Value ([object[]]$Before.inputs)
    $afterJson = ConvertTo-DeterministicJson -Value ([object[]]$After.inputs)
    return $beforeJson -ceq $afterJson
}

function Invoke-CodeGenParser {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedParserExecutable,
        [Parameter(Mandatory = $true)][string]$ResolvedInputList,
        [Parameter(Mandatory = $true)][string]$ResolvedContextFile,
        [Parameter(Mandatory = $true)][string]$ResolvedTemplateRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedModule,
        [Parameter(Mandatory = $true)][string]$StagingRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedLogPath
    )

    $controlRoot = Join-Path $StagingRoot '.he-codegen'
    [void][System.IO.Directory]::CreateDirectory($controlRoot)
    $inputHeader = Join-Path $controlRoot 'input-header.h'
    $indexPath = Join-Path $StagingRoot '.he-codegen-index.json'
    $parserArguments = @(
        '--input-list', $ResolvedInputList,
        '--input-header', $inputHeader,
        '--output-root', $StagingRoot,
        '--context', $ResolvedContextFile,
        '--templates', $ResolvedTemplateRoot,
        '--index', $indexPath,
        '--module', $ResolvedModule,
        '--quiet')

    $logDirectory = [System.IO.Path]::GetDirectoryName($ResolvedLogPath)
    if (-not [System.IO.Directory]::Exists($logDirectory)) {
        [void][System.IO.Directory]::CreateDirectory($logDirectory)
    }
    Assert-NoReparsePoint -Path $logDirectory

    $previousErrorPreference = $ErrorActionPreference
    $parserExitCode = -1
    try {
        $ErrorActionPreference = 'Continue'
        $global:LASTEXITCODE = 0
        & $ResolvedParserExecutable @parserArguments *> $ResolvedLogPath
        $parserExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorPreference
    }
    if ($parserExitCode -ne 0) {
        throw "HEngineParser failed with exit code $parserExitCode."
    }
    return Read-AndValidateParserIndex -IndexPath $indexPath -StagingRoot $StagingRoot
}

function New-PublishPlan {
    param(
        [Parameter(Mandatory = $true)]$ParserIndex,
        [Parameter(Mandatory = $true)][string]$StagingRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [Parameter(Mandatory = $true)]$ManifestState,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][byte[]]$ManifestBytes
    )

    if ($ManifestState.exists -and -not $ManifestState.valid) {
        throw "Existing codegen manifest is invalid; refusing to publish or delete owned files: $($ManifestState.error)"
    }

    $changed = New-Object System.Collections.ArrayList
    $unchanged = New-Object System.Collections.ArrayList
    $deleted = New-Object System.Collections.ArrayList
    $newOutputPaths = @{}

    foreach ($output in @($ParserIndex.outputs)) {
        $relative = [string]$output.path
        $newOutputPaths[$relative] = $true
        $source = Resolve-RelativeOutputPath -Root $StagingRoot -RelativePath $relative
        $destination = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath $relative
        Assert-NoReparsePoint -Path ([System.IO.Path]::GetDirectoryName($destination))
        if ([System.IO.Directory]::Exists($destination)) {
            throw "Generated output destination is a directory: '$destination'."
        }

        $hadOriginal = [System.IO.File]::Exists($destination)
        $originalHash = $null
        $originalLastWriteTimeUtcTicks = $null
        if ($hadOriginal) {
            Assert-NoReparsePoint -Path $destination
            $originalHash = Get-FileSha256 -Path $destination
            $originalLastWriteTimeUtcTicks = [System.IO.File]::GetLastWriteTimeUtc($destination).Ticks
            if ($originalHash -ceq [string]$output.hash) {
                [void]$unchanged.Add([pscustomobject][ordered]@{ path = $relative })
                continue
            }
        }
        [void]$changed.Add([pscustomobject][ordered]@{
            path = $relative
            source = $source
            hash = [string]$output.hash
            hadOriginal = [bool]$hadOriginal
            originalHash = $originalHash
            originalLastWriteTimeUtcTicks = $originalLastWriteTimeUtcTicks
        })
    }

    if ($ManifestState.valid) {
        foreach ($oldOutput in @($ManifestState.manifest.outputs)) {
            $relative = [string]$oldOutput.path
            if ($newOutputPaths.ContainsKey($relative)) {
                continue
            }
            $destination = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath $relative
            Assert-NoReparsePoint -Path $destination
            if ([System.IO.Directory]::Exists($destination)) {
                throw "Manifest-owned stale output is a directory: '$destination'."
            }
            if (-not [System.IO.File]::Exists($destination)) {
                continue
            }
            [void]$deleted.Add([pscustomobject][ordered]@{
                path = $relative
                originalHash = Get-FileSha256 -Path $destination
                originalLastWriteTimeUtcTicks = [System.IO.File]::GetLastWriteTimeUtc($destination).Ticks
            })
        }
    }

    $manifestChanged = -not (Test-FileBytesEqual -Path $ManifestPath -Bytes $ManifestBytes)
    $manifestHadOriginal = [System.IO.File]::Exists($ManifestPath)
    $manifestOriginalHash = $null
    $manifestOriginalLastWriteTimeUtcTicks = $null
    if ($manifestHadOriginal) {
        $manifestOriginalHash = Get-FileSha256 -Path $ManifestPath
        $manifestOriginalLastWriteTimeUtcTicks = [System.IO.File]::GetLastWriteTimeUtc($ManifestPath).Ticks
    }

    return [pscustomobject][ordered]@{
        changed = [object[]]$changed.ToArray()
        unchanged = [object[]]$unchanged.ToArray()
        deleted = [object[]]$deleted.ToArray()
        manifestChanged = [bool]$manifestChanged
        manifestHadOriginal = [bool]$manifestHadOriginal
        manifestOriginalHash = $manifestOriginalHash
        manifestOriginalLastWriteTimeUtcTicks = $manifestOriginalLastWriteTimeUtcTicks
        manifestBytes = $ManifestBytes
        expectedOutputs = [object[]]$ParserIndex.outputs
    }
}

function Backup-PublishPlan {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$BackupRoot
    )

    [void][System.IO.Directory]::CreateDirectory($BackupRoot)
    foreach ($entry in @($Plan.changed | Where-Object { $_.hadOriginal })) {
        $source = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath ([string]$entry.path)
        $destination = Resolve-RelativeOutputPath -Root $BackupRoot -RelativePath ([string]$entry.path)
        Copy-NewFileVerified -Source $source -Destination $destination -ExpectedHash ([string]$entry.originalHash)
    }
    foreach ($entry in @($Plan.deleted)) {
        $source = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath ([string]$entry.path)
        $destination = Resolve-RelativeOutputPath -Root $BackupRoot -RelativePath ([string]$entry.path)
        Copy-NewFileVerified -Source $source -Destination $destination -ExpectedHash ([string]$entry.originalHash)
    }
    if ($Plan.manifestChanged -and $Plan.manifestHadOriginal) {
        $manifestBackup = Join-Path $BackupRoot '.he-codegen\manifest.json'
        Copy-NewFileVerified -Source $ManifestPath -Destination $manifestBackup -ExpectedHash ([string]$Plan.manifestOriginalHash)
    }
}

function Invoke-PublishPlan {
    param(
        [Parameter(Mandatory = $true)]$Scope,
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$StagingRoot,
        [Parameter(Mandatory = $true)][string]$BackupRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [scriptblock]$PreCommitValidation
    )

    $mutationCount = @($Plan.changed).Count + @($Plan.deleted).Count
    if (-not $Plan.manifestChanged -and $mutationCount -eq 0) {
        Remove-SafeTransactionDirectory -Scope $Scope -Path $StagingRoot
        return
    }

    Backup-PublishPlan -Plan $Plan -ResolvedOutputRoot $ResolvedOutputRoot -ManifestPath $ManifestPath -BackupRoot $BackupRoot
    $journalChanged = @($Plan.changed | ForEach-Object {
        [pscustomobject][ordered]@{
            path = [string]$_.path
            hadOriginal = [bool]$_.hadOriginal
            originalHash = $_.originalHash
            originalLastWriteTimeUtcTicks = $_.originalLastWriteTimeUtcTicks
        }
    })
    $journalDeleted = @($Plan.deleted | ForEach-Object {
        [pscustomobject][ordered]@{
            path = [string]$_.path
            originalHash = [string]$_.originalHash
            originalLastWriteTimeUtcTicks = [int64]$_.originalLastWriteTimeUtcTicks
        }
    })
    $journal = [pscustomobject][ordered]@{
        schemaVersion = 1
        outputRoot = ConvertTo-LogicalPath -Path $ResolvedOutputRoot
        phase = 'prepared'
        stagingRoot = ConvertTo-LogicalPath -Path $StagingRoot
        backupRoot = ConvertTo-LogicalPath -Path $BackupRoot
        changed = [object[]]$journalChanged
        deleted = [object[]]$journalDeleted
        manifestChanged = [bool]$Plan.manifestChanged
        manifestHadOriginal = [bool]$Plan.manifestHadOriginal
        manifestOriginalHash = $Plan.manifestOriginalHash
        manifestOriginalLastWriteTimeUtcTicks = $Plan.manifestOriginalLastWriteTimeUtcTicks
    }
    Write-Journal -Scope $Scope -Journal $journal

    try {
        $journal.phase = 'publishing'
        Write-Journal -Scope $Scope -Journal $journal

        $script:TestOnlyPublishFaultStageCount = 0
        $script:TestOnlyPublishFaultArmed = -not [string]::IsNullOrWhiteSpace($script:TestOnlyPublishFault)
        $script:TestOnlyForwardPublish = $script:TestOnlyPublishFaultArmed

        if (-not [System.IO.Directory]::Exists($ResolvedOutputRoot)) {
            [void][System.IO.Directory]::CreateDirectory($ResolvedOutputRoot)
        }
        foreach ($entry in @($Plan.changed)) {
            $destination = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath ([string]$entry.path)
            Publish-FileAtomic -Source ([string]$entry.source) -Destination $destination -ExpectedHash ([string]$entry.hash)
        }
        foreach ($entry in @($Plan.deleted)) {
            Remove-OwnedOutputFile -ResolvedOutputRoot $ResolvedOutputRoot -RelativePath ([string]$entry.path)
        }

        if ($Plan.manifestChanged) {
            [void](Publish-BytesAtomic -Destination $ManifestPath -Bytes ([byte[]]$Plan.manifestBytes))
        }

        foreach ($expectedOutput in @($Plan.expectedOutputs)) {
            $expectedPath = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath ([string]$expectedOutput.path)
            if (-not [System.IO.File]::Exists($expectedPath) -or
                (Get-FileSha256 -Path $expectedPath) -cne [string]$expectedOutput.hash) {
                throw "Published output failed pre-commit verification: '$($expectedOutput.path)'."
            }
        }
        foreach ($deletedOutput in @($Plan.deleted)) {
            $deletedPath = Resolve-RelativeOutputPath -Root $ResolvedOutputRoot -RelativePath ([string]$deletedOutput.path)
            if ([System.IO.File]::Exists($deletedPath)) {
                throw "Stale output still exists during pre-commit verification: '$($deletedOutput.path)'."
            }
        }
        if (-not (Test-FileBytesEqual -Path $ManifestPath -Bytes ([byte[]]$Plan.manifestBytes))) {
            throw 'Published manifest failed pre-commit verification.'
        }
        if ($null -ne $PreCommitValidation) {
            & $PreCommitValidation
        }

        $script:TestOnlyForwardPublish = $false
        if ($script:TestOnlyPublishFaultArmed) {
            throw "Configured test-only publish fault '$($script:TestOnlyPublishFault)' was not reached."
        }

        $journal.phase = 'committed'
        Write-Journal -Scope $Scope -Journal $journal
        Restore-Transaction -Scope $Scope -Journal $journal -ResolvedOutputRoot $ResolvedOutputRoot -ManifestPath $ManifestPath
    }
    catch {
        $script:TestOnlyForwardPublish = $false
        $script:TestOnlyPublishFaultArmed = $false
        $publishError = $_
        try {
            if ([System.IO.File]::Exists($Scope.journalPath)) {
                $recoveryJournal = Read-JsonFile -Path $Scope.journalPath
                Restore-Transaction -Scope $Scope -Journal $recoveryJournal -ResolvedOutputRoot $ResolvedOutputRoot -ManifestPath $ManifestPath
            }
        }
        catch {
            throw "Codegen publish failed: $($publishError.Exception.Message) Rollback also failed: $($_.Exception.Message)"
        }
        throw $publishError
    }
}

function Invoke-CodeGenRun {
    param(
        [Parameter(Mandatory = $true)]$Scope,
        [Parameter(Mandatory = $true)]$Fingerprint,
        [Parameter(Mandatory = $true)]$ManifestState,
        [Parameter(Mandatory = $true)][string]$ResolvedRepositoryRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedParserExecutable,
        [Parameter(Mandatory = $true)][string]$ResolvedInputList,
        [Parameter(Mandatory = $true)][string]$ResolvedContextFile,
        [Parameter(Mandatory = $true)][string]$ResolvedTemplateRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedModule,
        [Parameter(Mandatory = $true)][string]$ResolvedLogPath,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    Assert-FingerprintUsable -Fingerprint $Fingerprint
    if ($ManifestState.exists -and -not $ManifestState.valid) {
        throw "Existing codegen manifest is invalid; refusing transactional publish: $($ManifestState.error)"
    }

    $transactionId = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $stagingRoot = Join-Path $Scope.parent ($Scope.prefix + '.s-' + $transactionId)
    $backupRoot = Join-Path $Scope.parent ($Scope.prefix + '.b-' + $transactionId)
    [void][System.IO.Directory]::CreateDirectory($stagingRoot)

    try {
        $includedFiles = @($Fingerprint.inputs | Where-Object {$_.kind -eq 'include'} | ForEach-Object {$_.path})
        for ($dependencyPass = 0; $dependencyPass -lt 2; $dependencyPass++) {
            $parserIndex = Invoke-CodeGenParser `
                -ResolvedParserExecutable $ResolvedParserExecutable `
                -ResolvedInputList $ResolvedInputList `
                -ResolvedContextFile $ResolvedContextFile `
                -ResolvedTemplateRoot $ResolvedTemplateRoot `
                -ResolvedModule $ResolvedModule `
                -StagingRoot $stagingRoot `
                -ResolvedLogPath $ResolvedLogPath

            $afterFingerprint = Get-CodeGenFingerprint `
                -ResolvedRepositoryRoot $ResolvedRepositoryRoot `
                -ResolvedProjectRoot $ResolvedProjectRoot `
                -ResolvedParserExecutable $ResolvedParserExecutable `
                -ResolvedInputList $ResolvedInputList `
                -ResolvedContextFile $ResolvedContextFile `
                -ResolvedTemplateRoot $ResolvedTemplateRoot `
                -ResolvedModule $ResolvedModule `
                -IncludedFiles $includedFiles
            Assert-FingerprintUsable -Fingerprint $afterFingerprint
            if (-not (Test-FingerprintsEqual -Before $Fingerprint -After $afterFingerprint)) {
                throw 'Codegen inputs changed while HEngineParser was running; retry with a stable input snapshot.'
            }

            $nextIncludes = @($parserIndex.includeDependencies)
            if ((@($includedFiles | Sort-Object) -join "`n") -eq (@($nextIncludes | Sort-Object) -join "`n")) { break }
            if ($dependencyPass -eq 1) { throw 'Parser include closure changed during verification; retry with stable inputs.' }
            $includedFiles = $nextIncludes
            $Fingerprint = Get-CodeGenFingerprint -ResolvedRepositoryRoot $ResolvedRepositoryRoot -ResolvedProjectRoot $ResolvedProjectRoot -ResolvedParserExecutable $ResolvedParserExecutable -ResolvedInputList $ResolvedInputList -ResolvedContextFile $ResolvedContextFile -ResolvedTemplateRoot $ResolvedTemplateRoot -ResolvedModule $ResolvedModule -IncludedFiles $includedFiles
        }

        $manifest = New-ManifestObject `
            -Fingerprint $afterFingerprint `
            -ParserIndex $parserIndex `
            -ResolvedRepositoryRoot $ResolvedRepositoryRoot `
            -ResolvedProjectRoot $ResolvedProjectRoot `
            -ResolvedOutputRoot $ResolvedOutputRoot `
            -ResolvedParserExecutable $ResolvedParserExecutable `
            -ResolvedInputList $ResolvedInputList `
            -ResolvedContextFile $ResolvedContextFile `
            -ResolvedTemplateRoot $ResolvedTemplateRoot `
            -ResolvedModule $ResolvedModule
        $manifestBytes = $script:Utf8NoBom.GetBytes((ConvertTo-DeterministicJson -Value $manifest))
        $plan = New-PublishPlan `
            -ParserIndex $parserIndex `
            -StagingRoot $stagingRoot `
            -ResolvedOutputRoot $ResolvedOutputRoot `
            -ManifestState $ManifestState `
            -ManifestPath $ManifestPath `
            -ManifestBytes $manifestBytes
        $preCommitValidation = {
            $commitFingerprint = Get-CodeGenFingerprint `
                -ResolvedRepositoryRoot $ResolvedRepositoryRoot `
                -ResolvedProjectRoot $ResolvedProjectRoot `
                -ResolvedParserExecutable $ResolvedParserExecutable `
                -ResolvedInputList $ResolvedInputList `
                -ResolvedContextFile $ResolvedContextFile `
                -ResolvedTemplateRoot $ResolvedTemplateRoot `
                -ResolvedModule $ResolvedModule `
                -IncludedFiles $includedFiles
            Assert-FingerprintUsable -Fingerprint $commitFingerprint
            if (-not (Test-FingerprintsEqual -Before $afterFingerprint -After $commitFingerprint)) {
                throw 'Codegen inputs changed during publish; rolling back to the previous valid tree.'
            }
        }.GetNewClosure()
        Invoke-PublishPlan `
            -Scope $Scope `
            -Plan $plan `
            -StagingRoot $stagingRoot `
            -BackupRoot $backupRoot `
            -ResolvedOutputRoot $ResolvedOutputRoot `
            -ManifestPath $ManifestPath `
            -PreCommitValidation $preCommitValidation

        return [pscustomobject][ordered]@{
            changed = [int]@($plan.changed).Count
            unchanged = [int]@($plan.unchanged).Count
            deleted = [int]@($plan.deleted).Count
        }
    }
    finally {
        if (-not [System.IO.File]::Exists($Scope.journalPath)) {
            if ([System.IO.Directory]::Exists($stagingRoot)) {
                Remove-SafeTransactionDirectory -Scope $Scope -Path $stagingRoot
            }
            if ([System.IO.Directory]::Exists($backupRoot)) {
                Remove-SafeTransactionDirectory -Scope $Scope -Path $backupRoot
            }
        }
    }
}

function New-CodeGenResult {
    param(
        [Parameter(Mandatory = $true)][string]$Status,
        [Parameter(Mandatory = $true)][string]$ConfigIdentity,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot,
        [object[]]$DirtyReasons = @(),
        [int]$Changed = 0,
        [int]$Unchanged = 0,
        [int]$Deleted = 0,
        [int64]$Duration = 0,
        [Parameter(Mandatory = $true)][string]$ResolvedLogPath,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    return [pscustomobject][ordered]@{
        status = $Status
        configIdentity = $ConfigIdentity
        outputRoot = ConvertTo-LogicalPath -Path $ResolvedOutputRoot
        dirtyReasons = [object[]]@($DirtyReasons)
        changed = [int]$Changed
        unchanged = [int]$Unchanged
        deleted = [int]$Deleted
        duration = [int64]$Duration
        logPath = ConvertTo-LogicalPath -Path $ResolvedLogPath
        manifestPath = ConvertTo-LogicalPath -Path $ManifestPath
    }
}

function Write-CodeGenResultPath {
    param(
        [Parameter(Mandatory = $true)]$Result,
        [string]$ResolvedResultPath,
        [Parameter(Mandatory = $true)][string]$ResolvedOutputRoot
    )

    if ([string]::IsNullOrWhiteSpace($ResolvedResultPath)) {
        return
    }
    if (Test-PathContained -Root $ResolvedOutputRoot -Path $ResolvedResultPath -AllowRoot) {
        throw "ResultPath must be outside OutputRoot: '$ResolvedResultPath'."
    }
    $bytes = $script:Utf8NoBom.GetBytes((ConvertTo-DeterministicJson -Value $Result))
    [void](Publish-BytesAtomic -Destination $ResolvedResultPath -Bytes $bytes)
}

function Write-CodeGenStampPath {
    param(
        [string]$ResolvedStampPath,
        [string]$ManifestPath
    )

    if ([string]::IsNullOrWhiteSpace($ResolvedStampPath)) {
        return
    }
    $manifest = Read-JsonFile -Path $ManifestPath
    $lines = New-Object 'System.Collections.Generic.List[string]'
    [void]$lines.Add('schemaVersion=1')
    [void]$lines.Add('configIdentity=' + [string]$manifest.configIdentity)
    [void]$lines.Add('generatorVersion=' + [string]$manifest.generatorVersion)
    foreach ($output in @($manifest.outputs)) {
        [void]$lines.Add(([string]$output.path) + '=' + ([string]$output.hash))
    }
    $bytes = $script:Utf8NoBom.GetBytes(($lines -join "`n") + "`n")
    [void](Publish-BytesAtomic -Destination $ResolvedStampPath -Bytes $bytes)
}

function Write-CodeGenSuccessOutput {
    param([Parameter(Mandatory = $true)]$Result)

    if ($Quiet) {
        return
    }
    if ($Json) {
        [Console]::Out.WriteLine((ConvertTo-DeterministicJson -Value $Result).TrimEnd())
        return
    }

    $reasonText = 'none'
    if (@($Result.dirtyReasons).Count -gt 0) {
        $reasonText = (@($Result.dirtyReasons | Select-Object -First 3) -join ',')
        if (@($Result.dirtyReasons).Count -gt 3) {
            $reasonText += ',...'
        }
    }
    [Console]::Out.WriteLine(
        "codegen: $($Result.status) config=$($Result.configIdentity.Substring(0, 12)) changed=$($Result.changed) unchanged=$($Result.unchanged) deleted=$($Result.deleted) dirty=$reasonText duration=$($Result.duration)ms")
}

function Append-CodeGenLog {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $directory = [System.IO.Path]::GetDirectoryName($Path)
    if (-not [System.IO.Directory]::Exists($directory)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }

    $encoding = $script:Utf8NoBom
    if ([System.IO.File]::Exists($Path)) {
        $prefix = New-Object byte[] 3
        $prefixLength = 0
        $prefixStream = New-Object System.IO.FileStream(
            $Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::ReadWrite)
        try {
            $prefixLength = $prefixStream.Read($prefix, 0, $prefix.Length)
        }
        finally {
            $prefixStream.Dispose()
        }
        if ($prefixLength -ge 2 -and $prefix[0] -eq 0xff -and $prefix[1] -eq 0xfe) {
            $encoding = [System.Text.Encoding]::Unicode
        }
        elseif ($prefixLength -ge 2 -and $prefix[0] -eq 0xfe -and $prefix[1] -eq 0xff) {
            $encoding = [System.Text.Encoding]::BigEndianUnicode
        }
    }
    [System.IO.File]::AppendAllText($Path, $Text + [Environment]::NewLine, $encoding)
}

function Get-PrimaryDiagnostics {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [int]$MaximumLines = 6
    )

    if (-not [System.IO.File]::Exists($Path)) {
        return @()
    }
    $matching = New-Object System.Collections.ArrayList
    $fallback = New-Object System.Collections.ArrayList
    $reader = New-Object System.IO.StreamReader($Path, [System.Text.Encoding]::UTF8, $true)
    try {
        $readCount = 0
        while (-not $reader.EndOfStream -and $readCount -lt 240 -and $matching.Count -lt $MaximumLines) {
            $readCount++
            $line = $reader.ReadLine()
            if ([string]::IsNullOrWhiteSpace($line)) {
                continue
            }
            if ($line.Length -gt 360) {
                $line = $line.Substring(0, 357) + '...'
            }
            if ($fallback.Count -lt $MaximumLines) {
                [void]$fallback.Add($line)
            }
            if ($line -match '(?i)(fatal|error|exception|failed|:[0-9]+:[0-9]+)') {
                [void]$matching.Add($line)
            }
        }
    }
    finally {
        $reader.Dispose()
    }
    if ($matching.Count -gt 0) {
        return [object[]]$matching.ToArray()
    }
    return [object[]]$fallback.ToArray()
}

function Write-CodeGenFailureOutput {
    param(
        [Parameter(Mandatory = $true)]$Result,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Quiet) {
        return
    }
    if ($Json) {
        [Console]::Out.WriteLine((ConvertTo-DeterministicJson -Value $Result).TrimEnd())
        return
    }

    if ($Message.Length -gt 600) {
        $Message = $Message.Substring(0, 597) + '...'
    }
    [Console]::Error.WriteLine("codegen: error: $Message")
    foreach ($diagnostic in @(Get-PrimaryDiagnostics -Path ([string]$Result.logPath))) {
        if ($diagnostic -cne $Message) {
            [Console]::Error.WriteLine("  $diagnostic")
        }
    }
    [Console]::Error.WriteLine("codegen: log: $($Result.logPath)")
}

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$outputLock = $null
$result = $null
$statusState = $null
$exitCode = 1
$resolvedRepositoryRoot = $null
$resolvedProjectRoot = $null
$resolvedOutputRoot = $null
$resolvedParserExecutable = $null
$resolvedInputList = $null
$resolvedContextFile = $null
$resolvedTemplateRoot = $null
$resolvedLogPath = $null
$resolvedResultPath = $null
$resolvedStampPath = $null
$manifestPath = $null
$fingerprint = $null

try {
    if ($null -ne $script:ArgumentNormalizationError) {
        throw $script:ArgumentNormalizationError
    }
    if ([string]::IsNullOrWhiteSpace($Module)) {
        throw 'Module cannot be empty.'
    }
    $resolvedModule = $Module.Trim()

    $resolvedRepositoryRoot = Get-FullPath -Path $RepositoryRoot
    if (-not [System.IO.Directory]::Exists($resolvedRepositoryRoot)) {
        throw "RepositoryRoot does not exist: '$resolvedRepositoryRoot'."
    }
    $resolvedProjectRoot = Get-FullPath -Path $ProjectRoot -BasePath $resolvedRepositoryRoot
    if (-not [System.IO.Directory]::Exists($resolvedProjectRoot)) {
        throw "ProjectRoot does not exist: '$resolvedProjectRoot'."
    }
    $resolvedOutputRoot = Get-FullPath -Path $OutputRoot -BasePath $resolvedRepositoryRoot
    $resolvedParserExecutable = Get-FullPath -Path $ParserExecutable -BasePath $resolvedRepositoryRoot
    $resolvedInputList = Get-FullPath -Path $InputList -BasePath $resolvedRepositoryRoot
    $resolvedContextFile = Get-FullPath -Path $ContextFile -BasePath $resolvedRepositoryRoot
    $resolvedTemplateRoot = Get-FullPath -Path $TemplateRoot -BasePath $resolvedRepositoryRoot
    Assert-SafeOutputRoot `
        -ResolvedOutputRoot $resolvedOutputRoot `
        -ResolvedRepositoryRoot $resolvedRepositoryRoot `
        -ResolvedProjectRoot $resolvedProjectRoot
    if (-not [string]::IsNullOrWhiteSpace($TestOnlyPublishFault)) {
        if ($env:HE_CODEGEN_ENABLE_TEST_FAULTS -cne '1') {
            throw 'TestOnlyPublishFault requires HE_CODEGEN_ENABLE_TEST_FAULTS=1.'
        }
        $testTemporaryRoot = Get-FullPath -Path ([System.IO.Path]::GetTempPath())
        if (-not (Test-PathContained -Root $testTemporaryRoot -Path $resolvedOutputRoot)) {
            throw 'TestOnlyPublishFault is restricted to an OutputRoot below the system temporary directory.'
        }
        if ($Action -notin @('Run', 'Ensure')) {
            throw 'TestOnlyPublishFault is valid only for Run or Ensure.'
        }
    }

    $logKey = (Get-Sha256Text -Text ((ConvertTo-LogicalPath -Path $resolvedOutputRoot).ToLowerInvariant())).Substring(0, 16)
    if ([string]::IsNullOrWhiteSpace($LogPath)) {
        $candidateLogPath = Join-Path $resolvedRepositoryRoot "out\agent\logs\codegen-$logKey.log"
    }
    else {
        $candidateLogPath = Get-FullPath -Path $LogPath -BasePath $resolvedRepositoryRoot
    }
    if (Test-PathContained -Root $resolvedOutputRoot -Path $candidateLogPath -AllowRoot) {
        throw "LogPath must be outside OutputRoot: '$candidateLogPath'."
    }
    Assert-SafeAuxiliaryPath `
        -ResolvedPath $candidateLogPath `
        -ResolvedRepositoryRoot $resolvedRepositoryRoot `
        -ResolvedProjectRoot $resolvedProjectRoot `
        -Label 'LogPath'
    $resolvedLogPath = $candidateLogPath
    if (-not [string]::IsNullOrWhiteSpace($ResultPath)) {
        $candidateResultPath = Get-FullPath -Path $ResultPath -BasePath $resolvedRepositoryRoot
        if (Test-PathContained -Root $resolvedOutputRoot -Path $candidateResultPath -AllowRoot) {
            throw "ResultPath must be outside OutputRoot: '$candidateResultPath'."
        }
        if (Test-PathEqual -Left $candidateResultPath -Right $resolvedLogPath) {
            throw 'ResultPath and LogPath must be different files.'
        }
        Assert-SafeAuxiliaryPath `
            -ResolvedPath $candidateResultPath `
            -ResolvedRepositoryRoot $resolvedRepositoryRoot `
            -ResolvedProjectRoot $resolvedProjectRoot `
            -Label 'ResultPath'
        $resolvedResultPath = $candidateResultPath
    }
    if (-not [string]::IsNullOrWhiteSpace($StampPath)) {
        $candidateStampPath = Get-FullPath -Path $StampPath -BasePath $resolvedRepositoryRoot
        if (Test-PathContained -Root $resolvedOutputRoot -Path $candidateStampPath -AllowRoot) {
            throw "StampPath must be outside OutputRoot: '$candidateStampPath'."
        }
        if ((Test-PathEqual -Left $candidateStampPath -Right $resolvedLogPath) -or
            ((-not [string]::IsNullOrWhiteSpace($resolvedResultPath)) -and
             (Test-PathEqual -Left $candidateStampPath -Right $resolvedResultPath))) {
            throw 'StampPath must differ from LogPath and ResultPath.'
        }
        Assert-SafeAuxiliaryPath `
            -ResolvedPath $candidateStampPath `
            -ResolvedRepositoryRoot $resolvedRepositoryRoot `
            -ResolvedProjectRoot $resolvedProjectRoot `
            -Label 'StampPath'
        $resolvedStampPath = $candidateStampPath
    }
    $manifestPath = Join-Path $resolvedOutputRoot '.he-codegen\manifest.json'
    Assert-NoReparsePoint -Path $manifestPath

    $scope = Get-TransactionScope -ResolvedOutputRoot $resolvedOutputRoot
    if (-not [System.IO.Directory]::Exists($scope.parent)) {
        [void][System.IO.Directory]::CreateDirectory($scope.parent)
    }
    Assert-NoReparsePoint -Path $scope.parent
    try {
        $outputLock = [System.IO.File]::Open(
            $scope.lockPath,
            [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
    }
    catch {
        throw "Another codegen operation owns OutputRoot '$resolvedOutputRoot'."
    }

    Recover-CodeGenTransaction -Scope $scope -ResolvedOutputRoot $resolvedOutputRoot -ManifestPath $manifestPath
    $manifestState = Read-ManifestState -ManifestPath $manifestPath -ResolvedOutputRoot $resolvedOutputRoot
    $includedFiles = @()
    if ($manifestState.valid) {
        $includedFiles = @($manifestState.manifest.inputs | Where-Object {$_.kind -eq 'include'} | ForEach-Object {$_.path})
    }
    $fingerprint = Get-CodeGenFingerprint `
        -ResolvedRepositoryRoot $resolvedRepositoryRoot `
        -ResolvedProjectRoot $resolvedProjectRoot `
        -ResolvedParserExecutable $resolvedParserExecutable `
        -ResolvedInputList $resolvedInputList `
        -ResolvedContextFile $resolvedContextFile `
        -ResolvedTemplateRoot $resolvedTemplateRoot `
        -ResolvedModule $resolvedModule `
        -IncludedFiles $includedFiles
    $statusState = Get-CodeGenStatus `
        -ManifestState $manifestState `
        -Fingerprint $fingerprint `
        -ResolvedOutputRoot $resolvedOutputRoot

    $shouldRun = ($Action -eq 'Run') -or ($Action -eq 'Ensure' -and $statusState.status -eq 'dirty')
    if ($shouldRun) {
        $runResult = Invoke-CodeGenRun `
            -Scope $scope `
            -Fingerprint $fingerprint `
            -ManifestState $manifestState `
            -ResolvedRepositoryRoot $resolvedRepositoryRoot `
            -ResolvedProjectRoot $resolvedProjectRoot `
            -ResolvedOutputRoot $resolvedOutputRoot `
            -ResolvedParserExecutable $resolvedParserExecutable `
            -ResolvedInputList $resolvedInputList `
            -ResolvedContextFile $resolvedContextFile `
            -ResolvedTemplateRoot $resolvedTemplateRoot `
            -ResolvedModule $resolvedModule `
            -ResolvedLogPath $resolvedLogPath `
            -ManifestPath $manifestPath

        $result = New-CodeGenResult `
            -Status 'generated' `
            -ConfigIdentity ([string]$fingerprint.configIdentity) `
            -ResolvedOutputRoot $resolvedOutputRoot `
            -DirtyReasons ([object[]]$statusState.dirtyReasons) `
            -Changed ([int]$runResult.changed) `
            -Unchanged ([int]$runResult.unchanged) `
            -Deleted ([int]$runResult.deleted) `
            -ResolvedLogPath $resolvedLogPath `
            -ManifestPath $manifestPath
        $exitCode = 0
    }
    else {
        $result = New-CodeGenResult `
            -Status ([string]$statusState.status) `
            -ConfigIdentity ([string]$fingerprint.configIdentity) `
            -ResolvedOutputRoot $resolvedOutputRoot `
            -DirtyReasons ([object[]]$statusState.dirtyReasons) `
            -Unchanged ([int]$statusState.unchanged) `
            -ResolvedLogPath $resolvedLogPath `
            -ManifestPath $manifestPath
        if ($Action -eq 'Check' -and $statusState.status -eq 'dirty') {
            $exitCode = 2
        }
        else {
            $exitCode = 0
        }
    }

    $stopwatch.Stop()
    $result.duration = [int64]$stopwatch.ElapsedMilliseconds
    Write-CodeGenResultPath -Result $result -ResolvedResultPath $resolvedResultPath -ResolvedOutputRoot $resolvedOutputRoot
    Write-CodeGenStampPath -ResolvedStampPath $resolvedStampPath -ManifestPath $manifestPath
    Write-CodeGenSuccessOutput -Result $result
}
catch {
    $caughtError = $_
    $stopwatch.Stop()
    $message = $caughtError.Exception.Message

    if ([string]::IsNullOrWhiteSpace($resolvedRepositoryRoot)) {
        try { $resolvedRepositoryRoot = Get-FullPath -Path $RepositoryRoot } catch { $resolvedRepositoryRoot = (Get-Location).Path }
    }
    if ([string]::IsNullOrWhiteSpace($resolvedOutputRoot)) {
        try { $resolvedOutputRoot = Get-FullPath -Path $OutputRoot -BasePath $resolvedRepositoryRoot } catch { $resolvedOutputRoot = Join-Path $resolvedRepositoryRoot 'out\agent\invalid-codegen-output' }
    }
    if ([string]::IsNullOrWhiteSpace($resolvedLogPath)) {
        $fallbackKey = (Get-Sha256Text -Text ([string]$resolvedOutputRoot)).Substring(0, 16)
        $resolvedLogPath = Join-Path $resolvedRepositoryRoot "out\agent\logs\codegen-$fallbackKey.log"
    }
    if ([string]::IsNullOrWhiteSpace($manifestPath)) {
        $manifestPath = Join-Path $resolvedOutputRoot '.he-codegen\manifest.json'
    }
    try {
        $diagnosticText = $caughtError.ToString()
        if (-not [string]::IsNullOrWhiteSpace([string]$caughtError.ScriptStackTrace)) {
            $diagnosticText += [Environment]::NewLine + $caughtError.ScriptStackTrace
        }
        Append-CodeGenLog -Path $resolvedLogPath -Text $diagnosticText
    }
    catch {
        # The original error remains primary if even the diagnostic log cannot be written.
    }

    $configIdentity = ('0' * 64)
    if ($null -ne $fingerprint) {
        $configIdentity = [string]$fingerprint.configIdentity
    }
    $failureReasons = New-Object System.Collections.ArrayList
    if ($null -ne $statusState) {
        foreach ($reason in @($statusState.dirtyReasons)) {
            [void]$failureReasons.Add([string]$reason)
        }
    }
    $boundedReason = $message
    if ($boundedReason.Length -gt 300) {
        $boundedReason = $boundedReason.Substring(0, 297) + '...'
    }
    [void]$failureReasons.Add('error:' + $boundedReason)
    $result = New-CodeGenResult `
        -Status 'error' `
        -ConfigIdentity $configIdentity `
        -ResolvedOutputRoot $resolvedOutputRoot `
        -DirtyReasons ([object[]]@(Sort-StringsOrdinal -Values @($failureReasons.ToArray()) -Unique)) `
        -Duration ([int64]$stopwatch.ElapsedMilliseconds) `
        -ResolvedLogPath $resolvedLogPath `
        -ManifestPath $manifestPath

    try {
        Write-CodeGenResultPath -Result $result -ResolvedResultPath $resolvedResultPath -ResolvedOutputRoot $resolvedOutputRoot
    }
    catch {
        # A ResultPath write error is already represented by a nonzero exit.
    }
    Write-CodeGenFailureOutput -Result $result -Message $message
    $exitCode = 1
}
finally {
    if ($null -ne $outputLock) {
        $outputLock.Dispose()
    }
}

exit $exitCode
