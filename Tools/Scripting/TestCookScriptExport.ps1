param(
    [string]$Cooker = '',
    [string]$EvidenceDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$Cooker) { $Cooker = Join-Path $repo 'out/agent/editor-relwithdebinfo/bin/Programs/HCooker/HCooker.exe' }
if (!$EvidenceDirectory) { $EvidenceDirectory = Join-Path $repo ('out/agent/scripting/cook-script-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
$project = Join-Path $EvidenceDirectory 'project'
New-Item -ItemType Directory -Force "$project/Assets", "$project/Config" | Out-Null
. (Join-Path $repo 'Tools/Agent/Storage.ps1')
$run = Start-HEStorageRun -Project (Join-Path $repo 'Project') -Workflow 'script-cook-validation' -Paths @($EvidenceDirectory) -Outer
$state = 'failed'
$results = @()
function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 15), [Text.UTF8Encoding]::new($false))
}
try {
    Write-Json "$project/Config/DefaultEngine.json" @{}
    Write-Json "$project/Config/BaseEngine.json" @{}
    Copy-Item -LiteralPath (Join-Path $repo 'Project/Assets/WeChatSample/checker.png') -Destination "$project/Assets/script-only.png"
    Copy-Item -LiteralPath "$project/Assets/script-only.png" -Destination "$project/Assets/registry-only.png"
    $engine = Join-Path $EvidenceDirectory 'engine'
    $generated = Join-Path $EvidenceDirectory 'generated'
    New-Item -ItemType Directory -Force "$engine/Config", "$engine/Assets", $generated, "$project/Assets/DevOnly" | Out-Null
    Write-Json "$engine/Config/DefaultEngine.json" @{origin='engine'}
    [IO.File]::WriteAllText("$engine/Assets/script-only.png", 'fallback')
    [IO.File]::WriteAllText("$project/Assets/DevOnly/excluded.txt", 'development')
    $snapshot = Join-Path $EvidenceDirectory 'CookInput-a1b2c3d4'
    & cmake "-DENGINE_CONTENT_DIR=$engine" "-DENGINE_GENERATED_CONTENT_DIR=$generated" "-DPROJECT_DIR=$project" `
        "-DCOOK_INPUT_DIR=$snapshot" "-DCOOK_INPUT_PARENT=$EvidenceDirectory" -DPROJECT_ID=script-copy-test `
        -DPYTHON_PLUGIN_ENABLED=OFF -P "$repo/CMake/PrepareCookInput.cmake" *> "$EvidenceDirectory/snapshot.log"
    if ($LASTEXITCODE -ne 0) { throw 'Isolated snapshot preparation failed.' }
    if ((Get-FileHash "$snapshot/Assets/script-only.png").Hash -ne (Get-FileHash "$project/Assets/script-only.png").Hash -or
        [IO.File]::ReadAllText("$engine/Assets/script-only.png") -ne 'fallback' -or
        (Get-Content "$snapshot/Config/DefaultEngine.json" -Raw | ConvertFrom-Json).origin -ne 'engine' -or
        (Test-Path "$snapshot/Assets/DevOnly/excluded.txt")) { throw 'Snapshot overlay/source isolation contract failed.' }
    $results += [ordered]@{case='snapshot-overlay-isolation';status='PASS';exitCode=0}
    Write-Json "$project/Config/Game.json" @{Game=@{Name='Script cook contract';DefaultScene='Assets/Main.he'};Cook=@{ContentVersion='script-test';CompressionLevel=1;CookedJsonExtensions=@('.he')}}
    foreach ($case in @('enabled','registry-resource','renamed-field','unknown-field','field-type-mismatch','disabled','missing-resource','invalid-field','missing-registry','missing-type')) {
        $resource = if ($case -eq 'missing-resource') { 'Assets/missing.png' } else { 'Assets/script-only.png' }
        $tag = if ($case -eq 'invalid-field') { 'runtime-object' } else { 'resource' }
        $fields = @{schema=1;fields=@{texture=@{type=$tag;value=$resource}}} | ConvertTo-Json -Depth 8 -Compress
        if ($case -eq 'registry-resource') { $fields = '{"schema":1,"fields":{}}' }
        if ($case -eq 'renamed-field') { $fields = $fields.Replace('"texture":','"oldTexture":') }
        if ($case -eq 'unknown-field') { $fields = $fields.Replace('"texture":','"removedTexture":') }
        if ($case -eq 'field-type-mismatch') { $fields = $fields.Replace('"type":"resource"','"type":"string"') }
        $types = if ($case -eq 'missing-type') { @() } else { @(@{id='sample.touch';dependencies=@('Assets/registry-only.png');fields=@(@{id='texture';formerIds=@('oldTexture');defaultValue=@{type='resource';value=''}})}) }
        Write-Json "$EvidenceDirectory/registry.json" @{version=1;types=@($types)}
        $metadataArgs = if ($case -eq 'missing-registry') { @() } else { @('--csharp-metadata', "$EvidenceDirectory/registry.json") }
        Write-Json "$project/Assets/Main.he" @{'script-entity'=@{ScriptComponent=@{Backend='he.script.csharp';Type='sample.touch';Enabled=$false;Fields=$fields}}}
        $mode = if ($case -eq 'disabled') { 'disabled' } else { 'enabled' }
        $previous = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & $Cooker --project $project --output "$EvidenceDirectory/$case" --csharp-plugin $mode --python-plugin disabled @metadataArgs *> "$EvidenceDirectory/$case.log"
            $code = $LASTEXITCODE
        } finally { $ErrorActionPreference = $previous }
        $log = Get-Content "$EvidenceDirectory/$case.log" -Raw
        if ($case -in @('enabled','registry-resource','renamed-field','unknown-field')) {
            if ($code -ne 0) { throw "Enabled script cook failed: $log" }
            $report = Get-Content "$EvidenceDirectory/$case/CookReport.json" -Raw | ConvertFrom-Json
            $expectedResource = if ($case -eq 'registry-resource') { 'Assets/registry-only.png' } else { 'Assets/script-only.png' }
            $expectedProvider = if ($case -eq 'registry-resource') { 'ScriptRegistry' } else { 'SceneComponent' }
            $edges = @($report.Edges | Where-Object { $_.Target -eq $expectedResource -and $_.Provider -eq $expectedProvider })
            if (!$edges.Count) { throw 'No structured script-only resource edge in the cooked report.' }
        } elseif ($code -eq 0 -or $log -notmatch 'Assets/Main.he' -or $log -notmatch 'script-entity') {
            throw "Export rejection lacks the expected nonzero result and scene/entity context: $case"
        }
        $results += [ordered]@{case=$case;status='PASS';exitCode=$code}
    }
    $state = 'success'
} finally {
    Write-Json "$EvidenceDirectory/results.json" $results
    Complete-HEStorageRun -Run $run -State $state
}
Write-Host "unit-test real cooker script export PASS: $EvidenceDirectory"
