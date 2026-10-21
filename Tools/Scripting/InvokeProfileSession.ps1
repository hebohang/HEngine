param([Parameter(Mandatory=$true)][string]$RequestPath)
$ErrorActionPreference='Stop'
$request=Get-Content -LiteralPath $RequestPath -Raw|ConvertFrom-Json
$parameters=@{}
foreach($property in $request.parameters.PSObject.Properties) { $parameters[$property.Name]=$property.Value }
try { & $request.scriptPath @parameters; exit 0 }
catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }
