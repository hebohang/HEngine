param([ValidateSet('all','none','python','csharp','both')][string]$Profile = 'all')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$project = Join-Path $repo 'Project'
$work = Join-Path $repo ('out/agent/scripting/shipping-matrix-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$run = Start-HEStorageRun -Project $project -Workflow 'script-plugin-matrix-shipping' -Paths @($work) -Outer
$state = 'failed'
$results = @()
try {
    $profiles = if ($Profile -eq 'all') { @('none','python','csharp','both') } else { @($Profile) }
    foreach ($name in $profiles) {
        $python = $name -in @('python','both')
        $csharp = $name -in @('csharp','both')
        $config = Get-Content "$project/Config/Plugins.json" -Raw | ConvertFrom-Json
        foreach ($hostName in @('Editor','DevGame','Game','Tests','Tools','RenderSmoke')) {
            $config.'he.script.python'.$hostName = $python -and $hostName -eq 'Game'
            $config.'he.script.csharp'.$hostName = $csharp -and $hostName -eq 'Game'
        }
        $configPath = Join-Path $work "$name.json"
        $config | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $configPath -Encoding UTF8
        $game = switch ($name) { none {'Native'} python {'Python'} csharp {'CSharp'} both {'Sample'} }
        $log = Join-Path $work "$name.log"
        $previousPreference = $ErrorActionPreference
        try {
            # Native stderr includes CMake warnings. Preserve the complete log
            # and decide success from the awaited process exit code.
            $ErrorActionPreference = 'Continue'
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$repo/Tools/Agent/Package.ps1" `
                -Project $project -GameConfig "Config/Game.Script$game.json" -PluginConfig $configPath *> $log
            $packageExit = $LASTEXITCODE
        } finally { $ErrorActionPreference = $previousPreference }
        if ($packageExit -ne 0) { throw "Shipping profile '$name' failed; see $log" }
        $text = Get-Content $log -Raw
        $match = [regex]::Match($text, '(?m)^Packaged game: ([^\r\n]+)')
        if (!$match.Success) { throw "No published executable receipt for '$name'." }
        $exe = $match.Groups[1].Value.Trim()
        $package = Split-Path $exe
        if ((Test-Path "$package/python311.dll") -ne $python -or
            (Test-Path "$package/Scripting/CSharp/HEngine.ScriptRuntime.dll") -ne $csharp) {
            throw "Shipping runtime file boundary differs in '$name'."
        }
        # Package.ps1 already requires the actual graph, shipping link map and
        # recursive PE/managed closure verifier before publishing. Keep the receipt.
        if ($text -notmatch 'recursive PE dependency closure' -or $text -notmatch 'full-start check PASS') {
            throw "Incomplete dependency or real-start evidence for '$name'."
        }
        $stdoutMatch = [regex]::Match($text, '(?m)^\s*stdout: ([^\r\n]+)')
        if (!$stdoutMatch.Success) { throw 'Missing real shipping stdout receipt.' }
        $stdout = Get-Content -LiteralPath $stdoutMatch.Groups[1].Value.Trim() -Raw
        $stdout | Set-Content "$work/$name.stdout.log"
        if ($csharp -and $stdout -notmatch '\[C#\].*Initialize entity=') { throw 'C# shipping script did not initialize.' }
        if ($python -and $stdout -notmatch '\[Python\] observer Initialize') { throw 'Python shipping script did not initialize.' }
        if ($name -eq 'none' -and $stdout -notmatch '\[NativeScript\] TouchSample created') { throw 'Native shipping gameplay did not initialize.' }
        if ($name -eq 'both' -and $stdout -notmatch '\[Python\] observed C# score=') { throw 'Shipping runtime cooperation was not observed.' }
        if ($stdout -match '\[Script\].*(kind=|failed)|Fatal|Unhandled exception') { throw 'Unexpected shipping script diagnostic.' }
        $manifest = Get-Content "$package/Game.manifest.json" -Raw | ConvertFrom-Json
        if (!$python -and @($manifest.Files | Where-Object {$_.Path -match '(?i)\.py$|PythonScripts.json$'}).Count) {
            throw 'Disabled Python leaked scripts into the cooked package.'
        }
        $results += [ordered]@{profile=$name;build='PASS';publication='PASS';dependencyClosure='PASS';boundedRealStartup='PASS';
            interactiveAcceptance='NOT_TESTED';executable=$exe;contentHash=$manifest.ContentHash;
            executableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()}
        $results | ConvertTo-Json -Depth 10 | Set-Content "$work/results.json"
    }
    $state = 'success'
} finally {
    @{status=$state;results=@($results)} | ConvertTo-Json -Depth 10 | Set-Content "$work/summary.json"
    Complete-HEStorageRun -Run $run -State $state
}
Write-Host "Shipping plugin matrix PASS: $work"
