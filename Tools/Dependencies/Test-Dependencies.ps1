$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Agent/LibClang.ps1')
$sdk = Resolve-HELibClang
if ($sdk.Root -notmatch '/HEngineParser/3rd_party/LLVM/Win64$') { throw 'Default SDK escaped the bundled root.' }
$fixture = Join-Path $PSScriptRoot ('../../out/agent/dependency-validation/sdk-invalid-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $fixture | Out-Null
$manifest = [ordered]@{schema=1;version='22.1.3';resourceVersion='22';platform='win-x64';files=@(@{path='../escape.dll';sha256=('0'*64)})}
$manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $fixture 'manifest.json')
$rejected = $false
try { Resolve-HELibClang -Root $fixture | Out-Null } catch { $rejected = $_.Exception.Message -match 'Invalid libclang manifest entry' }
if (!$rejected) { throw 'Unsafe SDK manifest was accepted.' }
$manifest.files = @(@{path='test.dll';sha256=('0'*64)})
[IO.File]::WriteAllText((Join-Path $fixture 'test.dll'),'corrupt')
$manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $fixture 'manifest.json')
$rejected = $false
try { Resolve-HELibClang -Root $fixture | Out-Null } catch { $rejected = $_.Exception.Message -match 'checksum mismatch' }
if (!$rejected) { throw 'Corrupt SDK was accepted.' }
$manifest.version = '19.0.0'
$manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $fixture 'manifest.json')
$rejected = $false
try { Resolve-HELibClang -Root $fixture | Out-Null } catch { $rejected = $_.Exception.Message -match 'Unsupported' }
if (!$rejected) { throw 'Unsupported SDK version was accepted.' }
Write-Output 'unit-test: Dependency SDK selection, traversal, corruption and version contracts PASS'
