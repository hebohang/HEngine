# AAPT2 on Windows cannot reliably consume Unicode or long paths. Stage only
# the managed Android host and verified inputs here; project paths stay intact.
function New-HeAndroidPackageWorkspace([string]$toolchainRoot) {
    $candidates = if ($env:HE_ANDROID_BUILD_ROOT) { @($env:HE_ANDROID_BUILD_ROOT) }
        else { @((Join-Path $toolchainRoot 'package-work'), (Join-Path ([IO.Path]::GetTempPath()) 'HEngineAndroid')) }
    foreach ($candidate in $candidates) {
        $root = [IO.Path]::GetFullPath($candidate).TrimEnd('\','/')
        if ($root -match '[^\x20-\x7e]' -or $root.Length -gt 90 -or $root.StartsWith('\\')) { continue }
        try {
            $null = New-Item -ItemType Directory -Force -Path $root
            $path = Join-Path $root ([guid]::NewGuid().ToString('N'))
            $null = New-Item -ItemType Directory -Path $path
            $marker = Join-Path $path '.he-android-workspace'
            [IO.File]::WriteAllText($marker, $path)
            return [pscustomobject]@{ Root=$root; Path=$path }
        } catch {
            if ($env:HE_ANDROID_BUILD_ROOT) { throw }
        }
    }
    throw 'Android packaging needs a writable short ASCII cache path. Set HE_ANDROID_BUILD_ROOT (for example D:\HEngineAndroid). SDK and project names may remain unchanged.'
}

function Remove-HeAndroidPackageWorkspace($workspace) {
    $path = [IO.Path]::GetFullPath($workspace.Path)
    $root = [IO.Path]::GetFullPath($workspace.Root).TrimEnd('\','/')
    if ((Split-Path $path -Parent) -cne $root -or (Split-Path $path -Leaf) -notmatch '^[a-f0-9]{32}$') { throw 'Invalid Android workspace ownership.' }
    $marker = Join-Path $path '.he-android-workspace'
    if (!(Test-Path -LiteralPath $marker) -or [IO.File]::ReadAllText($marker) -cne $path) { throw 'Missing Android workspace ownership marker.' }
    $links = @(Get-Item -LiteralPath $path; Get-ChildItem -LiteralPath $path -Recurse -Force) |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }
    if ($links) { throw 'Refusing to remove Android workspace containing a reparse point.' }
    Remove-Item -LiteralPath $path -Recurse -Force
}
