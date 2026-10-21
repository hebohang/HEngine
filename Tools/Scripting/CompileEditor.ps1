param(
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$Dotnet = 'dotnet',
    [string]$CMake = 'cmake',
    [ValidateSet('Windows','macOS')][string]$Platform = $(if (($PSVersionTable.PSEdition -eq 'Core') -and $IsMacOS) { 'macOS' } else { 'Windows' })
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
. (Join-Path $repo 'Tools/Dependencies/Environment.ps1')
Import-HEDependencyEnvironment
$work = Join-Path $repo ('out/agent/scripting/editor-compile-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$run = Start-HEStorageRun -Project $Project -Workflow 'script-editor-compile' -Paths @($work) -Outer
$state = 'failed'
try {
    # Never overwrite a loaded assembly or share the editor's CMake work directory.
    & "$PSScriptRoot/BuildManaged.ps1" -Project $Project -WorkDirectory "$work/managed" `
        -OutputDirectory "$work/output" -Configuration Release -Dotnet $Dotnet -CMake $CMake -Platform $Platform -IsolatedCompiler -EditorHost *> "$work/compiler.log"
    if ($LASTEXITCODE -ne 0) { throw 'C# compiler failed.' }
    if (Test-Path -LiteralPath (Join-Path $repo 'HEngine.distribution.json')) {
        # Keep the last verified generation with the project across editor restarts.
        # Never replace DLLs currently loaded by the stable host or a play session.
        $generation = [Guid]::NewGuid().ToString('N')
        $cache = Join-Path $Project 'Saved/Scripting/CSharp'
        $null = New-Item -ItemType Directory -Force -Path $cache
        Copy-Item -LiteralPath "$work/output" -Destination (Join-Path $cache $generation) -Recurse
        . "$repo/Tools/Agent/Project.ps1"
        Write-HEWorkspace (Join-Path $cache 'current.json') ([ordered]@{generation=$generation})
    }
    $state = 'success'
} catch {
    Write-Output $_.Exception.Message
} finally {
    $diagnostics = @()
    $modules = @{}
    if (Test-Path "$work/compiler.log") {
        foreach ($line in [IO.File]::ReadLines("$work/compiler.log")) {
            if ($line.StartsWith('HE_SCRIPT_MODULE_STATUS=')) {
                $module = $line.Substring('HE_SCRIPT_MODULE_STATUS='.Length) | ConvertFrom-Json
                $modules[$module.id] = $module
            }
            if ($line.StartsWith('HE_SCRIPT_MODULE_DIAGNOSTIC=')) {
                $diagnostic = $line.Substring('HE_SCRIPT_MODULE_DIAGNOSTIC='.Length) | ConvertFrom-Json
                if (-not $diagnostic.PSObject.Properties['log']) { $diagnostic | Add-Member NoteProperty log "$work/compiler.log" }
                $diagnostics += $diagnostic
            }
            if ($line -match '^(?<file>.+)\((?<line>\d+),(?<column>\d+)\):\s*(?<severity>error|warning)\s+(?<code>[A-Za-z]+\d+):\s*(?<message>.+?)(?:\s+\[(?<module>[^\]]+)\])?$') {
                $diagnostics += [ordered]@{severity=$matches.severity;code=$matches.code;file=$matches.file;line=[int]$matches.line;column=[int]$matches.column;
                    module=$matches.module;message=$matches.message;log="$work/compiler.log"}
            }
        }
    }
    [ordered]@{schema=1;status=$state;log="$work/compiler.log";output="$work/output";diagnostics=$diagnostics;modules=@($modules.Values | Sort-Object id)} |
        ConvertTo-Json -Depth 6 | Set-Content "$work/compile-result.json" -Encoding UTF8
    if (Test-Path "$work/compiler.log") { Get-Content "$work/compiler.log" -Tail 300 }
    Write-Output "HE_SCRIPT_DIAGNOSTICS=$work/compile-result.json"
    Write-Output "Full compiler log: $work/compiler.log"
    Complete-HEStorageRun -Run $run -State $state
}
if ($state -ne 'success') { exit 1 }
Write-Output "HE_SCRIPT_OUTPUT=$work/output"
Write-Output 'Compile PASS. The verified generation is staged for loading at a stopped scene boundary.'
