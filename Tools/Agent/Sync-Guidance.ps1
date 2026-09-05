$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$canonicalGuide = Join-Path $repositoryRoot "AGENTS.md"

Copy-Item -LiteralPath $canonicalGuide -Destination (Join-Path $repositoryRoot "CLAUDE.md") -Force
Copy-Item -LiteralPath $canonicalGuide -Destination (Join-Path $repositoryRoot ".github\copilot-instructions.md") -Force

$skillNames = @(
    "he-build-validate",
    "he-editor-imgui-dpi",
    "he-pack-game",
    "he-reflection-codegen",
    "he-resource-reflection-serialization",
    "he-single-testlayer-run",
    "he-vs-shared-build"
)

foreach ($skillName in $skillNames) {
    $source = Join-Path $repositoryRoot ".agents\skills\$skillName\SKILL.md"
    $destination = Join-Path $repositoryRoot ".claude\skills\$skillName\SKILL.md"
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Canonical skill is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

Write-Host "Synced AGENTS.md and 7 canonical skill files."
