param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$TemplateContract,
    [Parameter(Mandatory=$true)][string]$Manifest,
    [string]$GameConfig = ''
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repo 'Tools/Agent/Project.ps1')
$info = Resolve-HEProject -ProjectPath $Project -RepositoryRoot $repo
$descriptor = Get-Content -Raw -Encoding UTF8 -LiteralPath $info.DescriptorPath | ConvertFrom-Json
$contract = Get-Content -Raw -Encoding UTF8 -LiteralPath $TemplateContract | ConvertFrom-Json
if ($contract.SchemaVersion -ne 1 -or $contract.EngineVersion -ne $info.EngineVersion -or
    $contract.RuntimeFingerprint -notmatch '^[0-9a-f]{64}$' -or
    $contract.PackGame -ne $true -or $contract.DevAgent -ne $false -or $contract.Configuration -ne 'Release') {
    throw 'A compatible Release Shipping player contract is required.'
}
if (!$GameConfig) { $GameConfig = $descriptor.GameConfig }
$GameConfig = $GameConfig.Replace('\','/')
if ($GameConfig -notmatch '^[A-Za-z0-9_./-]+[.]json$' -or $GameConfig -match '(^|/)\.\.(/|$)' -or [IO.Path]::IsPathRooted($GameConfig)) {
    throw 'Player GameConfig must be a safe project-relative JSON file.'
}
$manifestPath = (Resolve-Path -LiteralPath $Manifest).Path
$manifestData = Get-Content -Raw -Encoding UTF8 -LiteralPath $manifestPath | ConvertFrom-Json
if ($manifestData.ManifestVersion -ne 1 -or !(@($manifestData.Files.Path) -ccontains $GameConfig)) {
    throw 'Player GameConfig is absent from the cooked package.'
}
$identity = [ordered]@{
    SchemaVersion=1;EngineVersion=$info.EngineVersion;ProjectId=$info.ProjectId
    DisplayName=$info.DisplayName;ExecutableName=$info.ExecutableName;GameConfig=$GameConfig
    RuntimeFingerprint=$contract.RuntimeFingerprint
}
if ($descriptor.LegacySavedDirectory) { $identity.LegacySavedDirectory = $descriptor.LegacySavedDirectory }
$manifestData | Add-Member -MemberType NoteProperty -Name Player -Value $identity -Force
$temporary = $manifestPath + '.tmp-' + [guid]::NewGuid().ToString('N')
try {
    [IO.File]::WriteAllText($temporary, ($manifestData | ConvertTo-Json -Depth 64), [Text.UTF8Encoding]::new($false))
    # A scanner can briefly open freshly cooked files without FILE_SHARE_DELETE.
    # Retry only Windows sharing/replacement failures, preserving atomic replace.
    for ($attempt = 0; ; ++$attempt) {
        try {
            [IO.File]::Replace($temporary, $manifestPath, [NullString]::Value)
            break
        } catch {
            $failure = $_.Exception.GetBaseException()
            $windowsError = $failure.HResult -band 0xffff
            if ($attempt -ge 7 -or $windowsError -notin @(32,33,1175) -or
                !(Test-Path -LiteralPath $temporary) -or !(Test-Path -LiteralPath $manifestPath)) { throw }
            Start-Sleep -Milliseconds ([Math]::Min(100 * [Math]::Pow(2, $attempt), 1000))
        }
    }
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
Write-Output "Player metadata PASS: $($info.ProjectId); $($contract.Platform)"
