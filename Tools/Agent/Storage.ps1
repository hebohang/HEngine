# Windows PowerShell 5.1 bridge; cleanup errors never replace workflow results.
$script:heStorageTool = Join-Path $PSScriptRoot 'storage.py'
$script:heStorageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))

function Invoke-HEStorage {
    param([string[]]$Arguments)
    $savedExit = $global:LASTEXITCODE
    try {
        if ($env:HE_STORAGE_PYTHON) {
            $output = & $env:HE_STORAGE_PYTHON $script:heStorageTool @Arguments
        }
        elseif (Get-Command py.exe -ErrorAction SilentlyContinue) {
            $output = & py.exe -3 $script:heStorageTool @Arguments
        }
        else { $output = & python $script:heStorageTool @Arguments }
        if ($LASTEXITCODE -ne 0) { throw "Storage command failed: $script:heStorageTool ($LASTEXITCODE)" }
        return $output
    }
    finally { $global:LASTEXITCODE = $savedExit }
}

function Start-HEStorageRun {
    param([string]$Project, [string]$Workflow, [string[]]$Paths, [string[]]$Locks = @(),
        [string]$Kind = 'evidence', [switch]$Outer, [string]$Id = '', [string]$ProjectId = '', [string[]]$Dependencies = @())
    $request = $null
    try {
        if (-not $Project) { $Project = Join-Path $script:heStorageRoot 'Project' }
        $request = [IO.Path]::GetTempFileName()
        $value = @{project=$Project;workflow=$Workflow;paths=@($Paths);locks=@($Locks);kind=$Kind;parent=[string]$env:HE_STORAGE_PARENT;project_id=$ProjectId;outer=[bool]$Outer}
        $value.owner_pid = $PID
        $value.owner_started = (Get-Process -Id $PID).StartTime.ToFileTimeUtc()
        $value.dependencies = @($Dependencies)
        $savedExit = $global:LASTEXITCODE
        try {
            $revision = & git -C $script:heStorageRoot rev-parse HEAD 2>$null
            if ($LASTEXITCODE -eq 0) { $value.revision = [string]$revision }
        }
        catch { $value.revision = 'unknown' }
        finally { $global:LASTEXITCODE = $savedExit }
        if ($Id) { $value.ident = $Id }
        $value | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $request -Encoding UTF8
        $result = (Invoke-HEStorage @('start','--request',$request)) | ConvertFrom-Json
        if (-not $result.id) { return $null } # Covered by the enclosing immutable artifact directory.
        $run = [pscustomobject]@{Id=$result.id;Parent=$env:HE_STORAGE_PARENT;Outer=[bool]$Outer}
        if ($Outer) { $env:HE_STORAGE_PARENT = $run.Id }
        return $run
    }
    catch { Write-Warning "storage registration deferred: $($_.Exception.Message)"; return $null }
    finally { if ($request) { Remove-Item -LiteralPath $request -Force -ErrorAction SilentlyContinue } }
}

function Complete-HEStorageRun {
    param($Run, [ValidateSet('success','failed','unknown')][string]$State = 'failed', [switch]$NoAuto)
    if ($null -eq $Run) { return }
    try { $null = Invoke-HEStorage @('finish','--id',$Run.Id,'--state',$State) }
    catch { Write-Warning "storage completion deferred ($($Run.Id)): $($_.Exception.Message)" }
    finally {
        if ($Run.Outer) { $env:HE_STORAGE_PARENT = $Run.Parent }
    }
    if (-not $NoAuto -and -not $env:HE_STORAGE_PARENT) { Invoke-HEStorageAuto }
}

function Set-HEStorageOwner {
    param($Run, $Process)
    if (-not $Run) { return }
    try { $null = Invoke-HEStorage @('owner','--id',$Run.Id,'--pid',[string]$Process.Id,'--started',[string]$Process.StartTime.ToFileTimeUtc()) }
    catch { Write-Warning "storage owner update deferred ($($Run.Id)): $($_.Exception.Message)" }
}

function Invoke-HEStorageAuto {
    if ($env:HE_STORAGE_PARENT) { return }
    try { $null = Invoke-HEStorage @('auto') }
    catch { Write-Warning "storage cleanup deferred: $($_.Exception.Message)" }
}

function Open-HECacheLease {
    param([string]$CachePath)
    # Kept outside the deletable cache unit, shared by build/start and explicit cache retirement.
    $leasePath = [string](Invoke-HEStorage @('lease-path','--cache-path',$CachePath))
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $leasePath) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ($true) {
        try { return [IO.File]::Open($leasePath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
        catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) { throw }
            Start-Sleep -Milliseconds 100
        }
    }
}
