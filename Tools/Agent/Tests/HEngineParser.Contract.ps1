[CmdletBinding()]
param(
    [string]$ParserExecutable = ""
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\.."))
$fixtureRoot = Join-Path $repositoryRoot "Engine\Source\Programs\HEngineParser\Tests\Fixtures"
$templateRoot = Join-Path $repositoryRoot "Engine\Source\Programs\HEngineParser\Template"

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Text)
    $parent = [System.IO.Path]::GetDirectoryName($Path)
    if (-not [System.IO.Directory]::Exists($parent)) {
        [void][System.IO.Directory]::CreateDirectory($parent)
    }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Get-TreeSnapshot {
    param([string]$Root)
    $records = @{}
    $rootPrefix = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse -Force |
        Where-Object { $_.Name -ne ".he-codegen-index.json" -and $_.FullName -notmatch "[\\/]\.he-codegen[\\/]" }) {
        Assert-True ($file.FullName.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) "snapshot file escaped root"
        $relative = $file.FullName.Substring($rootPrefix.Length).Replace('\', '/')
        $records[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $records
}

function Invoke-Parser {
    param(
        [string]$InputList,
        [string]$Context,
        [string]$OutputRoot,
        [string]$Templates = $templateRoot,
        [switch]$Quiet
    )
    $controlRoot = Join-Path $OutputRoot ".he-codegen"
    [void][System.IO.Directory]::CreateDirectory($controlRoot)
    $arguments = @(
        "--input-list", $InputList,
        "--input-header", (Join-Path $controlRoot "input.h"),
        "--output-root", $OutputRoot,
        "--context", $Context,
        "--templates", $Templates,
        "--index", (Join-Path $OutputRoot ".he-codegen-index.json"),
        "--module", "Contract")
    if ($Quiet) {
        $arguments += "--quiet"
    }
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $captured = @(& $script:Parser @arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    return [pscustomobject]@{
        exitCode = $exitCode
        output = (@($captured | ForEach-Object { [string]$_ }) -join [Environment]::NewLine)
        indexPath = Join-Path $OutputRoot ".he-codegen-index.json"
    }
}

if ([string]::IsNullOrWhiteSpace($ParserExecutable)) {
    $candidates = @(Get-ChildItem -LiteralPath (Join-Path $repositoryRoot "out\agent") `
        -Filter HEngineParser.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch "[\\/]CMakeFiles[\\/]" } |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($candidates.Count -eq 0) {
        throw "HEngineParser was not found under out/agent; build the tool first."
    }
    $ParserExecutable = $candidates[0].FullName
}
$script:Parser = [System.IO.Path]::GetFullPath($ParserExecutable)
Assert-True (Test-Path -LiteralPath $script:Parser -PathType Leaf) "Parser does not exist: $script:Parser"

$temporaryBase = [System.IO.Path]::GetTempPath().TrimEnd('\', '/')
$testRoot = Join-Path $temporaryBase ("HEngineParser-Contract-" + [guid]::NewGuid().ToString("N"))

try {
    $contractSource = Join-Path $fixtureRoot "Contract"
    $contractCopy = Join-Path $testRoot "Contract"
    Copy-Item -LiteralPath $contractSource -Destination $contractCopy -Recurse
    $contractContext = Join-Path $contractCopy "context.txt"
    Write-Utf8NoBom -Path $contractContext -Text ((@(
        "include=$($contractCopy.Replace('\', '/'))",
        "sourceRoot=$($contractCopy.Replace('\', '/'))|fixture") -join "`n") + "`n")
    $contractInputs = Join-Path $contractCopy "input-list.txt"
    Write-Utf8NoBom -Path $contractInputs -Text ((@(
        (Join-Path $contractCopy "Model\Types.h"),
        (Join-Path $contractCopy "B\Foo.h"),
        (Join-Path $contractCopy "A\Foo.h")) -join ";") + ";`n")

    $deepRoot = Join-Path $testRoot "deep\one\two\GeneratedSource"
    $contract = Invoke-Parser -InputList $contractInputs -Context $contractContext -OutputRoot $deepRoot
    Assert-True ($contract.exitCode -eq 0) "contract generation failed: $($contract.output)"
    $index = Get-Content -LiteralPath $contract.indexPath -Raw | ConvertFrom-Json
    Assert-True ([int]$index.schemaVersion -eq 1) "unexpected index schema"
    Assert-True (@($index.types).Count -eq 7) "expected seven reflected fixture types"
    Assert-True (@($index.outputs).Count -eq 15) "unexpected fixture output count"
    $foos = @($index.types | Where-Object { $_.legacyName -eq "Foo" })
    Assert-True ($foos.Count -eq 2) "same short name fixture did not preserve both types"
    Assert-True ($foos[0].identity -ne $foos[1].identity) "same short name identities collided"
    Assert-True (-not (@($index.outputs) -contains "_generated/serializer/Foo.gen.h")) "ambiguous basename emitted a compatibility shim"

    $child = $index.types | Where-Object { $_.qualifiedName -eq "Model::Child" }
    $dependencyKinds = @($child.dependencies | ForEach-Object { $_.kind } | Sort-Object)
    Assert-True (($dependencyKinds -join ",") -eq "base,base,container,container,field,field") "dependency IR is incomplete"
    $typesHeader = Get-Content -LiteralPath (Join-Path $deepRoot "_generated\serializer\fixture\Model\Types.gen.h") -Raw
    Assert-True ($typesHeader.Contains("fixture/A/Foo.gen.h")) "normal field dependency declaration missing"
    Assert-True ($typesHeader.Contains("fixture/B/Foo.gen.h")) "container dependency declaration missing"
    $typesSource = Get-Content -LiteralPath (Join-Path $deepRoot "_generated\serializer\fixture\Model\Types.gen.hpp") -Raw
    Assert-True ($typesSource.Contains('json["display:key"]')) "serializedName metadata with colon was not preserved"
    Assert-True ($typesSource.Contains('json["a&b"]')) "serializedName was HTML escaped"
    Assert-True ($typesSource.Contains('escaped\\\"quote')) "serializedName metadata with an escaped quote was not preserved"
    Assert-True ($typesSource.Contains('json["comma,key"]')) "metadata comma value was not preserved"
    Assert-True ($typesSource.Contains('json["Alpha::Foo"]')) "metadata namespace value was not preserved"
    Assert-True ($typesSource.Contains('json["[0,1]"]')) "metadata bracket value was not preserved"
    Assert-True ($typesSource.Contains('json["Factory(1,2)"]')) "metadata parenthesized value was not preserved"
    Assert-True ($typesSource.Contains('json["Nested(Alpha::Foo, Factory(1,2), [0,1], {3,4})"]')) "nested metadata value was not preserved"
    Assert-True ($typesSource.Contains("static_cast<const Model::Left&>")) "first base serializer missing"
    Assert-True ($typesSource.Contains("static_cast<const Model::Right&>")) "second base serializer missing"
    $typesReflection = Get-Content -LiteralPath (Join-Path $deepRoot "_generated\reflection\fixture\Model\Types.reflection.gen.h") -Raw
    $localVectorLines = @($typesReflection -split "`n" | Where-Object { $_ -match 'm_locals|std::vector' }) -join ' | '
    Assert-True ($typesReflection -match 'std::vector<Model::LocalValue,\s*std::Alloc<Model::LocalValue>\s*>') "same-namespace/custom-allocator vector type was not qualified and preserved: $localVectorLines"
    Assert-True ($typesReflection.Contains("outList[0]")) "first base reflection slot missing"
    Assert-True ($typesReflection.Contains("outList[1]")) "second base reflection slot missing"
    Assert-True ($typesReflection.Contains('REGISTER_BASE_CLASS_TO_MAP("Model::Empty"')) "empty reflected class was not registered"

    $shallowRoot = Join-Path $testRoot "GeneratedSource"
    Write-Utf8NoBom -Path $contractInputs -Text ((@(
        (Join-Path $contractCopy "A\Foo.h"),
        (Join-Path $contractCopy "B\Foo.h"),
        (Join-Path $contractCopy "Model\Types.h")) -join ";") + ";`n")
    $shallow = Invoke-Parser -InputList $contractInputs -Context $contractContext -OutputRoot $shallowRoot -Quiet
    Assert-True ($shallow.exitCode -eq 0) "shallow generation failed: $($shallow.output)"
    $deepSnapshot = Get-TreeSnapshot -Root $deepRoot
    $shallowSnapshot = Get-TreeSnapshot -Root $shallowRoot
    Assert-True ($deepSnapshot.Count -eq $shallowSnapshot.Count) "output depth changed file count"
    foreach ($relative in $deepSnapshot.Keys) {
        Assert-True ($shallowSnapshot.ContainsKey($relative)) "output depth lost $relative"
        Assert-True ($deepSnapshot[$relative] -eq $shallowSnapshot[$relative]) "output depth changed content for $relative"
    }

    $projectSliceSource = Join-Path $fixtureRoot "ProjectSlice"
    $projectSlice = Join-Path $testRoot "ProjectSlice"
    Copy-Item -LiteralPath $projectSliceSource -Destination $projectSlice -Recurse
    $projectSliceOutput = Join-Path $testRoot "ProjectSliceOutput"
    $projectSliceResult = Invoke-Parser `
        -InputList (Join-Path $projectSlice "input-list.txt") `
        -Context (Join-Path $projectSlice "context.txt") `
        -OutputRoot $projectSliceOutput
    Assert-True ($projectSliceResult.exitCode -eq 0) `
        "real Material project slice generation failed: $($projectSliceResult.output)"
    $projectSliceIndex = Get-Content -LiteralPath $projectSliceResult.indexPath -Raw | ConvertFrom-Json
    Assert-True (@($projectSliceIndex.types | Where-Object { $_.qualifiedName -eq "HEngine::MaterialRes" }).Count -eq 1) `
        "real Material project slice did not produce the expected FQN"

    $failureSource = Join-Path $fixtureRoot "Failure"
    $failureCopy = Join-Path $testRoot "Failure"
    Copy-Item -LiteralPath $failureSource -Destination $failureCopy -Recurse
    $failureContext = Join-Path $failureCopy "context.txt"
    Write-Utf8NoBom -Path $failureContext -Text ("include=$($failureCopy.Replace('\', '/'))`nsourceRoot=$($failureCopy.Replace('\', '/'))|fixture`n")
    $failureInputs = Join-Path $failureCopy "input-list.txt"
    Write-Utf8NoBom -Path $failureInputs -Text ((Join-Path $failureCopy "Broken.h") + ";`n")
    $failureRoot = Join-Path $testRoot "FailureOutput"
    $failure = Invoke-Parser -InputList $failureInputs -Context $failureContext -OutputRoot $failureRoot
    Assert-True ($failure.exitCode -ne 0) "syntax error fixture unexpectedly succeeded"
    Assert-True ($failure.output -match "HE_CODEGEN_DIAGNOSTIC\|error\|.*Broken\.h\|10\|") "syntax diagnostic did not map to Broken.h:10"
    Assert-True (-not (Test-Path -LiteralPath $failure.indexPath)) "syntax failure published an index"

    $metadataDuplicateInputs = Join-Path $failureCopy "metadata-duplicate-inputs.txt"
    Write-Utf8NoBom -Path $metadataDuplicateInputs -Text ((Join-Path $failureCopy "MetadataDuplicate.h") + ";`n")
    $metadataDuplicate = Invoke-Parser -InputList $metadataDuplicateInputs -Context $failureContext `
        -OutputRoot (Join-Path $testRoot "MetadataDuplicateOutput")
    Assert-True ($metadataDuplicate.exitCode -ne 0) "duplicate metadata key unexpectedly succeeded"
    Assert-True ($metadataDuplicate.output -match `
        "HE_CODEGEN_DIAGNOSTIC\|error\|.*MetadataDuplicate\.h\|[1-9][0-9]*\|[1-9][0-9]*\|duplicate metadata key 'DisplayName'") `
        "duplicate metadata key lacked a source-level diagnostic: $($metadataDuplicate.output)"
    Assert-True (-not (Test-Path -LiteralPath $metadataDuplicate.indexPath)) "duplicate metadata failure published an index"

    $metadataMalformedInputs = Join-Path $failureCopy "metadata-malformed-inputs.txt"
    Write-Utf8NoBom -Path $metadataMalformedInputs -Text ((Join-Path $failureCopy "MetadataMalformed.h") + ";`n")
    $metadataMalformed = Invoke-Parser -InputList $metadataMalformedInputs -Context $failureContext `
        -OutputRoot (Join-Path $testRoot "MetadataMalformedOutput")
    Assert-True ($metadataMalformed.exitCode -ne 0) "malformed metadata unexpectedly succeeded"
    foreach ($expectedMetadataDiagnostic in @(
        "unclosed double quote",
        "unclosed bracket '\['",
        "unclosed brace '\{'",
        "unclosed parenthesis '\('") ) {
        Assert-True ($metadataMalformed.output -match `
            ("HE_CODEGEN_DIAGNOSTIC\|error\|.*MetadataMalformed\.h\|[1-9][0-9]*\|[1-9][0-9]*\|metadata contains an " + `
                $expectedMetadataDiagnostic)) `
            "malformed metadata lacked '$expectedMetadataDiagnostic' source diagnostic: $($metadataMalformed.output)"
    }
    Assert-True (-not (Test-Path -LiteralPath $metadataMalformed.indexPath)) "malformed metadata failure published an index"

    $missingTemplates = Invoke-Parser -InputList $contractInputs -Context $contractContext `
        -OutputRoot (Join-Path $testRoot "MissingTemplateOutput") -Templates (Join-Path $testRoot "missing-templates")
    Assert-True ($missingTemplates.exitCode -ne 0) "missing templates unexpectedly succeeded"

    $emptyTemplateRoot = Join-Path $testRoot "EmptyTemplates"
    Copy-Item -LiteralPath $templateRoot -Destination $emptyTemplateRoot -Recurse
    [System.IO.File]::WriteAllBytes((Join-Path $emptyTemplateRoot "commonReflectionFile.mustache"), [byte[]]@())
    $emptyTemplate = Invoke-Parser -InputList $contractInputs -Context $contractContext `
        -OutputRoot (Join-Path $testRoot "EmptyTemplateOutput") -Templates $emptyTemplateRoot
    Assert-True ($emptyTemplate.exitCode -ne 0) "empty template unexpectedly succeeded"

    $invalidTemplateRoot = Join-Path $testRoot "InvalidTemplates"
    Copy-Item -LiteralPath $templateRoot -Destination $invalidTemplateRoot -Recurse
    Write-Utf8NoBom -Path (Join-Path $invalidTemplateRoot "commonReflectionFile.mustache") -Text "{{#unclosed}}`n"
    $invalidTemplate = Invoke-Parser -InputList $contractInputs -Context $contractContext `
        -OutputRoot (Join-Path $testRoot "InvalidTemplateOutput") -Templates $invalidTemplateRoot
    Assert-True ($invalidTemplate.exitCode -ne 0) "invalid mustache template unexpectedly succeeded"

    $unsupportedRoot = Join-Path $testRoot "Unsupported"
    [void][System.IO.Directory]::CreateDirectory($unsupportedRoot)
    $unsupportedHeader = Join-Path $unsupportedRoot "Unsupported.h"
    Write-Utf8NoBom -Path $unsupportedHeader -Text @'
#pragma once
#define HE_TEST_REFLECT(...) __attribute__((annotate(#__VA_ARGS__)))
#define HE_TEST_PROPERTY(...) __attribute__((annotate(#__VA_ARGS__)))
class HE_TEST_REFLECT(Fields) ConstField
{
public:
    const int value = 1;
};
class HE_TEST_REFLECT(Fields) NoDefault
{
public:
    explicit NoDefault(int) {}
    int value = 0;
};
class HE_TEST_REFLECT(Fields) DeletedDefault
{
public:
    DeletedDefault() = delete;
    int value = 0;
};
class HE_TEST_REFLECT(Fields) PointerField
{
public:
    int* value = nullptr;
};
class HE_TEST_REFLECT(Fields) DuplicateKeys
{
public:
    HE_TEST_PROPERTY(serializedName : "same") int first = 0;
    HE_TEST_PROPERTY(serializedName : "same") int second = 0;
};
'@
    $unsupportedInputs = Join-Path $unsupportedRoot "inputs.txt"
    Write-Utf8NoBom -Path $unsupportedInputs -Text ($unsupportedHeader + ";`n")
    $unsupportedContext = Join-Path $unsupportedRoot "context.txt"
    Write-Utf8NoBom -Path $unsupportedContext -Text ("include=$($unsupportedRoot.Replace('\', '/'))`nsourceRoot=$($unsupportedRoot.Replace('\', '/'))|fixture`n")
    $unsupported = Invoke-Parser -InputList $unsupportedInputs -Context $unsupportedContext `
        -OutputRoot (Join-Path $testRoot "UnsupportedOutput")
    Assert-True ($unsupported.exitCode -ne 0) "unsupported const/no-default fixture unexpectedly succeeded"
    Assert-True ($unsupported.output -match "const reflected field") "const field lacked source-level diagnostic"
    Assert-True ($unsupported.output -match "default constructor") "deleted/no-default constructor lacked source-level diagnostic"
    Assert-True ($unsupported.output -match "pointer reflected field") "pointer field lacked source-level diagnostic"
    Assert-True ($unsupported.output -match "duplicate flattened serialized key") "duplicate serialized key lacked source-level diagnostic"

    $traitRoot = Join-Path $testRoot "Traits"
    [void][System.IO.Directory]::CreateDirectory($traitRoot)
    $traitHeader = Join-Path $traitRoot "Traits.h"
    Write-Utf8NoBom -Path $traitHeader -Text @'
#pragma once
#define HE_TEST_REFLECT(...) __attribute__((annotate(#__VA_ARGS__)))
class DeletedAssignmentValue
{
public:
    DeletedAssignmentValue() = default;
    DeletedAssignmentValue& operator=(const DeletedAssignmentValue&) = delete;
};
class HE_TEST_REFLECT(Fields) DeletedAssignmentOwner
{
public:
    DeletedAssignmentValue value;
};
class NonDefaultMember
{
public:
    explicit NonDefaultMember(int) {}
    NonDefaultMember& operator=(const NonDefaultMember&) = default;
};
class HE_TEST_REFLECT(Fields) ImplicitDeletedDefault
{
public:
    NonDefaultMember value;
};
'@
    $traitInputs = Join-Path $traitRoot "inputs.txt"
    Write-Utf8NoBom -Path $traitInputs -Text ($traitHeader + ";`n")
    $traitContext = Join-Path $traitRoot "context.txt"
    Write-Utf8NoBom -Path $traitContext -Text ("include=$($traitRoot.Replace('\', '/'))`nsourceRoot=$($traitRoot.Replace('\', '/'))|fixture`n")
    $traits = Invoke-Parser -InputList $traitInputs -Context $traitContext `
        -OutputRoot (Join-Path $testRoot "TraitsOutput")
    Assert-True ($traits.exitCode -ne 0) "deleted assignment/default-constructor fixture unexpectedly succeeded"
    Assert-True ($traits.output -match "HE_CODEGEN_DIAGNOSTIC\|error\|.*Traits\.h\|[1-9][0-9]*\|.*not assignable.*DeletedAssignmentOwner::value") `
        "deleted assignment lacked a source-level codegen diagnostic: $($traits.output)"
    Assert-True ($traits.output -match "HE_CODEGEN_DIAGNOSTIC\|error\|.*Traits\.h\|[1-9][0-9]*\|.*not default constructible.*ImplicitDeletedDefault") `
        "implicit deleted default constructor lacked a source-level codegen diagnostic: $($traits.output)"

    $cycleSource = Join-Path $fixtureRoot "Cycles"
    $cycleRoot = Join-Path $testRoot "Cycles"
    Copy-Item -LiteralPath $cycleSource -Destination $cycleRoot -Recurse
    $cycleContext = Join-Path $cycleRoot "context.txt"
    Write-Utf8NoBom -Path $cycleContext -Text ("include=$($cycleRoot.Replace('\', '/'))`nsourceRoot=$($cycleRoot.Replace('\', '/'))|fixture`n")
    $cycleInputs = Join-Path $cycleRoot "inputs.txt"
    Write-Utf8NoBom -Path $cycleInputs -Text (((Join-Path $cycleRoot "A.h"), (Join-Path $cycleRoot "B.h")) -join ";" | ForEach-Object { $_ + ";`n" })
    $cycleOutput = Join-Path $testRoot "CycleOutput"
    $legalCycle = Invoke-Parser -InputList $cycleInputs -Context $cycleContext -OutputRoot $cycleOutput
    Assert-True ($legalCycle.exitCode -eq 0) "legal vector dependency cycle failed: $($legalCycle.output)"
    $cycleIndex = Get-Content -LiteralPath $legalCycle.indexPath -Raw | ConvertFrom-Json
    Assert-True (@($cycleIndex.types).Count -eq 2) "legal vector cycle lost a reflected type"
    foreach ($cycleType in @($cycleIndex.types)) {
        Assert-True (@($cycleType.dependencies).Count -eq 1 -and $cycleType.dependencies[0].kind -eq "container") `
            "legal vector cycle was not modeled as a weak container edge"
    }
    $invalidCycleInputs = Join-Path $cycleRoot "invalid-inputs.txt"
    Write-Utf8NoBom -Path $invalidCycleInputs -Text ((Join-Path $cycleRoot "Invalid.h") + ";`n")
    $invalidCycle = Invoke-Parser -InputList $invalidCycleInputs -Context $cycleContext `
        -OutputRoot (Join-Path $testRoot "InvalidCycleOutput")
    Assert-True ($invalidCycle.exitCode -ne 0) "invalid strong dependency cycle unexpectedly succeeded"
    Assert-True ($invalidCycle.output -match "HE_CODEGEN_DIAGNOSTIC\|error\|.*Invalid\.h\|[1-9][0-9]*\|") `
        "invalid dependency cycle lacked a source-level codegen diagnostic"

    $diamondRoot = Join-Path $testRoot "Diamond"
    [void][System.IO.Directory]::CreateDirectory($diamondRoot)
    $diamondHeader = Join-Path $diamondRoot "Diamond.h"
    Write-Utf8NoBom -Path $diamondHeader -Text @'
#pragma once
#define HE_TEST_REFLECT(...) __attribute__((annotate(#__VA_ARGS__)))
class HE_TEST_REFLECT(Fields) CommonBase { public: int shared = 0; };
class HE_TEST_REFLECT(Fields) BranchA : public CommonBase { public: int a = 0; };
class HE_TEST_REFLECT(Fields) BranchB : public CommonBase { public: int b = 0; };
class HE_TEST_REFLECT(Fields) Diamond : public BranchA, public BranchB { public: int own = 0; };
'@
    $diamondInputs = Join-Path $diamondRoot "inputs.txt"
    Write-Utf8NoBom -Path $diamondInputs -Text ($diamondHeader + ";`n")
    $diamondContext = Join-Path $diamondRoot "context.txt"
    Write-Utf8NoBom -Path $diamondContext -Text ("include=$($diamondRoot.Replace('\', '/'))`nsourceRoot=$($diamondRoot.Replace('\', '/'))|fixture`n")
    $diamond = Invoke-Parser -InputList $diamondInputs -Context $diamondContext `
        -OutputRoot (Join-Path $testRoot "DiamondOutput")
    Assert-True ($diamond.exitCode -ne 0) "non-virtual diamond key collision unexpectedly succeeded"
    Assert-True ($diamond.output -match "duplicate flattened serialized key 'shared'") "diamond key collision lacked codegen diagnostic"

    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $noArguments = @(& $script:Parser 2>&1)
        $noArgumentsExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    Assert-True ($noArgumentsExitCode -ne 0) "empty parser arguments unexpectedly succeeded"

    [Console]::Out.WriteLine(
        "HEngineParser contract: PASS (clang diagnostics, FQN/USR collisions, dependencies/cycles, metadata boundaries/duplicates, multi-base, empty type, depth determinism, templates, type traits, argc)")
}
finally {
    $fullTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $allowedPrefix = [System.IO.Path]::GetFullPath($temporaryBase).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if ($fullTestRoot.StartsWith($allowedPrefix, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Path]::GetFileName($fullTestRoot).StartsWith("HEngineParser-Contract-", [System.StringComparison]::Ordinal)) {
        if ([System.IO.Directory]::Exists($fullTestRoot)) {
            Remove-Item -LiteralPath $fullTestRoot -Recurse -Force
        }
    }
}
