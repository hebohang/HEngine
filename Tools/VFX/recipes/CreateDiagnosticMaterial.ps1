param([string]$OutputFile, [string]$CoreAssembly)
$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom($CoreAssembly)
$type = $assembly.GetType('Effekseer.Utils.MaterialInformation')
$material = [Activator]::CreateInstance($type)
$material.GUID = 180600041
$material.Name = 'Original diagnostic unlit cyan'
$material.Description = 'HEngine original constant output; no external inputs'
$material.ShadingModel = 1
# This is Effekseer's portable material source, never platform shader bytecode.
$material.Code = @'
$F3$ worldPositionOffset = $F3$(0.0, 0.0, 0.0);
$F3$ emissive = $F3$(0.05, 0.8, 1.0);
$F1$ opacity = 1.0;
$F1$ opacityMask = 1.0;
'@
[IO.File]::WriteAllBytes($OutputFile, $material.SaveToBytes())
$readback = [Activator]::CreateInstance($type)
if (!$readback.Load($OutputFile) -or $readback.ShadingModel -ne 1 -or $readback.Code -ne $material.Code -or $readback.Textures.Length -ne 0) {
    throw 'Official MaterialInformation roundtrip mismatch'
}
