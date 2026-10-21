param(
    [ValidateSet("before", "after", "debug")]
    [string]$Label = "after",

    [ValidateSet("Forward", "Backward", "Lateral", "Stationary", "Static")]
    [string[]]$Sequences = @("Forward", "Lateral"),

    [ValidateSet(
        "Disabled",
        "BaseColor",
        "WorldNormal",
        "Metallic",
        "Roughness",
        "AmbientOcclusion",
        "DirectLighting",
        "DiffuseIbl",
        "SpecularIbl",
        "Shadow",
        "FinalHdr",
        "HdrValidity")]
    [string]$PbrDebugMode = "Disabled",

    [switch]$CascadeDebug,

    [ValidateSet("Debug", "RelWithDebInfo")]
    [string]$Configuration = "RelWithDebInfo",

    [ValidatePattern("^[1-9][0-9]{0,3}[xX][1-9][0-9]{0,3}$")]
    [string]$Resolution = "1280x720",

    [ValidateRange(0.000001, 1.0)]
    [double]$FixedDeltaSeconds = 0.016666666666666666,

    [ValidateRange(1, 120)]
    [int]$StepFrames = 30,

    [ValidateRange(1, 240)]
    [int]$StepCount = 5,

    [ValidateRange(30, 300)]
    [int]$TimeoutSeconds = 180,

    [string]$OutputRoot = "",

    [switch]$RenderValidation,

    [switch]$RenderStats,

    [switch]$Overwrite,

    [switch]$IUnderstandThisTemporarilyChangesGameConfig
)

$ErrorActionPreference = "Stop"

function Get-BytesSha256 {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha256.ComputeHash($Bytes) | ForEach-Object { $_.ToString("x2") }) -join "")
    }
    finally {
        $sha256.Dispose()
    }
}

function Get-RepositoryRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RepositoryRoot
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRoot = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    if ([string]::Equals(
            $fullPath,
            $fullRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        return "."
    }

    $rootPrefix = $fullRoot + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Evidence path escaped the repository: $fullPath"
    }
    return $fullPath.Substring($rootPrefix.Length).Replace('\', '/')
}

function New-FileEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RepositoryRoot
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Evidence file is missing: $fullPath"
    }
    $file = Get-Item -LiteralPath $fullPath
    return [ordered]@{
        path = Get-RepositoryRelativePath -Path $fullPath -RepositoryRoot $RepositoryRoot
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $fullPath).Hash
        bytes = [long]$file.Length
    }
}

function Get-GitHead {
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

    $gitCommand = Get-Command git.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $gitCommand) {
        $gitCommand = Get-Command git -CommandType Application -ErrorAction Stop |
            Select-Object -First 1
    }
    $output = @(& $gitCommand.Source -C $RepositoryRoot rev-parse --verify HEAD)
    if ($LASTEXITCODE -ne 0 -or $output.Count -ne 1) {
        throw "Failed to resolve the repository Git HEAD."
    }
    $head = ([string]$output[0]).Trim()
    if ($head -notmatch '^[0-9a-fA-F]{40,64}$') {
        throw "Git returned an invalid HEAD object id: $head"
    }
    return $head.ToLowerInvariant()
}

function Get-CoreFileEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string[]]$RelativePaths
    )

    $evidence = [System.Collections.Generic.List[object]]::new()
    foreach ($relativePath in $RelativePaths) {
        $evidence.Add((New-FileEvidence `
            -Path (Join-Path $RepositoryRoot $relativePath) `
            -RepositoryRoot $RepositoryRoot))
    }
    return @($evidence)
}

function Assert-FileEvidenceUnchanged {
    param(
        [Parameter(Mandatory = $true)][object[]]$Before,
        [Parameter(Mandatory = $true)][object[]]$After
    )

    if ($Before.Count -ne $After.Count) {
        throw "The core evidence file set changed during capture."
    }
    for ($index = 0; $index -lt $Before.Count; ++$index) {
        if (-not [string]::Equals(
                [string]$Before[$index].path,
                [string]$After[$index].path,
                [StringComparison]::Ordinal) -or
            -not [string]::Equals(
                [string]$Before[$index].sha256,
                [string]$After[$index].sha256,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Core evidence changed during capture: $($Before[$index].path)"
        }
    }
}

function Get-DirectChildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$FileName
    )

    if ([string]::IsNullOrWhiteSpace($FileName) -or
        -not [string]::Equals(
            [System.IO.Path]::GetFileName($FileName),
            $FileName,
            [StringComparison]::Ordinal)) {
        throw "Expected a direct-child file name, got: $FileName"
    }
    $fullDirectory = [System.IO.Path]::GetFullPath($Directory).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $child = [System.IO.Path]::GetFullPath((Join-Path $fullDirectory $FileName))
    $expectedPrefix = $fullDirectory + [System.IO.Path]::DirectorySeparatorChar
    if (-not $child.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Direct-child path escaped its directory: $child"
    }
    return $child
}

function Remove-FlatDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $directory = Get-Item -LiteralPath $Path
    if (-not $directory.PSIsContainer -or
        ($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to clean a non-directory or reparse point: $Path"
    }
    $children = @(Get-ChildItem -LiteralPath $Path -Force)
    $nonFiles = @($children | Where-Object {
        $_.PSIsContainer -or ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
    })
    if ($nonFiles.Count -gt 0) {
        throw "Refusing to clean a non-flat staging directory: $Path"
    }
    foreach ($child in $children) {
        Remove-Item -LiteralPath $child.FullName -Force -ErrorAction Stop
    }
    Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
}

function Publish-StagedArtifacts {
    param(
        [Parameter(Mandatory = $true)][string]$StagingDirectory,
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [Parameter(Mandatory = $true)][string[]]$ArtifactNames,
        [Parameter(Mandatory = $true)][bool]$AllowOverwrite
    )

    $stagingDirectory = [System.IO.Path]::GetFullPath($StagingDirectory)
    $outputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
    $stagingParent = [System.IO.Path]::GetDirectoryName($stagingDirectory)
    $outputParent = [System.IO.Path]::GetDirectoryName($outputDirectory)
    if (-not [string]::Equals(
            $stagingParent,
            $outputParent,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Staging and output directories must share a parent for transactional publish."
    }

    $stagingInfo = Get-Item -LiteralPath $stagingDirectory
    if (-not $stagingInfo.PSIsContainer -or
        ($stagingInfo.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to publish a non-directory or reparse point: $stagingDirectory"
    }

    $seenNames = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $ArtifactNames) {
        if (-not $seenNames.Add($name)) {
            throw "Duplicate staged artifact name: $name"
        }
        $stagedPath = Get-DirectChildPath -Directory $StagingDirectory -FileName $name
        if (-not (Test-Path -LiteralPath $stagedPath -PathType Leaf)) {
            throw "Staged capture artifact is missing: $stagedPath"
        }
    }
    $stagedChildren = @(Get-ChildItem -LiteralPath $stagingDirectory -Force)
    $unexpectedStagedChildren = @($stagedChildren | Where-Object {
        $_.PSIsContainer -or
        ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
        -not $seenNames.Contains($_.Name)
    })
    if ($unexpectedStagedChildren.Count -gt 0 -or
        $stagedChildren.Count -ne $ArtifactNames.Count) {
        throw "The staging directory does not exactly match the expected artifact set."
    }

    $manifestSuffix = "-manifest.json"
    $manifestName = $ArtifactNames[$ArtifactNames.Count - 1]
    if (-not $manifestName.EndsWith(
            $manifestSuffix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "The manifest must be the final staged artifact."
    }
    $artifactStem = $manifestName.Substring(
        0, $manifestName.Length - $manifestSuffix.Length)

    $bOutputExists = Test-Path -LiteralPath $outputDirectory
    if ($bOutputExists) {
        $outputInfo = Get-Item -LiteralPath $outputDirectory
        if (-not $outputInfo.PSIsContainer -or
            ($outputInfo.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to replace a non-directory or reparse point: $outputDirectory"
        }
        $existingChildren = @(Get-ChildItem -LiteralPath $outputDirectory -Force)
        $unsafeChildren = @($existingChildren | Where-Object {
            $_.PSIsContainer -or
            ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
            -not $_.Name.StartsWith(
                "$artifactStem-", [StringComparison]::OrdinalIgnoreCase) -or
            $_.Extension -notin @(".png", ".log", ".json")
        })
        if ($unsafeChildren.Count -gt 0) {
            throw "Refusing to replace an output directory containing non-capture files: $outputDirectory"
        }
        if ($existingChildren.Count -gt 0 -and -not $AllowOverwrite) {
            throw "Capture output directory is not empty; pass -Overwrite to replace it: $outputDirectory"
        }
    }

    $rollbackDirectory = Join-Path $outputParent `
        (".{0}.rollback.{1}" -f `
            [System.IO.Path]::GetFileName($outputDirectory),
            [guid]::NewGuid().ToString("N"))
    $bBackedUp = $false
    $bPublished = $false
    $publishError = $null
    try {
        if ($bOutputExists) {
            Move-Item -LiteralPath $outputDirectory `
                -Destination $rollbackDirectory -ErrorAction Stop
            $bBackedUp = $true
        }
        Move-Item -LiteralPath $stagingDirectory `
            -Destination $outputDirectory -ErrorAction Stop
        $bPublished = $true
    }
    catch {
        $publishError = $_
    }

    if ($null -ne $publishError) {
        $rollbackErrors = [System.Collections.Generic.List[string]]::new()
        if ($bPublished -and (Test-Path -LiteralPath $outputDirectory)) {
            try {
                Move-Item -LiteralPath $outputDirectory `
                    -Destination $stagingDirectory -ErrorAction Stop
            }
            catch {
                $rollbackErrors.Add("Failed to move the new artifact set back to staging.")
            }
        }
        if ($bBackedUp -and
            (Test-Path -LiteralPath $rollbackDirectory) -and
            -not (Test-Path -LiteralPath $outputDirectory)) {
            try {
                Move-Item -LiteralPath $rollbackDirectory `
                    -Destination $outputDirectory -ErrorAction Stop
            }
            catch {
                $rollbackErrors.Add("Failed to restore the previous output directory.")
            }
        }
        if ($rollbackErrors.Count -gt 0) {
            throw "$($publishError.Exception.Message)$([Environment]::NewLine)Publish rollback failures:$([Environment]::NewLine)$($rollbackErrors -join [Environment]::NewLine)"
        }
        throw $publishError
    }

    if ($bBackedUp) {
        Remove-FlatDirectory -Path $rollbackDirectory
    }
}

function Write-BytesAtomically {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][byte[]]$Bytes
    )

    $directory = [System.IO.Path]::GetDirectoryName($Path)
    $temporaryPath = Join-Path $directory `
        (".{0}.{1}.tmp" -f [System.IO.Path]::GetFileName($Path), [guid]::NewGuid().ToString("N"))
    $replacementBackupPath = "$temporaryPath.replaced"
    try {
        [System.IO.File]::WriteAllBytes($temporaryPath, $Bytes)
        if ([System.IO.File]::Exists($Path)) {
            [System.IO.File]::Replace($temporaryPath, $Path, $replacementBackupPath)
            Remove-Item -LiteralPath $replacementBackupPath -Force -ErrorAction Stop
        }
        else {
            [System.IO.File]::Move($temporaryPath, $Path)
        }
    }
    finally {
        if ([System.IO.File]::Exists($temporaryPath)) {
            Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction Stop
        }
        if ([System.IO.File]::Exists($replacementBackupPath)) {
            Remove-Item -LiteralPath $replacementBackupPath -Force -ErrorAction Stop
        }
    }
}

function Write-TextAtomically {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    Write-BytesAtomically -Path $Path -Bytes ([Text.UTF8Encoding]::new($false).GetBytes($Text))
}

function Restore-PendingGameConfig {
    param(
        [Parameter(Mandatory = $true)][string]$GameConfigPath,
        [Parameter(Mandatory = $true)][string]$BackupPath,
        [Parameter(Mandatory = $true)][string]$MarkerPath
    )

    if (-not (Test-Path -LiteralPath $MarkerPath -PathType Leaf)) {
        return
    }
    if (-not (Test-Path -LiteralPath $BackupPath -PathType Leaf)) {
        throw "PBR capture recovery marker exists without Game.json backup: $MarkerPath"
    }
    if (-not (Test-Path -LiteralPath $GameConfigPath -PathType Leaf)) {
        throw "PBR capture recovery cannot find Game.json: $GameConfigPath"
    }

    $marker = Get-Content -Raw -Encoding UTF8 -LiteralPath $MarkerPath | ConvertFrom-Json
    $backupBytes = [System.IO.File]::ReadAllBytes($BackupPath)
    $backupHash = Get-BytesSha256 -Bytes $backupBytes
    $originalHash = [string]$marker.originalSha256
    if ([string]::IsNullOrWhiteSpace($originalHash)) {
        # Schema 1 markers did not persist this field; the backup is authoritative.
        $originalHash = $backupHash
    }
    if (-not [string]::Equals(
            $backupHash,
            $originalHash,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "PBR capture Game.json backup hash does not match its recovery marker."
    }

    $currentHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $GameConfigPath).Hash
    if ([string]::Equals(
            $currentHash,
            [string]$marker.temporarySha256,
            [StringComparison]::OrdinalIgnoreCase)) {
        Write-BytesAtomically -Path $GameConfigPath -Bytes $backupBytes
    }
    elseif (-not [string]::Equals(
            $currentHash,
            $originalHash,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Game.json changed after a PBR capture transaction; backup preserved at $BackupPath"
    }

    Remove-Item -LiteralPath $MarkerPath -Force -ErrorAction Stop
    Remove-Item -LiteralPath $BackupPath -Force -ErrorAction Stop
}

function Invoke-Hectl {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [switch]$Json
    )

    # Invoking the batch file through a native PowerShell pipeline can retain a
    # stdout pipe inherited by the Agent-owned game and block until the game
    # exits. Redirect the short-lived hectl process to files and wait only for
    # that exact process handle, never for its detached game descendant.
    $callId = [guid]::NewGuid().ToString("N")
    $stdoutPath = Join-Path $script:stateDirectory "hectl-$callId.stdout.tmp"
    $stderrPath = Join-Path $script:stateDirectory "hectl-$callId.stderr.tmp"
    try {
        $process = Start-Process -FilePath $script:hectlPath -ArgumentList $Arguments `
            -WorkingDirectory $script:repositoryRoot -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        $stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
            @(Get-Content -Encoding UTF8 -LiteralPath $stdoutPath)
        }
        else {
            @()
        }
        $stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
            @(Get-Content -Encoding UTF8 -LiteralPath $stderrPath)
        }
        else {
            @()
        }
        $output = @($stdout) + @($stderr)
        if ($exitCode -ne 0) {
            throw "hectl failed (exit=$exitCode): $($Arguments -join ' ')$([Environment]::NewLine)$($output -join [Environment]::NewLine)"
        }
        if (-not $Json) {
            $output | Write-Host
            return
        }

        $jsonLine = $stdout |
            ForEach-Object { [string]$_ } |
            Where-Object { $_.TrimStart().StartsWith("{", [StringComparison]::Ordinal) } |
            Select-Object -Last 1
        if ([string]::IsNullOrWhiteSpace($jsonLine)) {
            throw "hectl returned no JSON object: $($Arguments -join ' ')"
        }
        return ($jsonLine | ConvertFrom-Json)
    }
    finally {
        foreach ($temporaryOutput in @($stdoutPath, $stderrPath)) {
            if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf) {
                try {
                    Remove-Item -LiteralPath $temporaryOutput -Force -ErrorAction Stop
                }
                catch {
                    # agent start can leave this redirection handle inherited by
                    # the owned game until session.stop. Defer exact-file cleanup.
                    $script:deferredHectlOutputs.Add($temporaryOutput)
                }
            }
        }
    }
}

function Get-SessionStatus {
    param([Parameter(Mandatory = $true)][string]$OwnerTag)

    $response = Invoke-Hectl -Json -Arguments @(
        "agent", "call",
        "-Method", "session.status",
        "-ParamsJson", "{}",
        "-AgentOwnerTag", $OwnerTag,
        "-TimeoutSeconds", $TimeoutSeconds)
    if (-not [bool]$response.ok) {
        throw "session.status failed: $($response | ConvertTo-Json -Depth 16 -Compress)"
    }
    return $response.result
}

function Read-OwnedActiveSession {
    param([Parameter(Mandatory = $true)][string]$OwnerTag)

    if (-not (Test-Path -LiteralPath $script:activeSessionPath -PathType Leaf)) {
        return $null
    }
    $owner = Get-Content -Raw -Encoding UTF8 -LiteralPath $script:activeSessionPath |
        ConvertFrom-Json
    if ([string]::Equals(
            [string]$owner.ownerTag,
            $OwnerTag,
            [StringComparison]::OrdinalIgnoreCase)) {
        return $owner
    }
    return $null
}

function Add-CaptureRecord {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[object]]$Records,
        [Parameter(Mandatory = $true)]
        [string]$OwnerTag,
        [Parameter(Mandatory = $true)]
        [string]$Sequence,
        [Parameter(Mandatory = $true)]
        [long]$RequestedCompletedSteps,
        [Parameter(Mandatory = $true)]
        $Status
    )

    $runtimeControl = $Status.world.runtimeControl
    if ($null -eq $runtimeControl) {
        throw "session.status did not contain world.runtimeControl."
    }
    $actualCompletedSteps = [long]$runtimeControl.completedSteps
    if ($actualCompletedSteps -ne $RequestedCompletedSteps) {
        throw "Capture barrier mismatch: requested completedSteps=$RequestedCompletedSteps, actual=$actualCompletedSteps"
    }

    $frameStem = "{0}-{1}-f{2:D3}" -f `
        $script:artifactLabel, $Sequence.ToLowerInvariant(), $RequestedCompletedSteps
    $capture = Invoke-Hectl -Json -Arguments @(
        "agent", "capture",
        "-AgentCaptureName", "$frameStem.png",
        "-AgentOwnerTag", $OwnerTag,
        "-TimeoutSeconds", $TimeoutSeconds)
    if (-not [bool]$capture.ok) {
        throw "capture.frame failed: $($capture | ConvertTo-Json -Depth 16 -Compress)"
    }

    $sourcePath = [System.IO.Path]::GetFullPath([string]$capture.result.path)
    $destinationPath = Join-Path $script:outputDirectory "$frameStem.png"
    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourcePath).Hash
    $destinationHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destinationPath).Hash
    if (-not [string]::Equals(
            $sourceHash,
            $destinationHash,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Copied capture hash mismatch: $destinationPath"
    }
    $publishedPath = Join-Path $script:publishedOutputDirectory "$frameStem.png"

    $Records.Add([ordered]@{
        sequence = $Sequence
        requestedCompletedSteps = $RequestedCompletedSteps
        completedSteps = $actualCompletedSteps
        simulationTimeSeconds = [double]$Status.simulationTimeSeconds
        renderFrame = [long]$capture.result.renderFrame
        sourcePath = Get-RepositoryRelativePath `
            -Path $sourcePath -RepositoryRoot $script:repositoryRoot
        outputPath = Get-RepositoryRelativePath `
            -Path $publishedPath -RepositoryRoot $script:repositoryRoot
        sha256 = $destinationHash
        bytes = [long](Get-Item -LiteralPath $destinationPath).Length
    })
}

function Invoke-CaptureSequence {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("Forward", "Backward", "Lateral", "Stationary", "Static")]
        [string]$Sequence,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[object]]$Records,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[object]]$SequenceRecords
    )

    $ownerTag = [guid]::NewGuid().ToString("N")
    $owner = $null
    $executableEvidence = $null
    $bStarted = $false
    $sequenceError = $null
    $cleanupErrors = [System.Collections.Generic.List[string]]::new()
    try {
        $startArguments = @(
            "agent", "start",
            "-Configuration", $Configuration,
            "-FixedDeltaSeconds", $FixedDeltaSeconds.ToString(
                "R", [Globalization.CultureInfo]::InvariantCulture),
            "-Resolution", $Resolution,
            "-TimeoutSeconds", $TimeoutSeconds,
            "-AgentOwnerTag", $ownerTag)
        if ($RenderValidation) {
            $startArguments += "-RenderValidation"
        }
        if ($RenderStats) {
            $startArguments += "-RenderStats"
        }
        Invoke-Hectl -Arguments $startArguments
        $bStarted = $true

        $owner = Read-OwnedActiveSession -OwnerTag $ownerTag
        if ($null -eq $owner) {
            throw "The active Agent session is not owned by this capture sequence."
        }
        $executableEvidence = New-FileEvidence `
            -Path ([string]$owner.artifact) `
            -RepositoryRoot $script:repositoryRoot

        $status = Get-SessionStatus -OwnerTag $ownerTag
        Add-CaptureRecord -Records $Records -OwnerTag $ownerTag -Sequence $Sequence `
            -RequestedCompletedSteps 0 -Status $status

        if ($Sequence -ne "Static") {
            if ($Sequence -ne "Stationary") {
                $heldKey = switch ($Sequence) {
                    "Forward" { "w" }
                    "Backward" { "s" }
                    "Lateral" { "d" }
                }
                $input = Invoke-Hectl -Json -Arguments @(
                    "agent", "input",
                    "-AgentHeldKeys", $heldKey,
                    "-AgentOwnerTag", $ownerTag,
                    "-TimeoutSeconds", $TimeoutSeconds)
                if (-not [bool]$input.ok) {
                    throw "input.set failed: $($input | ConvertTo-Json -Depth 16 -Compress)"
                }
            }

            for ($stepIndex = 1; $stepIndex -le $StepCount; ++$stepIndex) {
                $step = Invoke-Hectl -Json -Arguments @(
                    "agent", "step",
                    "-AgentStepFrames", $StepFrames,
                    "-AgentOwnerTag", $ownerTag,
                    "-TimeoutSeconds", $TimeoutSeconds)
                if (-not [bool]$step.ok) {
                    throw "runtime.step failed: $($step | ConvertTo-Json -Depth 16 -Compress)"
                }
                $expectedSteps = [long]$StepFrames * $stepIndex
                $targetSteps = [long]$step.result.targetCompletedSteps
                if ($targetSteps -ne $expectedSteps) {
                    throw "runtime.step target mismatch: expected=$expectedSteps, actual=$targetSteps"
                }
                Add-CaptureRecord -Records $Records -OwnerTag $ownerTag -Sequence $Sequence `
                    -RequestedCompletedSteps $targetSteps -Status $step.result.status
            }
        }
    }
    catch {
        $sequenceError = $_
    }
    finally {
        if ($null -eq $owner) {
            try {
                $owner = Read-OwnedActiveSession -OwnerTag $ownerTag
            }
            catch {
                $cleanupErrors.Add("Failed to inspect the active Agent session: $($_.Exception.Message)")
            }
        }

        if ($bStarted -or $null -ne $owner) {
            try {
                $null = Invoke-Hectl -Json -Arguments @(
                    "agent", "clear-input",
                    "-AgentOwnerTag", $ownerTag,
                    "-TimeoutSeconds", $TimeoutSeconds)
            }
            catch {
                Write-Warning "Failed to clear Agent input: $($_.Exception.Message)"
            }
            try {
                Invoke-Hectl -Arguments @(
                    "agent", "stop",
                    "-AgentOwnerTag", $ownerTag,
                    "-TimeoutSeconds", $TimeoutSeconds)
            }
            catch {
                $cleanupErrors.Add("Failed to stop Agent session: $($_.Exception.Message)")
            }
        }

        if ($null -ne $owner) {
            $logEvidence = @{}
            foreach ($log in @("stdoutLog", "stderrLog")) {
                $sourceLog = [string]$owner.$log
                $logKind = if ($log -eq "stdoutLog") { "stdout" } else { "stderr" }
                $logFileName = "{0}-{1}.{2}.log" -f `
                    $script:artifactLabel, $Sequence.ToLowerInvariant(), $logKind
                $destinationLog = Join-Path $script:outputDirectory `
                    $logFileName
                try {
                    if (-not (Test-Path -LiteralPath $sourceLog -PathType Leaf)) {
                        throw "Session log is missing: $sourceLog"
                    }
                    Copy-Item -LiteralPath $sourceLog -Destination $destinationLog -Force
                    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceLog).Hash
                    $destinationHash = (Get-FileHash `
                        -Algorithm SHA256 -LiteralPath $destinationLog).Hash
                    if (-not [string]::Equals(
                            $sourceHash,
                            $destinationHash,
                            [StringComparison]::OrdinalIgnoreCase)) {
                        throw "Copied session log hash mismatch: $destinationLog"
                    }
                    $publishedLog = Join-Path `
                        $script:publishedOutputDirectory $logFileName
                    $logEvidence[$logKind] = [ordered]@{
                        sourcePath = Get-RepositoryRelativePath `
                            -Path $sourceLog -RepositoryRoot $script:repositoryRoot
                        outputPath = Get-RepositoryRelativePath `
                            -Path $publishedLog -RepositoryRoot $script:repositoryRoot
                        sha256 = $destinationHash
                        bytes = [long](Get-Item -LiteralPath $destinationLog).Length
                    }
                }
                catch {
                    $cleanupErrors.Add("Failed to preserve $logKind log: $($_.Exception.Message)")
                }
            }

            try {
                if ($null -eq $executableEvidence) {
                    throw "The launched executable was not recorded."
                }
                $executableAfter = New-FileEvidence `
                    -Path ([string]$owner.artifact) `
                    -RepositoryRoot $script:repositoryRoot
                if (-not [string]::Equals(
                        [string]$executableEvidence.path,
                        [string]$executableAfter.path,
                        [StringComparison]::Ordinal) -or
                    -not [string]::Equals(
                        [string]$executableEvidence.sha256,
                        [string]$executableAfter.sha256,
                        [StringComparison]::OrdinalIgnoreCase)) {
                    throw "The launched executable changed during capture."
                }
                if (-not $logEvidence.ContainsKey("stdout") -or
                    -not $logEvidence.ContainsKey("stderr")) {
                    throw "The session log evidence is incomplete."
                }
                $SequenceRecords.Add([ordered]@{
                    sequence = $Sequence
                    executable = $executableAfter
                    stdoutLog = $logEvidence["stdout"]
                    stderrLog = $logEvidence["stderr"]
                })
            }
            catch {
                $cleanupErrors.Add("Failed to record sequence evidence: $($_.Exception.Message)")
            }
        }
    }

    if ($null -ne $sequenceError) {
        if ($cleanupErrors.Count -gt 0) {
            throw "$($sequenceError.Exception.Message)$([Environment]::NewLine)Cleanup failures:$([Environment]::NewLine)$($cleanupErrors -join [Environment]::NewLine)"
        }
        throw $sequenceError
    }
    if ($cleanupErrors.Count -gt 0) {
        throw "PBR capture cleanup failed:$([Environment]::NewLine)$($cleanupErrors -join [Environment]::NewLine)"
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$gameConfigPath = [System.IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "Project\Config\Game.json"))
$expectedConfigPath = Join-Path $repositoryRoot "Project\Config\Game.json"
$pbrScenePath = Join-Path $repositoryRoot "Project\Assets\Scenes\PBRTest.he"
$hectlPath = Join-Path $repositoryRoot "hectl.bat"
$activeSessionPath = Join-Path $repositoryRoot "out\agent\sessions\active.json"
$stateDirectory = Join-Path $repositoryRoot "out\agent\pbr-shadow-capture"
$backupPath = Join-Path $stateDirectory "Game.json.backup"
$markerPath = Join-Path $stateDirectory "active.json"
$lockPath = Join-Path $stateDirectory "capture.lock"
$deferredHectlOutputs = [System.Collections.Generic.List[string]]::new()
$stagingDirectory = $null

if (-not [string]::Equals(
        $gameConfigPath,
        [System.IO.Path]::GetFullPath($expectedConfigPath),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Resolved Game.json escaped the repository."
}

New-Item -ItemType Directory -Force -Path $stateDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageReceipt = Join-Path $repositoryRoot ('out/agent/captures/pbr-workflow-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $storageReceipt | Out-Null
$storageRun = Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'pbr-shadow' -Paths @($storageReceipt) -Locks @($lockPath) -Outer
$storageState = 'failed'
$stateLock = $null
try {
    try {
        $stateLock = [System.IO.File]::Open(
            $lockPath,
            [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
    }
    catch {
        throw "Another PBR shadow capture transaction is active: $lockPath"
    }

    # Recovery happens before scene/tool/output validation so an interrupted
    # transaction cannot be stranded by an unrelated missing input.
    Restore-PendingGameConfig -GameConfigPath $gameConfigPath `
        -BackupPath $backupPath -MarkerPath $markerPath
    if (Test-Path -LiteralPath $backupPath -PathType Leaf) {
        throw "Orphaned PBR capture backup exists without a marker; inspect it before continuing: $backupPath"
    }

    if (-not $IUnderstandThisTemporarilyChangesGameConfig) {
        throw @"
This runner temporarily selects Assets/Scenes/PBRTest.he in Project/Config/Game.json because
HGame has no scene override. Re-run with -IUnderstandThisTemporarilyChangesGameConfig after
confirming no other process is editing Game.json. The original bytes are restored in finally.
"@
    }

    $seenSequences = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    if ($Sequences.Count -eq 0) {
        throw "At least one capture sequence is required."
    }
    foreach ($sequence in $Sequences) {
        if (-not $seenSequences.Add($sequence)) {
            throw "Duplicate capture sequence is not allowed: $sequence"
        }
    }
    $resolutionMatch = [regex]::Match($Resolution, "^(?<width>[1-9][0-9]*)[xX](?<height>[1-9][0-9]*)$")
    $width = [int]$resolutionMatch.Groups["width"].Value
    $height = [int]$resolutionMatch.Groups["height"].Value
    if ($width -gt 8192 -or $height -gt 8192) {
        throw "Resolution dimensions must each be at most 8192: $Resolution"
    }
    if ($Label -ne "debug" -and ($PbrDebugMode -ne "Disabled" -or $CascadeDebug)) {
        throw "PBR and cascade debug views require -Label debug."
    }

    foreach ($requiredPath in @($gameConfigPath, $pbrScenePath, $hectlPath)) {
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw "Missing required PBR capture input: $requiredPath"
        }
    }

    if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
        $OutputRoot = Join-Path $repositoryRoot "Artifacts\PBRShadowCSM"
    }
    elseif (-not [System.IO.Path]::IsPathRooted($OutputRoot)) {
        $OutputRoot = Join-Path $repositoryRoot $OutputRoot
    }
    $OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
    $null = Get-RepositoryRelativePath `
        -Path $OutputRoot -RepositoryRoot $repositoryRoot

    if ($Label -eq "debug") {
        $profileName = $PbrDebugMode.ToLowerInvariant()
        if ($CascadeDebug) {
            $profileName += "-cascades"
        }
        $artifactLabel = $profileName
        $publishedOutputDirectory = [System.IO.Path]::GetFullPath(
            (Join-Path (Join-Path $OutputRoot "debug") $profileName))
    }
    else {
        $artifactLabel = $Label
        $publishedOutputDirectory = [System.IO.Path]::GetFullPath(
            (Join-Path $OutputRoot $Label))
    }

    $expectedArtifactNames = [System.Collections.Generic.List[string]]::new()
    foreach ($sequence in $Sequences) {
        $sequenceName = $sequence.ToLowerInvariant()
        $expectedArtifactNames.Add(
            ("{0}-{1}-f000.png" -f $artifactLabel, $sequenceName))
        if ($sequence -ne "Static") {
            for ($stepIndex = 1; $stepIndex -le $StepCount; ++$stepIndex) {
                $completedSteps = [long]$StepFrames * $stepIndex
                $expectedArtifactNames.Add(
                    ("{0}-{1}-f{2:D3}.png" -f `
                        $artifactLabel, $sequenceName, $completedSteps))
            }
        }
        foreach ($logKind in @("stdout", "stderr")) {
            $expectedArtifactNames.Add(
                ("{0}-{1}.{2}.log" -f $artifactLabel, $sequenceName, $logKind))
        }
    }
    $manifestFileName = "$artifactLabel-manifest.json"
    # Publish the manifest last so it cannot describe a partially replaced set.
    $expectedArtifactNames.Add($manifestFileName)
    if (Test-Path -LiteralPath $publishedOutputDirectory) {
        $publishedOutputInfo = Get-Item -LiteralPath $publishedOutputDirectory
        if (-not $publishedOutputInfo.PSIsContainer -or
            ($publishedOutputInfo.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to replace a non-directory or reparse point: $publishedOutputDirectory"
        }
        $existingArtifacts = @(Get-ChildItem `
            -LiteralPath $publishedOutputDirectory -Force)
        $unsafeArtifacts = @($existingArtifacts | Where-Object {
            $_.PSIsContainer -or
            ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
            -not $_.Name.StartsWith(
                "$artifactLabel-", [StringComparison]::OrdinalIgnoreCase) -or
            $_.Extension -notin @(".png", ".log", ".json")
        })
        if ($unsafeArtifacts.Count -gt 0) {
            throw "Refusing to replace an output directory containing non-capture files: $publishedOutputDirectory"
        }
        if ($existingArtifacts.Count -gt 0 -and -not $Overwrite) {
            throw "PBR capture outputs already exist; pass -Overwrite to replace them:$([Environment]::NewLine)$($existingArtifacts.FullName -join [Environment]::NewLine)"
        }
    }

    $outputParent = Split-Path -Parent $publishedOutputDirectory
    New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
    $outputParentInfo = Get-Item -LiteralPath $outputParent
    if (-not $outputParentInfo.PSIsContainer -or
        ($outputParentInfo.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to stage through a non-directory or reparse point: $outputParent"
    }
    $stagingDirectory = Join-Path $outputParent `
        (".{0}.staging.{1}" -f `
            [System.IO.Path]::GetFileName($publishedOutputDirectory),
            [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $stagingDirectory -ErrorAction Stop | Out-Null
    $script:outputDirectory = $stagingDirectory
    $script:publishedOutputDirectory = $publishedOutputDirectory

    $coreEvidencePaths = @(
        "Engine/Source/Runtime/Core/AppFramework/Application.cpp",
        "Engine/Source/Runtime/Graphics/Material/Material.cpp",
        "Engine/Source/Runtime/Graphics/RHI/TextureManager.cpp",
        "Engine/Source/Runtime/Graphics/RHI/TextureManager.h",
        "Engine/Source/Runtime/Graphics/Renderer/RuntimeRenderer.cpp",
        "Engine/Source/Runtime/Graphics/Renderer/RuntimeRenderer.h",
        "Engine/Source/Runtime/Graphics/Renderer/SceneRenderer.cpp",
        "Engine/Source/Runtime/Graphics/Renderer/SceneRenderer.h",
        "Engine/Content/Shaders/Modern/SceneEnvironmentPrecompute.hlsl",
        "Engine/Content/Shaders/Modern/SceneOpaque.hlsl",
        "Engine/Content/Shaders/Modern/SceneShadow.hlsl",
        "Engine/Content/Shaders/Modern/SceneToneMap.hlsl",
        "Project/Assets/Scenes/PBRTest.he",
        "Project/Scripts/PBRShadowCaptureCamera.py",
        "Tools/Agent/Capture-PBRShadow.ps1",
        "Tools/Agent/Session.ps1",
        "Tools/Agent/hectl.ps1")
    $gitHeadBefore = Get-GitHead -RepositoryRoot $repositoryRoot
    $coreEvidenceBefore = Get-CoreFileEvidence `
        -RepositoryRoot $repositoryRoot -RelativePaths $coreEvidencePaths

    $originalConfig = [System.IO.File]::ReadAllBytes($gameConfigPath)
    $originalHash = Get-BytesSha256 -Bytes $originalConfig
    $configText = [Text.Encoding]::UTF8.GetString($originalConfig)
    $matches = [regex]::Matches(
        $configText,
        '(?m)("DefaultScene"\s*:\s*")(?<scene>[^"]+)(")')
    if ($matches.Count -ne 1) {
        throw "Project/Config/Game.json must contain exactly one Game.DefaultScene."
    }
    $sceneGroup = $matches[0].Groups["scene"]
    $temporaryText = $configText.Substring(0, $sceneGroup.Index) +
        "Assets/Scenes/PBRTest.he" +
        $configText.Substring($sceneGroup.Index + $sceneGroup.Length)
    $temporaryBytes = [Text.UTF8Encoding]::new($false).GetBytes($temporaryText)
    $temporaryHash = Get-BytesSha256 -Bytes $temporaryBytes

    $environmentNames = @("HE_PBR_DEBUG_VIEW", "HE_SHADOW_CASCADE_DEBUG")
    $originalEnvironment = @{}
    foreach ($name in $environmentNames) {
        $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
    }

    $records = [System.Collections.Generic.List[object]]::new()
    $sequenceRecords = [System.Collections.Generic.List[object]]::new()
    try {
        Write-BytesAtomically -Path $backupPath -Bytes $originalConfig
        $markerText = (([ordered]@{
            schemaVersion = 2
            originalSha256 = $originalHash
            temporarySha256 = $temporaryHash
            backupPath = $backupPath
        } | ConvertTo-Json -Compress) + [Environment]::NewLine)
        try {
            Write-TextAtomically -Path $markerPath -Text $markerText
        }
        catch {
            Remove-Item -LiteralPath $backupPath -Force -ErrorAction Stop
            throw
        }
        Write-BytesAtomically -Path $gameConfigPath -Bytes $temporaryBytes

        [Environment]::SetEnvironmentVariable("HE_PBR_DEBUG_VIEW", $PbrDebugMode, "Process")
        [Environment]::SetEnvironmentVariable(
            "HE_SHADOW_CASCADE_DEBUG", $(if ($CascadeDebug) { "1" } else { "0" }), "Process")

        foreach ($sequence in $Sequences) {
            Invoke-CaptureSequence -Sequence $sequence -Records $records `
                -SequenceRecords $sequenceRecords
        }
    }
    finally {
        try {
            Restore-PendingGameConfig -GameConfigPath $gameConfigPath `
                -BackupPath $backupPath -MarkerPath $markerPath
        }
        finally {
            foreach ($name in $environmentNames) {
                [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name], "Process")
            }
        }
    }

    $restoredConfigHash = (Get-FileHash `
        -Algorithm SHA256 -LiteralPath $gameConfigPath).Hash
    if (-not [string]::Equals(
            $restoredConfigHash,
            $originalHash,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Project/Config/Game.json was not restored after capture."
    }
    if ($sequenceRecords.Count -ne $Sequences.Count) {
        throw "Sequence evidence count mismatch: expected=$($Sequences.Count), actual=$($sequenceRecords.Count)"
    }

    $gitHeadAfter = Get-GitHead -RepositoryRoot $repositoryRoot
    if (-not [string]::Equals(
            $gitHeadBefore,
            $gitHeadAfter,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Git HEAD changed during capture."
    }
    $coreEvidenceAfter = Get-CoreFileEvidence `
        -RepositoryRoot $repositoryRoot -RelativePaths $coreEvidencePaths
    Assert-FileEvidenceUnchanged `
        -Before $coreEvidenceBefore -After $coreEvidenceAfter

    $executableEvidence = $sequenceRecords[0].executable
    foreach ($sequenceRecord in $sequenceRecords) {
        if (-not [string]::Equals(
                [string]$sequenceRecord.executable.path,
                [string]$executableEvidence.path,
                [StringComparison]::Ordinal) -or
            -not [string]::Equals(
                [string]$sequenceRecord.executable.sha256,
                [string]$executableEvidence.sha256,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Capture sequences used different executable artifacts."
        }
    }

    $manifestPath = Get-DirectChildPath `
        -Directory $stagingDirectory -FileName $manifestFileName
    $publishedManifestPath = Get-DirectChildPath `
        -Directory $publishedOutputDirectory -FileName $manifestFileName
    $manifest = [ordered]@{
        schemaVersion = 3
        capturedAtUtc = [DateTime]::UtcNow.ToString(
            "o", [Globalization.CultureInfo]::InvariantCulture)
        repository = [ordered]@{
            gitHead = $gitHeadBefore
            coreFiles = @($coreEvidenceBefore)
        }
        executable = $executableEvidence
        outputDirectory = Get-RepositoryRelativePath `
            -Path $publishedOutputDirectory -RepositoryRoot $repositoryRoot
        manifestPath = Get-RepositoryRelativePath `
            -Path $publishedManifestPath -RepositoryRoot $repositoryRoot
        gameConfig = [ordered]@{
            path = "Project/Config/Game.json"
            originalSha256 = $originalHash
            temporarySha256 = $temporaryHash
        }
        label = $Label
        artifactLabel = $artifactLabel
        scene = "Assets/Scenes/PBRTest.he"
        sceneSourcePath = "Project/Assets/Scenes/PBRTest.he"
        configuration = $Configuration
        resolution = $Resolution.ToLowerInvariant()
        fixedDeltaSeconds = $FixedDeltaSeconds
        timeoutSeconds = $TimeoutSeconds
        exposure = 0.31
        initialCamera = [ordered]@{
            translation = @(0.0, 2.5, 11.0)
            rotationRadians = @(0.0, -1.5707963705062866, 0.0)
            perspectiveFovRadians = 1.0471975803375244
            nearPlane = 0.1
            farPlane = 250.0
        }
        stepFrames = $StepFrames
        stepCount = $StepCount
        denseSequence = [bool]($StepFrames -eq 1)
        captureIntervalSimulationSeconds = [double]($StepFrames * $FixedDeltaSeconds)
        sequences = @($Sequences)
        pbrDebugMode = $PbrDebugMode
        cascadeDebug = [bool]$CascadeDebug
        renderValidation = [bool]$RenderValidation
        renderStats = [bool]$RenderStats
        sequenceRuns = @($sequenceRecords)
        captures = @($records)
    }
    Write-TextAtomically -Path $manifestPath `
        -Text (($manifest | ConvertTo-Json -Depth 16) + [Environment]::NewLine)
    Publish-StagedArtifacts `
        -StagingDirectory $stagingDirectory `
        -OutputDirectory $publishedOutputDirectory `
        -ArtifactNames $expectedArtifactNames.ToArray() `
        -AllowOverwrite ([bool]$Overwrite)
    $stagingDirectory = $null
    Write-Host "PBR shadow captures: $publishedOutputDirectory"
    Write-Host "Manifest: $publishedManifestPath"
    [ordered]@{output=$publishedOutputDirectory;manifest=$publishedManifestPath;manifestSha256=(Get-FileHash -LiteralPath $publishedManifestPath -Algorithm SHA256).Hash;retention='Explicit capture baseline/output is preserved'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $storageReceipt 'result.json') -Encoding UTF8
    $storageState = 'success'
}
finally {
    try {
        if (-not [string]::IsNullOrWhiteSpace($stagingDirectory) -and
            (Test-Path -LiteralPath $stagingDirectory)) {
            try {
                Remove-FlatDirectory -Path $stagingDirectory
            }
            catch {
                Write-Warning "Failed to clean capture staging directory: $stagingDirectory"
            }
        }
        foreach ($temporaryOutput in ($deferredHectlOutputs | Select-Object -Unique)) {
            if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf) {
                try {
                    Remove-Item -LiteralPath $temporaryOutput -Force -ErrorAction Stop
                }
                catch {
                    Write-Warning "Deferred hectl output is still locked: $temporaryOutput"
                }
            }
        }
    }
    finally {
        if ($null -ne $stateLock) {
            $stateLock.Dispose()
        }
        Complete-HEStorageRun -Run $storageRun -State $storageState
    }
}
