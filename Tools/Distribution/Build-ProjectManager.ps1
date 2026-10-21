param([Parameter(Mandatory=$true)][string]$Output,
    [ValidateSet('x64','arm64')][string]$Architecture='x64')
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$Output=[IO.Path]::GetFullPath($Output)
$compiler=Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$source=Join-Path $PSScriptRoot 'ProjectManager.cs'
$icon=Join-Path $repo 'Engine/Resources/Icons/EngineIcon.ico'
foreach ($file in @($compiler,$source,$icon)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Project Manager build input missing: $file" }
}
$automation=Get-ChildItem -LiteralPath (Join-Path $env:WINDIR 'Microsoft.NET/assembly/GAC_MSIL/System.Management.Automation') -Recurse -Filter System.Management.Automation.dll |
    Select-Object -First 1 -ExpandProperty FullName
if (!$automation) { throw 'Windows PowerShell assembly is missing.' }
$null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Output))
$platform=if ($Architecture -eq 'arm64') { '/platform:anycpu' } else { '/platform:x64' }
& $compiler /nologo /target:winexe $platform /reference:System.Windows.Forms.dll "/reference:$automation" "/win32icon:$icon" "/out:$Output" $source
if ($LASTEXITCODE -ne 0) { throw 'ProjectManager.exe compilation failed.' }
Write-Output "build-only PASS: ProjectManager.exe ($Architecture), embedded HEngine icon; $Output"
