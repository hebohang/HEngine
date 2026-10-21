param([switch]$KeepSessionArtifacts)
$ErrorActionPreference = "Stop"
$repository = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$output = Join-Path $repository ("out/agent/scene-editing/" + (Get-Date -Format "yyyyMMdd-HHmmss"))
$null = New-Item -ItemType Directory -Path $output -Force
$scenePath = "Saved/SceneEditing-$([Guid]::NewGuid().ToString('N')).he"
$sceneFullPath = Join-Path $repository "Project/$scenePath"
$null = New-Item -ItemType Directory -Path (Split-Path $sceneFullPath) -Force
[System.IO.File]::WriteAllText($sceneFullPath, "{}", [System.Text.UTF8Encoding]::new($false))
$script:calls = 0
$script:requestBytes = 0
function Invoke-Scene([string]$Action, [hashtable]$Request) {
    $script:calls++
    $json = $Request | ConvertTo-Json -Depth 32 -Compress
    $script:requestBytes += [System.Text.Encoding]::UTF8.GetByteCount($json)
    $file = Join-Path $output ("request-{0:D2}-{1}.json" -f $script:calls,$Action)
    [System.IO.File]::WriteAllText($file,$json,[System.Text.UTF8Encoding]::new($false))
    $raw = & (Join-Path $repository "Tools/Agent/hectl.ps1") scene $Action -RequestFile $file
    if ($LASTEXITCODE -ne 0) { throw "scene.$Action transport failed: $raw" }
    $response = ($raw -join "`n") | ConvertFrom-Json
    $raw | Set-Content -LiteralPath (Join-Path $output ("receipt-{0:D2}.json" -f $script:calls)) -Encoding UTF8
    if (-not $response.ok -or -not $response.result.ok) { throw "scene.$Action failed: $raw" }
    return $response.result
}
function Write-Request([string]$Action, $State) {
    return Invoke-Scene $Action @{sceneId=$State.sceneId; baseRevision=$State.revision; requestId=[Guid]::NewGuid().ToString('N')}
}
$started = $false
try {
    $startOutput = & (Join-Path $repository "Tools/Agent/hectl.ps1") agent start
    if ($LASTEXITCODE -ne 0) { throw "Agent start failed: $startOutput" }
    $started = $true
    $state = Invoke-Scene open @{path=$scenePath}
    $batch = @{
        sceneId=$state.sceneId; baseRevision=$state.revision; requestId=[Guid]::NewGuid().ToString('N')
        operations=@(
            @{op="create"; temporaryId='$parent'; name="Authoring Parent"},
            @{op="create"; temporaryId='$left'; name="Left Cube"; parent='$parent'; components=@{MeshComponent=@{Path="Assets/Models/Cube/Cube.hmesh"}; TransformComponent=@{Translation=@(-2,0,0)}}},
            @{op="create"; temporaryId='$center'; name="Center Cube"; parent='$parent'; components=@{MeshComponent=@{Path="Assets/Models/Cube/Cube.hmesh"}}},
            @{op="create"; temporaryId='$right'; name="Right Cube"; parent='$parent'; components=@{MeshComponent=@{Path="Assets/Models/Cube/Cube.hmesh"}; TransformComponent=@{Translation=@(2,0,0)}}},
            @{op="create"; name="Camera"; components=@{CameraComponent=@{Camera=@{mProjectionType=0}}; TransformComponent=@{Translation=@(0,1.2,10)}}},
            @{op="create"; name="Light"; components=@{PointLightComponent=@{Intensity=1200}; TransformComponent=@{Translation=@(1,4,4)}}},
            @{op="transform"; entities=@('$left','$center','$right'); patch=@{Scale=@(0.65,0.65,0.65)}},
            @{op="material"; entity='$center'; slot=0; path="Assets/Material/Emissive.hmat"}
        )
    }
    $planned = Invoke-Scene plan $batch
    if ($planned.revision -ne $state.revision) { throw "plan changed revision" }
    $state = Invoke-Scene apply $batch
    $retry = Invoke-Scene apply $batch
    if ($retry.transactionId -ne $state.transactionId) { throw "idempotency retry changed transaction" }
    $query = Invoke-Scene query @{sceneId=$state.sceneId; fields=@("uuid","name","parent","TransformComponent","MeshComponent");limit=32}
    if ($query.entities.Count -ne 6) { throw "entity count mismatch" }
    $null = Invoke-Scene preview @{sceneId=$state.sceneId;revision=$state.revision}
    $captureRaw = & (Join-Path $repository "Tools/Agent/hectl.ps1") agent capture -AgentCaptureName "scene-editing.png"
    if ($LASTEXITCODE -ne 0) { throw "capture failed: $captureRaw" }
    $capture = ($captureRaw -join "`n") | ConvertFrom-Json
    Copy-Item -LiteralPath $capture.result.path -Destination (Join-Path $output "scene-editing.png")
    $state = Write-Request save $state
    if ($state.dirty) { throw "save is dirty" }
    $savedBytes = [System.IO.File]::ReadAllText($sceneFullPath)
    $state = Write-Request undo $state
    if (-not $state.dirty) { throw "undo after save should be dirty" }
    $state = Write-Request redo $state
    if ($state.dirty) { throw "redo should return to saved content" }
    $state = Invoke-Scene open @{path=$scenePath}
    $reloaded = Invoke-Scene query @{sceneId=$state.sceneId; fields=@("uuid","name","parent","TransformComponent","MeshComponent");limit=32}
    if (($query.entities | ConvertTo-Json -Depth 32 -Compress) -ne ($reloaded.entities | ConvertTo-Json -Depth 32 -Compress)) { throw "reload mismatch" }
    $child = @($reloaded.entities | Where-Object name -eq "Center Cube")[0].uuid
    $state = Invoke-Scene apply @{sceneId=$state.sceneId;baseRevision=$state.revision;requestId=[Guid]::NewGuid().ToString('N');operations=@(@{op="rename";entity=$child;name="Changed after reload"})}
    $state = Write-Request undo $state
    if ($state.dirty) { throw "undo after reload must restore saved content" }
    $state = Write-Request redo $state
    if (-not $state.dirty) { throw "redo after reload must be dirty" }
    $state = Write-Request undo $state
    $null = Invoke-Scene validate @{sceneId=$state.sceneId}
    Copy-Item -LiteralPath $sceneFullPath -Destination (Join-Path $output "saved-scene.he")
    $summary = @{ok=$true;calls=$script:calls;requestBytes=$script:requestBytes;sceneBytes=[System.Text.Encoding]::UTF8.GetByteCount($savedBytes);output=$output;scenePath=$scenePath}
    $summary | ConvertTo-Json -Compress | Tee-Object -FilePath (Join-Path $output "summary.json")
}
finally {
    if ($started) { & (Join-Path $repository "Tools/Agent/hectl.ps1") agent stop | Out-Null }
    # Only the uniquely created scratch scene is removed; resources are read-only.
    if (-not $KeepSessionArtifacts -and (Test-Path -LiteralPath $sceneFullPath)) { Remove-Item -LiteralPath $sceneFullPath }
}
