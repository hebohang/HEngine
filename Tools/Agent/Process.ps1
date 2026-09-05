if (-not ([System.Management.Automation.PSTypeName]"HEngine.AgentProcessNativeMethods").Type) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace HEngine
{
    [StructLayout(LayoutKind.Sequential)]
    public struct AgentJobObjectBasicLimitInformation
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AgentIoCounters
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AgentJobObjectExtendedLimitInformation
    {
        public AgentJobObjectBasicLimitInformation BasicLimitInformation;
        public AgentIoCounters IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    public static class AgentProcessNativeMethods
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateJobObject(IntPtr jobAttributes, string name);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr OpenJobObject(uint desiredAccess, bool inheritHandle, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetInformationJobObject(
            IntPtr job,
            int informationClass,
            ref AgentJobObjectExtendedLimitInformation information,
            uint informationLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll")]
        public static extern IntPtr GetCurrentProcess();

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool DuplicateHandle(
            IntPtr sourceProcess,
            IntPtr sourceHandle,
            IntPtr targetProcess,
            out IntPtr targetHandle,
            uint desiredAccess,
            bool inheritHandle,
            uint options);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool TerminateJobObject(IntPtr job, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool CloseHandle(IntPtr handle);
    }
}
'@
}

function New-AgentWin32Exception {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Operation,

        [Parameter(Mandatory = $true)]
        [int]$ErrorCode
    )

    $nativeError = New-Object System.ComponentModel.Win32Exception($ErrorCode)
    return New-Object System.InvalidOperationException(
        "$Operation failed with Win32 error ${ErrorCode}: $($nativeError.Message)",
        $nativeError)
}

function Get-AgentProcessJobHandle {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Job
    )

    if ($null -eq $Job.PSObject.Properties["Handle"]) {
        throw "Invalid Agent process Job: missing Handle."
    }

    $handle = [System.IntPtr]$Job.Handle
    if ($handle -eq [System.IntPtr]::Zero) {
        throw "Agent process Job is already closed."
    }
    return $handle
}

function New-AgentProcessJob {
    param(
        [ValidateNotNullOrEmpty()]
        [string]$Name
    )

    $jobName = $null
    if (-not [string]::IsNullOrWhiteSpace($Name)) {
        $jobName = $Name
    }

    $handle = [HEngine.AgentProcessNativeMethods]::CreateJobObject(
        [System.IntPtr]::Zero,
        $jobName)
    if ($handle -eq [System.IntPtr]::Zero) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw (New-AgentWin32Exception -Operation "CreateJobObject" -ErrorCode $errorCode)
    }

    $bCreated = $false
    try {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($null -ne $jobName -and $errorCode -eq 183) {
            throw "Agent process Job already exists: $jobName"
        }

        $information = New-Object HEngine.AgentJobObjectExtendedLimitInformation
        # BasicLimitInformation is a value type. Assign the populated value
        # back as a whole so Windows PowerShell 5.1 does not mutate a copy.
        $basicLimitInformation = New-Object HEngine.AgentJobObjectBasicLimitInformation
        $basicLimitInformation.LimitFlags = 0x00002000
        $information.BasicLimitInformation = $basicLimitInformation
        $informationLength = [Runtime.InteropServices.Marshal]::SizeOf(
            [type][HEngine.AgentJobObjectExtendedLimitInformation])
        if (-not [HEngine.AgentProcessNativeMethods]::SetInformationJobObject(
            $handle,
            9,
            [ref]$information,
            [uint32]$informationLength)) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw (New-AgentWin32Exception -Operation "SetInformationJobObject" -ErrorCode $errorCode)
        }

        $bCreated = $true
        return [pscustomobject]@{
            PSTypeName = "HEngine.AgentProcessJob"
            Handle = $handle
            Name = $jobName
            KillOnClose = $true
        }
    }
    finally {
        if (-not $bCreated) {
            [void][HEngine.AgentProcessNativeMethods]::CloseHandle($handle)
        }
    }
}

function Add-AgentProcessToJob {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Job,

        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process
    )

    $handle = Get-AgentProcessJobHandle -Job $Job
    $Process.Refresh()
    if ($Process.HasExited) {
        throw "Cannot assign an exited process to an Agent process Job: pid=$($Process.Id)"
    }

    try {
        $processHandle = $Process.Handle
        if (-not [HEngine.AgentProcessNativeMethods]::AssignProcessToJobObject(
            $handle,
            $processHandle)) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw (New-AgentWin32Exception -Operation "AssignProcessToJobObject(pid=$($Process.Id))" -ErrorCode $errorCode)
        }

        if (-not [string]::IsNullOrWhiteSpace([string]$Job.Name)) {
            # Membership alone does not keep a named Job object discoverable
            # after this PowerShell command exits. Keep one intentional handle
            # in the session root; Windows closes it automatically with the
            # process, while a later command can OpenJobObject by name.
            $remoteJobHandle = [System.IntPtr]::Zero
            if (-not [HEngine.AgentProcessNativeMethods]::DuplicateHandle(
                [HEngine.AgentProcessNativeMethods]::GetCurrentProcess(),
                $handle,
                $processHandle,
                [ref]$remoteJobHandle,
                0,
                $false,
                0x00000002)) {
                $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                throw (New-AgentWin32Exception -Operation "DuplicateHandle(Job, pid=$($Process.Id))" -ErrorCode $errorCode)
            }
        }
    }
    catch {
        throw
    }
}

function Close-AgentProcessJob {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Job
    )

    $handle = Get-AgentProcessJobHandle -Job $Job
    if (-not [HEngine.AgentProcessNativeMethods]::CloseHandle($handle)) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw (New-AgentWin32Exception -Operation "CloseHandle(Job)" -ErrorCode $errorCode)
    }
    $Job.Handle = [System.IntPtr]::Zero
}

function Stop-AgentProcessJobByName {
    [OutputType([bool])]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateNotNullOrEmpty()]
        [string]$Name,

        [uint32]$ExitCode = 1
    )

    $jobTerminateAccess = 0x00000008
    $handle = [HEngine.AgentProcessNativeMethods]::OpenJobObject(
        $jobTerminateAccess,
        $false,
        $Name)
    if ($handle -eq [System.IntPtr]::Zero) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($errorCode -eq 2) {
            return $false
        }
        throw (New-AgentWin32Exception -Operation "OpenJobObject('$Name')" -ErrorCode $errorCode)
    }

    $operationError = $null
    try {
        if (-not [HEngine.AgentProcessNativeMethods]::TerminateJobObject($handle, $ExitCode)) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw (New-AgentWin32Exception -Operation "TerminateJobObject('$Name')" -ErrorCode $errorCode)
        }
        return $true
    }
    catch {
        $operationError = $_
        throw
    }
    finally {
        if (-not [HEngine.AgentProcessNativeMethods]::CloseHandle($handle)) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            if ($null -eq $operationError) {
                throw (New-AgentWin32Exception -Operation "CloseHandle(Job '$Name')" -ErrorCode $errorCode)
            }
        }
    }
}

function Get-AgentArtifactProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactPath,

        [Parameter(Mandatory = $true)]
        [string]$ProcessName
    )

    if (-not (Test-Path -LiteralPath $ArtifactPath)) {
        return $null
    }
    $expectedPath = [System.IO.Path]::GetFullPath($ArtifactPath)
    foreach ($candidate in Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) {
        try {
            if ([string]::Equals(
                [System.IO.Path]::GetFullPath($candidate.Path),
                $expectedPath,
                [System.StringComparison]::OrdinalIgnoreCase)) {
                return $candidate
            }
        }
        catch {
        }
    }
    return $null
}

function Stop-AgentOwnedProcess {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,

        [string]$Description = "Agent-owned process"
    )

    $Process.Refresh()
    if ($Process.HasExited) {
        return
    }
    if ($Process.CloseMainWindow() -and $Process.WaitForExit(2000)) {
        $Process.WaitForExit()
        return
    }

    # HEngine tools may create compiler, shader, or helper children. Terminate
    # the exact still-running root and its descendants instead of leaking them.
    $taskkill = Join-Path $env:SystemRoot "System32\taskkill.exe"
    if (-not (Test-Path -LiteralPath $taskkill)) {
        throw "Windows taskkill.exe was not found: $taskkill"
    }
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $taskkill /PID ([string]$Process.Id) /T /F *> $null
        $taskkillExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    if (-not $Process.WaitForExit(5000)) {
        throw "$Description tree did not exit after termination: pid=$($Process.Id)"
    }
    $Process.WaitForExit()
    if ($taskkillExitCode -ne 0) {
        throw "Failed to terminate the complete $Description tree: taskkill exit=$taskkillExitCode."
    }
}
