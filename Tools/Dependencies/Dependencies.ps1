param(
    [ValidateSet('check','install')][string]$Action = 'check',
    [ValidateSet('Editor','DevGame','Tests','Package','Parser','SDK','Source')][string]$Profile = 'Editor',
    [string]$Project = '', [string]$PluginConfig = '',
    [switch]$Offline, [switch]$WhatIf, [switch]$Json
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Agent/Toolchain.ps1')
. (Join-Path $PSScriptRoot '../Agent/Project.ps1')
. (Join-Path $PSScriptRoot '../Agent/LibClang.ps1')
. (Join-Path $PSScriptRoot 'Environment.ps1')
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$sourceMode = $Profile -eq 'Source'
if ($sourceMode -and (Test-Path -LiteralPath "$repo/HEngine.distribution.json")) { throw 'The Source dependency profile requires an engine source checkout.' }
if ($sourceMode) { $Project = Join-Path $repo 'Engine/Templates/Blank' }
$sdkMode = $Profile -eq 'SDK' -or (Test-Path -LiteralPath "$repo/HEngine.distribution.json")
$nodeArchitecture=Get-HeWindowsHostArchitecture
if (Test-Path -LiteralPath "$repo/HEngine.distribution.json") {
    $sdkPlatform=(Get-Content "$repo/HEngine.distribution.json" -Raw | ConvertFrom-Json).Platform
    $nodeArchitecture=if ($sdkPlatform -eq 'Windows-arm64') { 'arm64' } else { 'x64' }
}
$cache = Join-Path $repo 'out/agent/dependencies/windows'
$lock = Get-Content -Raw (Join-Path $PSScriptRoot 'windows.lock.json') | ConvertFrom-Json
$projectInfo = Resolve-HEProject -ProjectPath $Project -RepositoryRoot $repo
$configPath = if ($PluginConfig) { [IO.Path]::GetFullPath($PluginConfig) } else { Join-Path $projectInfo.Root 'Config/Plugins.json' }
$plugins = if ($sourceMode) { [pscustomobject]@{} } else { Get-Content -Raw -LiteralPath $configPath | ConvertFrom-Json }
$hostName = if ($Profile -eq 'Package') { 'Game' } else { $Profile }
$csharp = $sdkMode -or $sourceMode -or ($Profile -ne 'Parser' -and $plugins.'he.script.csharp' -and $plugins.'he.script.csharp'.$hostName)
$needsOidn = $sourceMode -or (!$sdkMode -and $Profile -ne 'Parser' -and $plugins.'he.offline-reference-renderer' -and $plugins.'he.offline-reference-renderer'.$hostName)
if ((Resolve-HeWindowsArchitecture) -eq 'arm64') { $needsOidn = $false }
$needsNode = $sdkMode -or $sourceMode -or ($csharp -and (Test-Path (Join-Path $projectInfo.Root 'Config/SharedUI.json')))
$sdk = (Get-Content -Raw (Join-Path $repo 'Engine/Managed/global.json') | ConvertFrom-Json).sdk.version
$runtime = Get-Content -Raw (Join-Path $repo 'Tools/Scripting/runtime.lock.json') | ConvertFrom-Json
$records = [Collections.Generic.List[object]]::new()
function Add-Check($Name, $Required, [scriptblock]$Probe) {
    if (!$Required) { return }
    try { $detail = & $Probe; $records.Add([pscustomobject]@{name=$Name;pass=$true;detail=[string]$detail}) }
    catch { $records.Add([pscustomobject]@{name=$Name;pass=$false;detail=$_.Exception.Message}) }
}
function Get-Python {
    if ($env:HE_BUILD_PYTHON) { $candidate = $env:HE_BUILD_PYTHON }
    elseif (Get-Command py.exe -ErrorAction SilentlyContinue) { $candidate = (& py -3.11 -c 'import sys; print(sys.executable)' 2>$null) }
    elseif (Get-Command python.exe -ErrorAction SilentlyContinue) { $candidate = (Get-Command python.exe).Source }
    else { $candidate = Join-Path $env:LOCALAPPDATA 'Programs/Python/Python311/python.exe' }
    if (!$candidate) { throw 'Python 3.11 not found.' }
    $version = & $candidate -c 'import sys; assert sys.version_info[:2] == (3,11); print(sys.version.split()[0])'
    if ($LASTEXITCODE -ne 0) { throw 'Python 3.11 required for the current content tools.' }
    return $candidate
}
function Run-Checks {
    $records.Clear()
    Import-HEDependencyEnvironment
    Add-Check 'native-runtimes' (Test-Path -LiteralPath "$repo/HEngine.distribution.json") {
        . "$repo/Tools/Distribution/Template.ps1"
        $hostPlatform=Get-HEWindowsTemplatePlatform $repo
        . "$PSScriptRoot/RuntimeDependencies.ps1"
        $editor = Test-HENativeRuntimeDependencies -Root $repo
        $null = Test-HENativeRuntimeDependencies -Root "$repo/ExportTemplates/$hostPlatform/Player" -Shipping
        $null = Test-HENativeRuntimeDependencies -Root "$repo/DevelopmentTemplates/$hostPlatform/Player"
        "FSR=$($editor.Fsr); OIDN=$($editor.Oidn); versioned runtime files verified"
    }
    Add-Check 'libclang' (!$sdkMode) { $clang = Resolve-HELibClang; "$($clang.Version) $($clang.Root)" }
    Add-Check 'native' (!$sdkMode) {
        $vs = Get-HeVisualStudio; Import-HeToolchain -Toolchain $vs
        & ninja --version | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Ninja unavailable.' }
        if (!(Get-Command rc.exe -ErrorAction SilentlyContinue)) { throw 'Windows SDK rc.exe unavailable.' }
        "$($vs.Generator) / $($vs.Toolset)"
    }
    Add-Check 'cmake' ($sdkMode -or $sourceMode) {
        $version = & cmake --version
        if ($LASTEXITCODE -ne 0 -or ($version -join "`n") -notmatch 'cmake version (\d+\.\d+\.\d+)' -or
            [version]$Matches[1] -lt [version]'4.2.0') { throw "CMake 4.2+ required; run hectl deps install -Profile $Profile." }
        $probe = & cmake -P "$PSScriptRoot/CheckCMake.cmake" 2>&1
        if ($LASTEXITCODE -ne 0) { throw "CMake script-mode/module check failed; run hectl deps install -Profile $Profile. $($probe -join ' ')" }
        (Get-Command cmake.exe).Source
    }
    Add-Check 'oidn' $needsOidn {
        $oidnRoot = Join-Path $cache 'oidn'
        foreach ($relative in @('lib/cmake/OpenImageDenoise-2.5.1/OpenImageDenoiseConfig.cmake',
            'include/OpenImageDenoise/oidn.h','bin/OpenImageDenoise.dll','bin/OpenImageDenoise_core.dll',
            'bin/OpenImageDenoise_device_cpu.dll','bin/tbb12.dll','doc/LICENSE.txt','doc/third-party-programs.txt','doc/third-party-programs-oneTBB.txt')) {
            if (!(Test-Path (Join-Path $oidnRoot $relative))) { throw 'Pinned OIDN missing; run hectl deps install.' }
        }
        $nativeLock = Get-Content -LiteralPath "$PSScriptRoot/native-runtime.lock.json" -Raw | ConvertFrom-Json
        foreach ($file in $nativeLock.files.PSObject.Properties | Where-Object { $_.Name -match 'OpenImageDenoise|^tbb12.dll$' }) {
            $candidate = Join-Path "$oidnRoot/bin" (Split-Path $file.Name -Leaf)
            if ((Get-HEFileDigest $candidate 'SHA256') -cne $file.Value) { throw "OIDN runtime hash mismatch: $candidate" }
        }
        $oidnRoot
    }
    Add-Check 'python'  ($Profile -ne 'Parser') { Get-Python }
    Add-Check 'dotnet' $csharp {
        $dotnet = (Get-Command dotnet.exe -ErrorAction Stop).Source
        if (!(@(& $dotnet --list-sdks) -match "^$([regex]::Escape($sdk)) ")) { throw ".NET SDK $sdk missing." }
        if (!(Test-Path (Join-Path (Split-Path $dotnet) "host/fxr/$($runtime.version)/hostfxr.dll"))) { throw ".NET runtime $($runtime.version) missing." }
        "$sdk / $($runtime.version)"
    }
    Add-Check 'node' $needsNode {
        & node -e 'process.exit(parseInt(process.versions.node) >= 18 && process.arch === process.argv[1] ? 0 : 1)' $nodeArchitecture
        if ($LASTEXITCODE -ne 0) { throw "Node.js >= 18 ($nodeArchitecture) required for the SDK's native esbuild." }
        (Get-Command npm.cmd -ErrorAction Stop).Source
    }
    Add-Check 'pillow' ($sdkMode -or $sourceMode) {
        $python = Get-Python
        & $python -c "import importlib.util,sys; sys.exit(1) if importlib.util.find_spec('PIL') is None else None; from PIL import Image; import PIL; assert PIL.__version__ == '11.3.0'; Image.new('RGB', (1,1)).resize((2,2))"
        if ($LASTEXITCODE -ne 0) { throw 'Pinned Pillow 11.3.0 is required for portable texture export.' }
        '11.3.0'
    }
    Add-Check 'wasm-tools' $sdkMode {
        Push-Location (Join-Path $repo 'Engine/Managed')
        try { $installed = & dotnet workload list; $code = $LASTEXITCODE } finally { Pop-Location }
        if ($code -ne 0 -or !(@($installed) -match '^wasm-tools\s+10\.0\.110/10\.0\.100\s')) {
            throw 'Pinned wasm-tools 10.0.110 is required for Web and WeChat exports.'
        }
        '10.0.110/10.0.100'
    }
    Add-Check 'esbuild' $needsNode {
        & node -e "const e=require(process.argv[1]);if(e.version!=='0.25.12')process.exit(1);e.transformSync('let x=1')" (Join-Path $repo 'Tools/Scripting/node_modules/esbuild')
        if ($LASTEXITCODE -ne 0) { throw 'Locked esbuild installation missing or unusable.' }
        '0.25.12'
    }
}
function Get-Archive($Url, $Digest, $Algorithm='SHA256') {
    $archive = Join-Path $cache ('archives/' + $Digest + '.zip')
    if (Test-Path -LiteralPath $archive) {
        if ((Get-HEFileDigest $archive $Algorithm) -ne $Digest) { throw "Corrupt dependency archive: $archive" }
        return $archive
    }
    if ($Offline) { throw "Offline dependency archive missing: $Url" }
    New-Item -ItemType Directory -Force (Split-Path $archive) | Out-Null
    & curl.exe --fail --location --retry 3 --max-time 600 --silent --show-error $Url -o ($archive + '.partial')
    if ($LASTEXITCODE -ne 0 -or (Get-HEFileDigest ($archive+'.partial') $Algorithm) -ne $Digest) { throw "Dependency download/checksum failed: $Url" }
    Move-Item -LiteralPath ($archive+'.partial') -Destination $archive
    return $archive
}
function Install-Zip($Archive, $Destination, $ArchiveRoot='') {
    $stage = Join-Path $cache ('stage-' + [guid]::NewGuid().ToString('N'))
    $python = Get-Python
    $arguments = @("$PSScriptRoot/InstallArchive.py",'--archive',$Archive,'--stage',$stage,'--destination',$Destination)
    if ($ArchiveRoot) { $arguments += @('--archive-root',$ArchiveRoot) }
    & $python @arguments
    if ($LASTEXITCODE -ne 0) { throw "Dependency archive installation failed: $Archive" }
}
function Install-Signed($Name,$Url,$Arguments,$Publisher) {
    if ($Offline) { throw "$Name installer requires online mode." }
    $installer = Join-Path $cache ($Name+'.exe')
    & curl.exe --fail --location --retry 3 --max-time 600 --silent --show-error $Url -o $installer
    if ($LASTEXITCODE -ne 0) { throw "$Name download failed." }
    $signature = Get-AuthenticodeSignature -LiteralPath $installer
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch $Publisher) { throw "$Name installer signature invalid." }
    $process = Start-Process -FilePath $installer -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -eq 3010) { throw "$Name installed; restart Windows and rerun dependency checks." }
    if ($process.ExitCode -ne 0) { throw "$Name installer exited $($process.ExitCode)." }
}
Run-Checks
if ($Action -eq 'install' -and !$WhatIf) {
    New-Item -ItemType Directory -Force $cache | Out-Null
    $guard = [IO.File]::Open((Join-Path $cache '.install.lock'),'OpenOrCreate','ReadWrite','None')
    try {
        # Python bootstraps archive extraction, including on a clean SDK machine.
        foreach ($missing in @($records | Where-Object {!$_.pass} | Sort-Object @{Expression={if($_.name -eq 'python'){0}else{1}}})) {
            switch ($missing.name) {
                'native-runtimes' { throw "Installed engine runtime payload is invalid; repair or reinstall this exact engine release. $($missing.detail)" }
                'libclang' { throw $missing.detail }
                'native' {
                    if ($sourceMode) {
                        Write-Warning 'Install Visual Studio 2026 (v145) or 2022 (v143) manually: Desktop development with C++, x64/x86 MSVC, C++ CMake tools (Ninja), and Windows SDK 10.0.26100. Rerun Win-InstallDependencies.bat afterwards.'
                    } else {
                        $arguments = @('--passive','--wait','--norestart')
                        foreach ($component in $lock.visualStudio.components) { $arguments += @('--add',$component) }
                        Install-Signed 'vs-buildtools' $lock.visualStudio.url $arguments 'Microsoft Corporation'
                    }
                }
                'python' { Install-Signed 'python' $lock.python.url @('/quiet','InstallAllUsers=0','Include_launcher=1','InstallLauncherAllUsers=0','Include_test=0','PrependPath=0') 'Python Software Foundation' }
                'dotnet' {
                    $pin = (Get-Content -Raw (Join-Path $repo 'Tools/Android/toolchain.lock.json') | ConvertFrom-Json).dotnet
                    if ($pin.version -ne $sdk) { throw 'SDK download lock differs from global.json.' }
                    Install-Zip (Get-Archive $pin.url $pin.hash $pin.algorithm) (Join-Path $cache 'dotnet')
                    Install-Zip (Get-Archive $runtime.url $runtime.sha512 'SHA512') (Join-Path $cache 'dotnet')
                }
                'oidn' { Install-Zip (Get-Archive $lock.oidn.url $lock.oidn.sha256) (Join-Path $cache 'oidn') $lock.oidn.archiveRoot }
                'node' {
                    $nodePin=if ($nodeArchitecture -eq 'arm64') { $lock.nodeArm64 } else { $lock.node }
                    Install-Zip (Get-Archive $nodePin.url $nodePin.sha256) (Join-Path $cache 'node') $nodePin.archiveRoot
                }
                'cmake' { Install-Zip (Get-Archive $lock.cmake.url $lock.cmake.sha256) (Join-Path $cache 'cmake') $lock.cmake.archiveRoot }
                'pillow' {
                    # A pinned CPython 3.11 x64 wheel is a zip; keep it SDK-local.
                    Install-Zip (Get-Archive $lock.pillow.url $lock.pillow.sha256) (Join-Path $cache 'python-packages')
                }
            }
        }
        Import-HEDependencyEnvironment
        if ($sdkMode -and @($records | Where-Object {$_.name -eq 'wasm-tools' -and !$_.pass}).Count) {
            if ($Offline) { throw 'The wasm-tools workload installer requires online mode.' }
            # Do not mutate a global SDK or require an elevated installer.
            if (!(Test-Path -LiteralPath "$cache/dotnet/dotnet.exe")) {
                $pin = (Get-Content -Raw (Join-Path $repo 'Tools/Android/toolchain.lock.json') | ConvertFrom-Json).dotnet
                if ($pin.version -ne $sdk) { throw 'SDK download lock differs from global.json.' }
                Install-Zip (Get-Archive $pin.url $pin.hash $pin.algorithm) (Join-Path $cache 'dotnet')
                Install-Zip (Get-Archive $runtime.url $runtime.sha512 'SHA512') (Join-Path $cache 'dotnet')
                Import-HEDependencyEnvironment
            }
            Push-Location (Join-Path $repo 'Engine/Managed')
            try {
                & "$cache/dotnet/dotnet.exe" workload install wasm-tools --from-rollback-file "$PSScriptRoot/wasm-workload.lock.json" --disable-parallel *> "$cache/wasm-install.log"
                if ($LASTEXITCODE -ne 0) { throw "Pinned wasm-tools installation failed; see $cache/wasm-install.log" }
            } finally { Pop-Location }
        }
        if ($needsNode -and @($records | Where-Object {$_.name -eq 'esbuild' -and !$_.pass}).Count) {
            Push-Location (Join-Path $repo 'Tools/Scripting')
            try {
                $arguments = @('ci','--ignore-scripts','--include=optional','--no-audit','--no-fund')
                if ($Offline) { $arguments += '--offline' }
                & npm.cmd @arguments *> (Join-Path $cache 'npm-restore.log')
                if ($LASTEXITCODE -ne 0) { throw 'Locked npm restore failed.' }
            } finally { Pop-Location }
        }
        Run-Checks
    } finally { $guard.Dispose() }
}
$result = [ordered]@{profile=$Profile;project=$projectInfo.Root;pass=(@($records | Where-Object {!$_.pass}).Count -eq 0);checks=@($records.ToArray());whatIf=[bool]$WhatIf}
if ($Json) { $result | ConvertTo-Json -Depth 5 } else { $records | Format-Table -Wrap; Write-Output "Dependencies $Profile PASS=$($result.pass)" }
if (!$result.pass -and !$WhatIf) { exit 1 }
