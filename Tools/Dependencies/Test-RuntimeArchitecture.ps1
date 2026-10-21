$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/RuntimeDependencies.ps1"
$work=Join-Path ([IO.Path]::GetTempPath()) ('he-runtime-arch-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $work
try {
    foreach ($arch in @('', 'x64', 'arm64')) {
        $contract=[ordered]@{schema=1;fsr=$false;dx12=$false;vulkan=$false;oidn=$false}
        if ($arch) { $contract.architecture=$arch }
        $contract | ConvertTo-Json | Set-Content "$work/HEngine.Runtime.json" -Encoding UTF8
        $result=Test-HENativeRuntimeDependencies -Root $work
        if ($result.Checked -ne 0) { throw 'Disabled runtime unexpectedly required native files.' }
    }
    $contract.architecture='unsupported'
    $contract | ConvertTo-Json | Set-Content "$work/HEngine.Runtime.json" -Encoding UTF8
    $rejected=$false
    try { $null=Test-HENativeRuntimeDependencies -Root $work } catch { $rejected=$true }
    if (!$rejected) { throw 'Unknown native architecture was accepted.' }
    Write-Output 'unit-test PASS: legacy/x64/ARM64 runtime contracts under strict mode; unknown architecture rejected.'
} finally {
    Remove-Item -LiteralPath "$work/HEngine.Runtime.json" -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $work -ErrorAction SilentlyContinue
}
