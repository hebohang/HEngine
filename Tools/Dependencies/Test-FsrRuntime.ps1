$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
# Isolated manifest fixtures test validation rules; these are not executable DLLs.
$work=Join-Path ([IO.Path]::GetTempPath()) ('he-fsr-contract-'+[guid]::NewGuid().ToString('N'))
$created=[Collections.Generic.List[string]]::new()
function Write-Fixture([string]$Name,[string]$Content) {
    $path=Join-Path $work $Name
    [IO.File]::WriteAllText($path,$Content,[Text.UTF8Encoding]::new($false))
    if (!$created.Contains($path)) { $created.Add($path) }
}
$null=New-Item -ItemType Directory -Path "$work/D3D12","$work/Licenses" -Force
try {
    Write-Fixture 'RuntimeDependencies.ps1' ([IO.File]::ReadAllText("$PSScriptRoot/RuntimeDependencies.ps1"))
    foreach ($file in @('D3D12/D3D12Core.dll','amd_fidelityfx_dx12.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll',
        'Licenses/FidelityFX.txt','Licenses/Microsoft-Visual-Cpp-Runtime.txt')) { Write-Fixture $file "fixture:$file" }
    $lock=Get-Content "$PSScriptRoot/native-runtime.arm64.lock.json" -Raw | ConvertFrom-Json
    $lock.files.'D3D12/D3D12Core.dll'=(Get-FileHash "$work/D3D12/D3D12Core.dll").Hash.ToLowerInvariant()
    Write-Fixture 'native-runtime.arm64.lock.json' ($lock | ConvertTo-Json -Depth 5)
    . "$work/RuntimeDependencies.ps1"
    $baseline=[ordered]@{schema=1;architecture='arm64';fsr=$true;fsrDx12=$true;fsrVulkan=$false;dx12=$true;vulkan=$true;oidn=$false;
        fsrSourceArchiveSha256=$lock.fsrSourceArchiveSha256;
        fsrRuntimeSha256=(Get-FileHash "$work/amd_fidelityfx_dx12.dll").Hash.ToLowerInvariant()}
    Write-Fixture 'HEngine.Runtime.json' ($baseline | ConvertTo-Json)
    $result=Test-HENativeRuntimeDependencies -Root $work -Shipping
    if (!$result.Fsr -or $result.Checked -ne 2) { throw 'Valid DX12-only ARM64 FSR contract was rejected.' }
    foreach ($field in @('fsrRuntimeSha256','fsrSourceArchiveSha256','fsrVulkan','oidn','fsrDx12')) {
        $invalid=$baseline | ConvertTo-Json | ConvertFrom-Json
        $invalid.$field=if ($field -like '*Sha256') { '0'*64 } elseif ($field -eq 'fsrDx12') { 'true' } else { $true }
        Write-Fixture 'HEngine.Runtime.json' ($invalid | ConvertTo-Json)
        $rejected=$false
        try { $null=Test-HENativeRuntimeDependencies -Root $work -Shipping } catch { $rejected=$true }
        if (!$rejected) { throw "Invalid $field was accepted." }
    }
    $missing=$baseline | ConvertTo-Json | ConvertFrom-Json
    $missing.PSObject.Properties.Remove('fsrDx12')
    Write-Fixture 'HEngine.Runtime.json' ($missing | ConvertTo-Json)
    $rejected=$false
    try { $null=Test-HENativeRuntimeDependencies -Root $work } catch { $rejected=$true }
    if (!$rejected) { throw 'Ambiguous ARM64 FSR capability was accepted.' }
    Write-Fixture 'HEngine.Runtime.json' ($baseline | ConvertTo-Json)
    Write-Fixture 'amd_fidelityfx_dx12.dll' 'changed after packaging'
    $rejected=$false
    try { $null=Test-HENativeRuntimeDependencies -Root $work } catch { $rejected=$true }
    if (!$rejected) { throw 'Tampered runtime was accepted.' }
    Write-Output 'unit-test PASS: ARM64 DX12-only FSR, provenance, digest, backend and Shipping exclusions.'
} finally {
    foreach ($path in $created) { Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue }
    foreach ($directory in @("$work/D3D12","$work/Licenses",$work)) { Remove-Item -LiteralPath $directory -ErrorAction SilentlyContinue }
}
