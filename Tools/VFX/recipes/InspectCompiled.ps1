param([string]$InputFile, [string]$OutputFile, [string]$CoreAssembly)
$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom($CoreAssembly)
$data = [IO.File]::ReadAllBytes($InputFile)
$offset = 8
$edit = $null
while ($offset -lt $data.Length) {
    if ($offset + 8 -gt $data.Length) { throw 'Truncated EFKE chunk header' }
    $tag = [Text.Encoding]::ASCII.GetString($data, $offset, 4)
    $size = [BitConverter]::ToInt32($data, $offset + 4)
    if ($size -lt 0 -or $offset + 8 + $size -gt $data.Length) { throw 'Invalid EFKE chunk length' }
    if ($tag -eq 'EDIT') {
        $edit = [byte[]]::new($size)
        [Array]::Copy($data, $offset + 8, $edit, 0, $size)
        break
    }
    $offset += 8 + $size
}
if ($null -eq $edit) { throw 'Compiled EFKE lacks editable data' }
$type = $assembly.GetType('Effekseer.IO.EfkEfcXml')
$decoder = [Activator]::CreateInstance($type, $true)
$document = $type.GetMethod('Decompress').Invoke($decoder, @(,$edit))
$document.Save($OutputFile)
