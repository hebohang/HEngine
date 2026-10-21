param([string]$EvidenceDirectory = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
if (!$EvidenceDirectory) { $EvidenceDirectory = "$repo/out/agent/scripting/pc-game-evidence-$(Get-Date -Format yyyyMMdd-HHmmss-fff)" }
$work = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Force $work | Out-Null
$tag = [Guid]::NewGuid().ToString('N')
$relative = "Assets/AgentScriptValidation-$tag"
$fixtures = [IO.Path]::GetFullPath("$repo/Project/$relative")
$session = "$repo/Tools/Agent/Session.ps1"
$exe = "$repo/out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe"
$sha = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant()
$managed = Get-Content (Join-Path (Split-Path $exe) 'Scripting/CSharp/build.json') -Raw | ConvertFrom-Json
$checks = [Collections.Generic.List[string]]::new()
$state = 'FAIL'; $started = $false; $stopped = $false; $index = 0
. "$repo/Tools/Agent/Storage.ps1"
$record = Start-HEStorageRun -Project "$repo/Project" -Workflow 'scripting-pc-validation' -Paths @($work) -Outer
function Assert-Pc($condition, [string]$name) {
    if (!$condition) { throw "PC script check failed: $name" }
    $checks.Add($name)
}
function Call-Pc([string]$method, $parameters = @{}) {
    $script:index++
    $log = Join-Path $work ('{0:D3}-{1}.json' -f $script:index, $method)
    # Pass structured JSON in-process, avoiding Windows native argv quoting.
    # Success is the parsed response, never a stale native LASTEXITCODE.
    $request = $parameters | ConvertTo-Json -Depth 16 -Compress
    & $session -Action Call -OwnerTag $tag -Method $method -ParamsJson $request > $log
    $response = Get-Content $log -Raw | ConvertFrom-Json
    if (!$response.ok) { throw "Agent rejected $method; $log" }
    return $response.result
}
function Read-PcEntity { Call-Pc 'world.get' @{uuid=$entityId;components=@('ScriptComponent','TransformComponent','SpriteRendererComponent')} }
function Fields-Pc($entity) { ($entity.components.ScriptComponent.Fields | ConvertFrom-Json).fields }
function Load-Pc([string]$name) {
    $path = "$relative/$name.he"
    $null = Call-Pc 'runtime.scene.load' @{path=$path;forceReload=$true}
    for ($attempt=0; $attempt -lt 30; $attempt++) {
        $status = Call-Pc 'session.status'
        if (!$status.runtimeSceneRequestPending -and $status.scene -eq $path) {
            $null = Call-Pc 'runtime.step' @{frames=1}
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Scene did not commit: $path"
}
try {
    New-Item -ItemType Directory $fixtures | Out-Null
    $base = Get-Content "$repo/Project/Assets/WeChatSample/ScriptCoexist.he" -Raw | ConvertFrom-Json
    $entry = @($base.PSObject.Properties | Where-Object {$_.Value.ScriptComponent.Backend -eq 'he.script.csharp'})
    if ($entry.Count -ne 1) { throw 'Expected one real C# sample in source scene.' }
    $entityId = $entry[0].Name
    $fields = $entry[0].Value.ScriptComponent.Fields | ConvertFrom-Json
    $fields.fields | Add-Member -NotePropertyName saveKey -NotePropertyValue @{type='string';value="pc-agent-$tag"}
    $entry[0].Value.ScriptComponent.Fields = $fields | ConvertTo-Json -Depth 16 -Compress
    $base | ConvertTo-Json -Depth 32 | Set-Content "$fixtures/active.he" -Encoding UTF8
    $entry[0].Value.ScriptComponent.Enabled = $false
    $base | ConvertTo-Json -Depth 32 | Set-Content "$fixtures/disabled.he" -Encoding UTF8
    $entry[0].Value.ScriptComponent.Enabled = $true
    $fields.fields | Add-Member -NotePropertyName throwOnUpdate -NotePropertyValue @{type='bool';value=$true}
    $entry[0].Value.ScriptComponent.Fields = $fields | ConvertTo-Json -Depth 16 -Compress
    $base | ConvertTo-Json -Depth 32 | Set-Content "$fixtures/fault.he" -Encoding UTF8
    $base.PSObject.Properties.Remove($entityId)
    $base | ConvertTo-Json -Depth 32 | Set-Content "$fixtures/removed.he" -Encoding UTF8
    Copy-Item "$fixtures/*.he" $work
    # Session starts a long-lived process. Run its PowerShell entry in-process;
    # native redirected pipelines can retain inherited handles until game exit.
    & $session -Action Start -Configuration RelWithDebInfo -ExistingBuildSha256 $sha -StartupScene "$relative/active.he" -OwnerTag $tag *> "$work/start.log"
    $started = $true
    $owner = Get-Content "$repo/out/agent/sessions/active.json" -Raw | ConvertFrom-Json
    if ($owner.ownerTag -ne $tag) { throw 'The ready session has a different owner.' }
    $null = Call-Pc 'api.discover'
    # Paused startup has restored the scene document. Script field snapshots
    # are published by ScriptSystem's completed simulation update.
    $null = Call-Pc 'runtime.step' @{frames=1}
    $first = Read-PcEntity
    Assert-Pc ((Fields-Pc $first).score.value -eq 0 -and (Fields-Pc $first).enableCount.value -eq 1) 'restored-fields-and-initialize'
    $null = Call-Pc 'input.set' @{held=@('d')}
    $null = Call-Pc 'runtime.step' @{frames=20}
    $null = Call-Pc 'input.clear'
    $moved = Read-PcEntity
    Assert-Pc ($moved.components.TransformComponent.Translation[0] -gt $first.components.TransformComponent.Translation[0]) 'keyboard-moves-real-csharp-entity'
    $null = Call-Pc 'input.click' @{x=800;y=360}
    $null = Call-Pc 'runtime.step' @{frames=2}
    $clicked = Read-PcEntity
    Assert-Pc ((Fields-Pc $clicked).score.value -eq 3) 'click-adds-three'
    Assert-Pc ($clicked.components.SpriteRendererComponent.Color[0] -eq 1) 'click-changes-real-sprite-color'
    $save = Join-Path (Split-Path $owner.sessionDirectory) "Saved/Scripting/pc-agent-$tag.txt"
    Assert-Pc ((Get-Content -LiteralPath $save -Raw) -eq '3') 'real-save-file-written'
    Copy-Item -LiteralPath $save -Destination "$work/saved-score.txt"
    $capture = Call-Pc 'capture.frame' @{filename='pc-script-clicked.png'}
    Copy-Item -LiteralPath $capture.path -Destination "$work/clicked.png"
    Load-Pc 'active'
    Assert-Pc ((Fields-Pc (Read-PcEntity)).score.value -eq 3) 'scene-reentry-restores-real-save'
    Load-Pc 'disabled'
    $before = Read-PcEntity
    $null = Call-Pc 'input.set' @{held=@('d')}
    $null = Call-Pc 'runtime.step' @{frames=20}
    $null = Call-Pc 'input.clear'
    Assert-Pc ((Read-PcEntity).components.TransformComponent.Translation[0] -eq $before.components.TransformComponent.Translation[0]) 'disabled-scene-instance-does-not-update'
    $null = Call-Pc 'logs.read' @{limit=100}
    $baseline = Call-Pc 'logs.read' @{limit=100}
    Load-Pc 'fault'
    $null = Call-Pc 'runtime.step' @{frames=3}
    $logs = Call-Pc 'logs.read' @{after=$baseline.nextCursor;limit=100}
    $faults = @($logs.entries | Where-Object {$_.message -match '\[Script\] instance=\d+, kind=\d+:'})
    Assert-Pc ($faults.Count -eq 1 -and $faults[0].message -match 'GameScripts.ScriptTouchSample.Update') 'actual-coreclr-stack-with-one-fault'
    Load-Pc 'removed'
    Assert-Pc (!(Read-PcEntity).found) 'scene-replacement-destroys-script-entity'
    Load-Pc 'active'
    Assert-Pc ((Fields-Pc (Read-PcEntity)).score.value -eq 3) 'responsive-reentry-after-fault-and-destruction'
    $state = 'PASS'
} finally {
    try {
        if (!$started -and (Test-Path "$repo/out/agent/sessions/active.json")) {
            $owner = Get-Content "$repo/out/agent/sessions/active.json" -Raw | ConvertFrom-Json
            $started = $owner.ownerTag -eq $tag
        }
        if ($started) {
            try { & $session -Action Stop -OwnerTag $tag *> "$work/stop.log"; $stopped = $true }
            catch { $state = 'FAIL'; throw }
        }
        # Only this invocation's generated directory, never source sample assets.
        if ([IO.Path]::GetFullPath($fixtures) -ne "$repo\Project\Assets\AgentScriptValidation-$tag") { throw 'Fixture cleanup boundary differs.' }
        if (Test-Path -LiteralPath $fixtures) { Remove-Item -LiteralPath $fixtures -Recurse -Force }
    } finally {
        @{status=$state;evidenceType='runtime-manual-engine-agent';checks=@($checks);executableSha256=$sha;gameplaySourceIdentity=$managed.gameplaySourceIdentity;gameScriptsSha256=$managed.gameScriptsSha256;visualReview='PENDING';limitations='Dev Game with real engine virtual input; not shipping UI or same-instance Inspector toggles.'} | ConvertTo-Json -Depth 6 | Set-Content "$work/result.json"
        Complete-HEStorageRun -Run $record -State $(if ($started -and !$stopped) {'unknown'} elseif ($state -eq 'PASS') {'success'} else {'failed'})
    }
}
Write-Output "PC scripting runtime checks PASS: $work"
