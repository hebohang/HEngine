param([string]$ManagedWork = 'out/agent/scripting/dependency-managed')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$work = [IO.Path]::GetFullPath($ManagedWork)
$test = Join-Path $work ('dependency-contract-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force $test | Out-Null
$core = [Security.SecurityElement]::Escape((Join-Path $work 'output/HEngine.ScriptCore.dll'))
[IO.File]::WriteAllText("$test/Fixture.csproj", "<Project Sdk=`"Microsoft.NET.Sdk`"><PropertyGroup><TargetFramework>net10.0</TargetFramework><AssemblyName>DependencyFixture</AssemblyName><EnableDefaultCompileItems>false</EnableDefaultCompileItems></PropertyGroup><ItemGroup><Compile Include=`"Fixture.cs`"/><Reference Include=`"HEngine.ScriptCore`"><HintPath>$core</HintPath></Reference></ItemGroup></Project>")
. (Join-Path $repo 'Tools/Agent/Storage.ps1')
$run = Start-HEStorageRun -Project (Join-Path $repo 'Project') -Workflow 'script-registry-dependencies' -Paths @($test) -Outer
$state = 'failed'
$results = @()
Push-Location (Join-Path $repo 'Engine/Managed')
try {
    foreach ($case in @('inherited-and-default','invalid-path')) {
        $dependency = if ($case -eq 'invalid-path') { '../escape.png' } else { 'Assets/dynamic.png' }
        $source = @"
using HEngine;
[ScriptDependency("Assets/shared.png")]
public abstract class Base : ScriptBehaviour {}
[ScriptType("test.dependencies"), ScriptDependency("$dependency")]
public sealed class Fixture : Base {
    [ScriptField("texture")] public ResourceReference Texture = new("Assets/shared.png");
    [ScriptField("emptyTexture")] public ResourceReference EmptyTexture;
    [ScriptField("emptyEntity")] public EntityReference EmptyEntity;
}
[ScriptType("sample.touch")]
public sealed class FileScript : ScriptBehaviour {
    public override void Update(double seconds) { Helper.Read(); }
}
public static class Helper {
    public static string Read() => System.IO.File.ReadAllText("not-opened-during-generation.txt");
}
[ScriptType("test.safe-math")]
public sealed class MathScript : ScriptBehaviour {
    public override void Update(double seconds) { System.Math.Clamp(seconds, 0, 1); }
}
[ScriptType("test.interface-access")]
public sealed class InterfaceScript : ScriptBehaviour {
    public override void Update(double seconds) { System.IDisposable item = new DisposableHelper(); item.Dispose(); }
}
public sealed class DisposableHelper : System.IDisposable {
    public void Dispose() { Helper.Read(); }
}
[ScriptType("test.erased-object")]
public sealed class ErasedScript : ScriptBehaviour {
    public override void Update(double seconds) { object value = new CustomText(); value.ToString(); }
}
public sealed class CustomText {
    public override string ToString() => Helper.Read();
}
[ScriptType("test.bcl-callback")]
public sealed class CallbackScript : ScriptBehaviour {
    public override void Update(double seconds) { System.Array.Sort(new object[] { new Comparable(), new Comparable() }); }
}
public sealed class Comparable : System.IComparable {
    public int CompareTo(object other) { Helper.Read(); return 0; }
}
"@
        [IO.File]::WriteAllText("$test/Fixture.cs", $source)
        & dotnet build "$test/Fixture.csproj" -c Release -o "$test/$case" *> "$test/$case-build.log"
        if ($LASTEXITCODE -ne 0) { throw "Dependency fixture compile failed: $case" }
        & dotnet "$work/generator/RegistryGenerator.dll" "$test/$case/DependencyFixture.dll" "$test/$case-registry" ('0' * 64) *> "$test/$case-generator.log"
        $code = $LASTEXITCODE
        if ($case -eq 'invalid-path') {
            $log = Get-Content "$test/$case-generator.log" -Raw
            if ($code -eq 0 -or $log -notmatch 'project-relative resource path' -or $log -notmatch 'Fixture') { throw 'Invalid dependency was not diagnosed.' }
        } else {
            if ($code -ne 0) { throw 'Valid dependencies did not generate.' }
            $metadata = Get-Content "$test/$case-registry/GameScripts.metadata.json" -Raw | ConvertFrom-Json
            $fixture = $metadata.types | Where-Object id -eq 'test.dependencies'
            if (($fixture.dependencies -join '|') -ne 'Assets/dynamic.png|Assets/shared.png') { throw 'Inherited/default dependencies were missing, duplicated or unstable.' }
            foreach ($id in @('emptyTexture','emptyEntity')) {
                $field = $fixture.fields | Where-Object id -eq $id
                if ($null -eq $field -or $null -eq $field.defaultValue.value -or $field.defaultValue.value -ne '') { throw "Uninitialized reference was not encoded as an empty stable ID: $id" }
            }
            $unsafeScript = $metadata.types | Where-Object id -eq 'sample.touch'
            $safeScript = $metadata.types | Where-Object id -eq 'test.safe-math'
            if ($unsafeScript.portableApiContract -ne 2 -or ($unsafeScript.portableDiagnostics -join '|') -notmatch 'Helper.Read.*System.IO.File.ReadAllText') { throw 'Indirect file access escaped portable IL audit.' }
            if ($safeScript.portableDiagnostics.Count -ne 0 -or $fixture.portableDiagnostics.Count -ne 0) { throw 'Unrelated unsafe script contaminated a portable script audit.' }
            $interfaceScript = $metadata.types | Where-Object id -eq 'test.interface-access'
            if (($interfaceScript.portableDiagnostics -join '|') -notmatch 'Helper.Read.*System.IO.File.ReadAllText') { throw 'Interface dispatch escaped portable IL audit.' }
            foreach ($id in @('test.erased-object','test.bcl-callback')) {
                $callbackScript = $metadata.types | Where-Object id -eq $id
                if (($callbackScript.portableDiagnostics -join '|') -notmatch 'Helper.Read.*System.IO.File.ReadAllText') { throw "Erased type/BCL callback escaped portable audit: $id" }
            }
        }
        $results += @{case=$case;status='PASS';exitCode=$code}
    }
    $state = 'success'
} finally {
    Pop-Location
    $results | ConvertTo-Json | Set-Content "$test/results.json"
    Complete-HEStorageRun -Run $run -State $state
}
Write-Host "unit-test generated dependency contracts PASS: $test"
