param([Parameter(Mandatory=$true)][string]$RequestFile)
$ErrorActionPreference = 'Stop'
$request = Get-Content -Raw -LiteralPath $RequestFile | ConvertFrom-Json
$hectl = Join-Path $PSScriptRoot '../Agent/hectl.ps1'
$owner = [guid]::NewGuid().ToString('N')
$started = $false
$result = [ordered]@{ok=$false; mode=$request.mode; sceneSha256=$request.sceneSha256; importVersion=$request.importVersion; assertions=@()}
$result.verifierSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$project = [string]$request.project
$scenePath = Join-Path $project $request.scene
$watch = [Diagnostics.Stopwatch]::StartNew()
function Call([string]$Method, $Parameters=@{}) {
    if ($watch.Elapsed.TotalSeconds -gt ([int]$request.timeout + 120)) { throw 'Verification deadline exceeded.' }
    $raw = & $hectl agent call -Project $project -Method $Method -ParamsJson ($Parameters | ConvertTo-Json -Depth 16 -Compress) -AgentOwnerTag $owner -TimeoutSeconds 60
    if ($LASTEXITCODE -ne 0) { throw "Agent method failed: $Method" }
    $response = ($raw -join "`n") | ConvertFrom-Json
    @{method=$Method; parameters=$Parameters; response=$response} | ConvertTo-Json -Depth 24 -Compress | Add-Content -Encoding UTF8 -LiteralPath (Join-Path $request.report 'calls.ndjson')
    if (-not $response.ok -or $response.result.ok -eq $false) { throw "Agent rejected $Method" }
    return $response.result
}
try {
    if ((Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $request.sceneSha256) { throw 'Published scene version changed before verification.' }
    $document = Get-Content -Raw -LiteralPath $scenePath | ConvertFrom-Json
    $baseline = Get-Content -Raw -LiteralPath $request.baselineRecord | ConvertFrom-Json
    $cameras = @($document.PSObject.Properties | Where-Object { $_.Value.CameraComponent.Primary })
    if ($cameras.Count -ne 1) { throw 'Verification requires exactly one authored primary camera.' }
    & $hectl agent start -Project $project -StartupScene $request.scene -AgentOwnerTag $owner -TimeoutSeconds ([Math]::Min(300,[int]$request.timeout)) -Resolution 1280x720 -RenderValidation -RenderStats -RenderStatsPath (Join-Path $request.report 'render-stats.csv') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Agent start failed.' }
    $started = $true
    $status = Call 'session.status'
    $result.session = $status
    $world = Call 'world.summary'
    if (-not $world.available -or $world.scene -ne $request.scene -or $world.sceneGeneration -le 0) { throw 'Ready session does not identify the published scene/generation.' }
    $result.sceneGeneration = $world.sceneGeneration
    $result.startupAndBuildMilliseconds = $watch.ElapsedMilliseconds
    $result.loadMilliseconds = $null
    if ($world.entityCount -ne @($document.PSObject.Properties).Count) { throw 'Loaded entity count differs from published scene.' }
    # Check native load and local fields before any simulation step. No screenshot
    # may substitute for these state assertions.
    $dynamic = @()
    foreach ($entry in $document.PSObject.Properties) {
        $expected = $entry.Value
        $actual = Call 'world.get' @{uuid=$entry.Name; components=@('TransformComponent','MeshComponent','Rigidbody3DComponent')}
        if (-not $actual.found) { throw "Missing loaded entity: $($entry.Name)" }
        $expectedWorld = $baseline.worldMatrices.($entry.Name)
        if ($expectedWorld) {
            if (@($actual.worldMatrix).Count -ne 16) { throw 'Agent must report a complete world matrix.' }
            for ($i=0; $i -lt 16; $i++) {
                if ([Math]::Abs([double]$expectedWorld[$i]-[double]$actual.worldMatrix[$i]) -gt 0.0003 * [Math]::Max(1,[Math]::Abs([double]$expectedWorld[$i]))) { throw "World matrix mismatch: $($entry.Name)" }
            }
        }
        foreach ($field in @('Translation','Rotation','Scale')) {
            $want = $expected.TransformComponent.$field
            $got = $actual.components.TransformComponent.$field
            for ($i=0; $i -lt 3; $i++) {
                if ([Math]::Abs([double]$want[$i]-[double]$got[$i]) -gt 0.0002) { throw "Transform mismatch: $($entry.Name).$field" }
            }
        }
        if ($expected.TransformComponent.ParentUuid -ne $actual.components.TransformComponent.ParentUuid) { throw 'Parent identity mismatch.' }
        if ($expected.MeshComponent -and ([string]$expected.MeshComponent.Path).Replace('\','/') -ine ([string]$actual.components.MeshComponent.Path).Replace('\','/')) {
            throw "Shared geometry path mismatch for $($entry.Name): expected=$($expected.MeshComponent.Path) actual=$($actual.components.MeshComponent.Path)"
        }
        if ($expected.MeshComponent) {
            $wantedMaterials = @($expected.MeshComponent.MaterialOverrides)
            $loadedMaterials = @($actual.components.MeshComponent.MaterialOverrides)
            if ($wantedMaterials.Count -ne $loadedMaterials.Count) { throw 'Material slot count mismatch.' }
            for ($i=0; $i -lt $wantedMaterials.Count; $i++) {
                if (([string]$wantedMaterials[$i]).Replace('\','/') -ine ([string]$loadedMaterials[$i]).Replace('\','/')) { throw 'Instance material binding mismatch.' }
            }
        }
        if ($expected.Rigidbody3DComponent.Type -eq 1) { $dynamic += @{uuid=$entry.Name; initialY=[double]$actual.components.TransformComponent.Translation[1]} }
    }
    $result.assertions += 'native entities, hierarchy, local transforms and shared mesh paths'
    if ($request.mode -eq 'visual') {
        $author = Call 'scene.open' @{path=$request.scene}
        $result.authorPreview = Call 'scene.preview' @{sceneId=$author.sceneId; revision=$author.revision}
        $result.evidence = 'author-preview; physics/scripts not verified'
    }
    else {
        $null = Call 'runtime.step' @{frames=120}
        $null = Call 'runtime.step' @{frames=120}
        $drops = @()
        foreach ($body in $dynamic) {
            $actual = Call 'world.get' @{uuid=$body.uuid; components=@('TransformComponent')}
            $y = [double]$actual.components.TransformComponent.Translation[1]
            if ([double]::IsNaN($y) -or [double]::IsInfinity($y)) { throw 'Nonfinite dynamic body state.' }
            $drops += @{uuid=$body.uuid; initialY=$body.initialY; after240Y=$y}
        }
        $result.dynamicBodies = $drops
        $result.assertions += '240 fixed simulation steps; finite dynamic body state'
        $result.evidence = 'runtime-manual; lifecycle and state assertions; generic sync does not infer collision success from a screenshot'
        if ($request.recipe) {
            if ($request.recipe.schema -ne 1 -or @($request.recipe.phases).Count -gt 32) { throw 'Invalid verification recipe schema/phase limit.' }
            $results = @()
            foreach ($phase in $request.recipe.phases) {
                if ([int]$phase.frames -lt 0 -or [int]$phase.frames -gt 120) { throw 'Recipe phases accept 0..120 simulation frames.' }
                if ($phase.heldKeys) { $null = Call 'input.set' @{held=@($phase.heldKeys)} }
                else { $null = Call 'input.clear' }
                if ([int]$phase.frames -gt 0) { $null = Call 'runtime.step' @{frames=[int]$phase.frames} }
                foreach ($assertion in $phase.assertions) {
                    $matches = @($document.PSObject.Properties | Where-Object { $_.Name -eq $assertion.uuid -or $_.Value.TagComponent.Tag -ceq $assertion.name })
                    if ($matches.Count -ne 1) { throw 'Recipe assertion must resolve exactly one entity UUID or exact name.' }
                    $entity = Call 'world.get' @{uuid=$matches[0].Name; components=@('TransformComponent')}
                    $position = @($entity.worldMatrix[12],$entity.worldMatrix[13],$entity.worldMatrix[14])
                    if (@($assertion.minimum).Count -ne 3 -or @($assertion.maximum).Count -ne 3) { throw 'Recipe assertion requires 3D minimum/maximum world position.' }
                    for ($i=0; $i -lt 3; $i++) {
                        if ($position[$i] -lt $assertion.minimum[$i] -or $position[$i] -gt $assertion.maximum[$i]) { throw "Behavior assertion failed: $($assertion.name), world position $position" }
                    }
                    $results += @{name=$assertion.name; uuid=$matches[0].Name; position=$position; passed=$true}
                }
            }
            $null = Call 'input.clear'
            $result.behaviorAssertions = $results
        }
    }
    $null = Call 'camera.cut'
    $capture = Call 'capture.frame' @{filename='content.png'}
    $screenshot = Join-Path $request.report 'content.png'
    Copy-Item -LiteralPath $capture.path -Destination $screenshot
    $result.screenshot = $screenshot
    $result.renderFrame = $capture.renderFrame
    Add-Type -AssemblyName System.Drawing
    $bitmap = [System.Drawing.Bitmap]::FromFile($screenshot)
    try {
        $nonblack = 0; $samples = 0; $maximum = 0
        for ($y=0; $y -lt $bitmap.Height; $y+=13) {
            for ($x=0; $x -lt $bitmap.Width; $x+=13) {
                $pixel=$bitmap.GetPixel($x,$y); $value=[Math]::Max($pixel.R,[Math]::Max($pixel.G,$pixel.B))
                $samples++; $maximum=[Math]::Max($maximum,$value)
                if ($value -gt 3) { $nonblack++ }
            }
        }
        $result.imageStatistics=@{samples=$samples; nonblack=$nonblack; maximumChannel=$maximum}
        if ($nonblack -eq 0) { throw 'empty_capture: rendered image is black; check camera projection, visibility and exposure.' }
    } finally { $bitmap.Dispose() }
    $result.world = Call 'world.summary'
    if ($result.world.sceneGeneration -ne $world.sceneGeneration) { throw 'Scene generation changed during verification.' }
    if ((Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $request.sceneSha256) { throw 'Published scene changed during verification.' }
    $result.ok = $true
}
catch { $result.error = $_.Exception.Message; $result.stack = $_.ScriptStackTrace }
finally {
    if ($started) {
        try { & $hectl agent stop -Project $project -AgentOwnerTag $owner | Out-Null; if ($LASTEXITCODE -ne 0) { throw 'Agent stop or rendering validation failed.' } }
        catch { $result.ok=$false; $result.stopError=$_.Exception.Message }
    }
    $result.elapsedMilliseconds = $watch.ElapsedMilliseconds
    $result | ConvertTo-Json -Depth 32 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $request.report 'verification.json')
}
if (-not $result.ok) { exit 4 }
