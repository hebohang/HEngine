param(
    [string]$Engine = '',
    [Parameter(Mandatory = $true)][string]$DevelopmentTemplate,
    [string]$EvidenceRoot = ''
)

$ErrorActionPreference = 'Stop'
if (!$Engine) { $Engine = Join-Path $PSScriptRoot '../..' }
$root = (Resolve-Path -LiteralPath $Engine).Path
$playerTemplate = (Resolve-Path -LiteralPath $DevelopmentTemplate).Path
. (Join-Path $root 'Tools/Agent/Project.ps1')
$session = Join-Path $root 'Tools/Agent/Session.ps1'
if (!$EvidenceRoot) { $EvidenceRoot = Join-Path $root 'out/agent/starter-native' }
$runRoot = Join-Path ([IO.Path]::GetFullPath($EvidenceRoot)) ('run-' + [guid]::NewGuid().ToString('N'))
$projectsRoot = Join-Path $runRoot 'projects'
$null = New-Item -ItemType Directory -Path $projectsRoot -Force
$results = [Collections.Generic.List[object]]::new()

function Assert-Native([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}

function Invoke-StarterAgent($Context, [string]$Method, [hashtable]$Parameters = @{}) {
    $LASTEXITCODE = 0
    $json = & $session -Action Call -ProjectRoot $Context.Project -SessionRoot $Context.Sessions `
        -OwnerTag $Context.OwnerTag -Method $Method -ParamsJson ($Parameters | ConvertTo-Json -Depth 8 -Compress) `
        -TimeoutSeconds 180
    if ($LASTEXITCODE -ne 0) { throw "Agent call failed: $Method; response: $json" }
    $response = $json | ConvertFrom-Json
    [ordered]@{method = $Method; parameters = $Parameters; response = $response} |
        ConvertTo-Json -Depth 32 -Compress | Add-Content -LiteralPath $Context.Calls -Encoding UTF8
    Assert-Native ([bool]$response.ok) "Agent rejected $Method"
    return $response.result
}

function Get-StarterEntity($Context, [string]$Uuid, [string[]]$Components = @('TransformComponent')) {
    $entity = Invoke-StarterAgent $Context 'world.get' @{uuid = $Uuid; components = $Components}
    Assert-Native ($entity.available -and $entity.found) "Scene entity is unavailable: $Uuid"
    Assert-Native (@($entity.worldMatrix).Count -eq 16) "Entity has no complete world transform: $Uuid"
    Assert-Native ($null -eq $entity.missingComponents -or @($entity.missingComponents).Count -eq 0) "Entity lacks a requested component: $Uuid"
    return $entity
}

function Step-StarterKeys($Context, [string[]]$Keys, [int]$Frames) {
    # Publish on the next simulation step so edge-triggered Reset is not lost
    # while the paused renderer continues processing mailbox requests.
    $null = Invoke-StarterAgent $Context 'input.set' @{held = @($Keys); onStep = $true}
    $null = Invoke-StarterAgent $Context 'runtime.step' @{frames = $Frames}
    $null = Invoke-StarterAgent $Context 'input.clear'
}

function Assert-StarterPosition($Actual, $Expected, [string]$Message) {
    foreach ($index in 12..14) {
        Assert-Native ([Math]::Abs([double]$Actual.worldMatrix[$index] - [double]$Expected.worldMatrix[$index]) -lt .001) $Message
    }
}

function Save-StarterCapture($Context, [string]$Name) {
    $capture = Invoke-StarterAgent $Context 'capture.frame' @{filename = "$Name.png"}
    $destination = Join-Path $Context.Evidence "$Name.png"
    Copy-Item -LiteralPath $capture.path -Destination $destination
    return [ordered]@{path = $destination; recordedIdentity = $capture.recordedIdentity}
}

foreach ($template in @('FirstPerson', 'TopDown')) {
    $evidence = Join-Path $runRoot $template
    $sessions = Join-Path $evidence 'sessions'
    $null = New-Item -ItemType Directory -Path $sessions -Force
    $context = [pscustomobject]@{
        Project = ''; Sessions = $sessions; Evidence = $evidence
        OwnerTag = [guid]::NewGuid().ToString('N'); Calls = Join-Path $evidence 'agent-calls.jsonl'
    }
    $result = [ordered]@{
        schema = 1; template = $template; pass = $false; evidence = 'test-layer'
        validation = 'Native DX12 WARP scene startup, C# registration, shared resources, HUD, movement, shooting, respawn and reset. Screenshots require separate visual review.'
        developmentTemplate = $playerTemplate; checkpoints = [ordered]@{}
    }
    $failure = $null
    try {
        $project = New-HEProject -RepositoryRoot $root -ProjectPath (Join-Path $projectsRoot $template) `
            -Name "$template Native Validation" -Template $template
        $context.Project = $project.Root
        $result.project = $project.Root
        $scene = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $project.Root 'Assets/Scenes/Main.he') | ConvertFrom-Json
        $player = @($scene.PSObject.Properties | Where-Object { $_.Value.TagComponent.Tag -eq 'Player' })
        $target = @($scene.PSObject.Properties | Where-Object { $_.Value.TagComponent.Tag -eq 'Target2' })
        Assert-Native ($player.Count -eq 1 -and $target.Count -eq 1) "$template must author one Player and Target2"

        $LASTEXITCODE = 0
        & $session -Action Start -ProjectRoot $project.Root -SessionRoot $sessions -OwnerTag $context.OwnerTag `
            -DevelopmentTemplate $playerTemplate -BuildProfile windows -Resolution 960x540 `
            -FixedDeltaSeconds 0.0166667 -UseWarp -GraphicsApi DX12 -StrictGraphicsApi `
            -TimeoutSeconds 180 *> (Join-Path $evidence 'start.log')
        if ($LASTEXITCODE -ne 0) { throw "Native startup failed: $evidence/start.log" }
        $owner = Get-Content -Raw -LiteralPath (Join-Path $sessions 'active.json') | ConvertFrom-Json
        $result.nativeSha256 = $owner.artifactSha256
        $result.startup = Invoke-StarterAgent $context 'session.status'
        Assert-Native ($result.startup.world.available -and $result.startup.world.runtimeControl.paused) 'Startup did not publish a paused runtime scene'
        $entities = Invoke-StarterAgent $context 'world.list' @{limit = 100}
        foreach ($authored in $scene.PSObject.Properties) {
            Assert-Native ($authored.Name -in @($entities.entities.uuid)) "Authored entity did not load: $($authored.Value.TagComponent.Tag)"
        }
        $initial = Get-StarterEntity $context $player[0].Name @('TransformComponent', 'ScriptComponent')
        Assert-Native ($initial.components.ScriptComponent.Backend -eq 'he.script.csharp' -and
            $initial.components.ScriptComponent.Type -eq $player[0].Value.ScriptComponent.Type -and
            $initial.components.ScriptComponent.Enabled) 'Player C# script binding did not survive native scene loading'
        $targetInitial = Get-StarterEntity $context $target[0].Name
        $result.checkpoints.playerInitial = $initial
        $result.checkpoints.targetInitial = $targetInitial
        $null = Invoke-StarterAgent $context 'runtime.step' @{frames = 2}
        $hud = Invoke-StarterAgent $context 'ui.element.rect' @{id = 'score'}
        Assert-Native ([bool]$hud.visible) 'The native C# controller did not load its score HUD'
        $title = Invoke-StarterAgent $context 'ui.element.rect' @{id = 'title'}
        Assert-Native ([bool]$title.visible) 'The native starter HUD did not show its title'
        Assert-Native ([double]$hud.rect[1] -ge ([double]$title.rect[1] + [double]$title.rect[3])) 'The score HUD must appear below the title instead of running together on one line'
        $result.initialCapture = Save-StarterCapture $context 'initial'

        Step-StarterKeys $context @('space') 1
        $hit = Get-StarterEntity $context $target[0].Name
        Assert-Native ([Math]::Abs([double]$hit.worldMatrix[13] + 10) -lt .001) 'Space did not hit and hide the center target from the authored spawn'
        $result.checkpoints.targetHit = $hit
        $result.hitCapture = Save-StarterCapture $context 'target-hit'
        # runtime.step caps each request at 120 frames. Cross the two-second
        # respawn boundary using two bounded calls and no held Fire input.
        $null = Invoke-StarterAgent $context 'runtime.step' @{frames = 120}
        $null = Invoke-StarterAgent $context 'runtime.step' @{frames = 2}
        $respawned = Get-StarterEntity $context $target[0].Name
        Assert-StarterPosition $respawned $targetInitial 'Target did not respawn at its authored position'
        $result.checkpoints.targetRespawned = $respawned

        Step-StarterKeys $context @('d') 20
        $moved = Get-StarterEntity $context $player[0].Name
        $distance = [double]$moved.worldMatrix[12] - [double]$initial.worldMatrix[12]
        Assert-Native ($distance -gt 1 -and $distance -lt 2) 'D input did not move the actual native player by the expected bounded distance'
        $result.checkpoints.playerMoved = $moved
        Step-StarterKeys $context @('r') 1
        $reset = Get-StarterEntity $context $player[0].Name
        Assert-StarterPosition $reset $initial 'Reset did not restore the native player spawn'
        $result.checkpoints.playerReset = $reset
        Step-StarterKeys $context @() 1
        Step-StarterKeys $context @('space') 1
        $hitAgain = Get-StarterEntity $context $target[0].Name
        Assert-Native ([Math]::Abs([double]$hitAgain.worldMatrix[13] + 10) -lt .001) 'Shooting after player reset did not hit Target2'
        Step-StarterKeys $context @('r') 1
        $targetReset = Get-StarterEntity $context $target[0].Name
        Assert-StarterPosition $targetReset $targetInitial 'Reset did not immediately restore the hit target'
        $result.checkpoints.targetReset = $targetReset
        $result.finalStatus = Invoke-StarterAgent $context 'session.status'
        $after = 0
        for ($page = 0; $page -lt 20; ++$page) {
            $logs = Invoke-StarterAgent $context 'logs.read' @{after = $after; limit = 100}
            $errors = @($logs.entries | Where-Object { $_.level -in @('error', 'critical') })
            Assert-Native ($errors.Count -eq 0) ("Native runtime reported errors: " + ($errors.message -join '; '))
            if (@($logs.entries).Count -lt 100) { break }
            $after = $logs.nextCursor
        }
    }
    catch {
        $failure = $_
        $result.error = $_.Exception.Message
    }
    finally {
        if ($context.Project -and (Test-Path -LiteralPath (Join-Path $sessions 'active.json'))) {
            try {
                $LASTEXITCODE = 0
                & $session -Action Stop -ProjectRoot $context.Project -SessionRoot $sessions `
                    -OwnerTag $context.OwnerTag *> (Join-Path $evidence 'stop.log')
                if ($LASTEXITCODE -ne 0) { throw "Native session cleanup failed: $evidence/stop.log" }
                $result.cleanup = 'stopped'
                $owner = Get-Content -Raw -LiteralPath (Join-Path $sessions 'active.json') | ConvertFrom-Json
                $errorLines = @(Select-String -LiteralPath @($owner.stdoutLog, $owner.stderrLog) -Pattern '\[(error|critical)\]')
                if ($errorLines.Count) { throw "Native engine logged errors: $($errorLines.Line -join '; ')" }
            }
            catch {
                $result.cleanupError = $_.Exception.Message
                if (!$failure) { $failure = $_ }
            }
        }
        $result.pass = $null -eq $failure
        $result | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath (Join-Path $evidence 'result.json') -Encoding UTF8
        $results.Add([pscustomobject]$result)
    }
}

$failed = @($results | Where-Object { !$_.pass })
[ordered]@{schema = 1; pass = $failed.Count -eq 0; evidence = 'test-layer'; templates = @($results.ToArray())} |
    ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $runRoot 'result.json') -Encoding UTF8
if ($failed.Count) {
    throw "Native starter validation failed for $($failed.template -join ', '); evidence: $runRoot"
}
Write-Output "PASS: native FirstPerson and TopDown startup, HUD, movement, hits, respawn and reset. Evidence: $runRoot"
