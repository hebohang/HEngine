# Shared by publish and preview; output may be outside the project.
function Resolve-HEBuildOutput([string]$ProjectRoot, [string]$OutputPath) {
    if ([string]::IsNullOrWhiteSpace($OutputPath)) { throw 'Choose an output directory.' }
    if ([IO.Path]::IsPathRooted($OutputPath) -and $OutputPath -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\[^\\]+)') {
        throw 'Output must be project-relative or a fully qualified absolute path.'
    }
    $path = if ([IO.Path]::IsPathRooted($OutputPath)) { $OutputPath } else { Join-Path $ProjectRoot $OutputPath }
    $path = [IO.Path]::GetFullPath($path).TrimEnd('\', '/')
    $engineRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..')).TrimEnd('\', '/')
    foreach ($protected in @($ProjectRoot, $engineRoot)) {
        $protected = [IO.Path]::GetFullPath($protected).TrimEnd('\', '/')
        if ($protected.Equals($path, [StringComparison]::OrdinalIgnoreCase) -or
            $protected.StartsWith($path + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Output cannot be the project, engine, or an ancestor directory.'
        }
    }
    if ($path -eq [IO.Path]::GetPathRoot($path).TrimEnd('\', '/')) { throw 'Output cannot be a drive root.' }
    # Reject junctions/symlinks, including ancestors, before moving any output.
    $cursor = $path
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Output must be a directory without symlink or junction ancestors.'
            }
        }
        $parent = Split-Path $cursor -Parent
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $path
}

function Assert-HEBuildOutputOwner([string]$OutputPath, [string]$ProjectRoot, [string]$Profile, [switch]$RequireBuild) {
    $marker = Join-Path $OutputPath '.he-build.json'
    if (Test-Path -LiteralPath $marker -PathType Leaf) {
        $owner = Get-Content -LiteralPath $marker -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($owner.project -ne $ProjectRoot -or $owner.profile -ne $Profile) { throw 'Output belongs to a different project/profile.' }
    } elseif ($RequireBuild) { throw 'Build this profile before previewing.' }
    elseif ((Test-Path -LiteralPath $OutputPath) -and @(Get-ChildItem -LiteralPath $OutputPath -Force).Count -gt 0) {
        throw "Choose an empty directory or the previous output of this profile: $OutputPath"
    }
}

function Publish-HEBuildOutput([string]$Stage, [string]$OutputPath, [string]$ProjectRoot, [string]$Profile) {
    $output = Resolve-HEBuildOutput $ProjectRoot $OutputPath
    Assert-HEBuildOutputOwner $output $ProjectRoot $Profile
    $parent = Split-Path $output -Parent
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $suffix = [Guid]::NewGuid().ToString('N')
    $incoming = "$output.incoming-$suffix"
    $previous = "$output.previous-$suffix"
    # Copy onto the destination volume before swapping directories. A failed
    # copy leaves the last successful build intact, even across drives.
    Copy-Item -LiteralPath $Stage -Destination $incoming -Recurse -ErrorAction Stop
    $null = Resolve-HEBuildOutput $ProjectRoot $output
    Assert-HEBuildOutputOwner $output $ProjectRoot $Profile
    if (Test-Path -LiteralPath $output) { Move-Item -LiteralPath $output -Destination $previous -ErrorAction Stop }
    try { Move-Item -LiteralPath $incoming -Destination $output -ErrorAction Stop }
    catch {
        if (Test-Path -LiteralPath $previous) { Move-Item -LiteralPath $previous -Destination $output -ErrorAction Stop }
        throw
    }
    if (Test-Path -LiteralPath $previous) { Write-Output "Previous output preserved: $previous" }
}
