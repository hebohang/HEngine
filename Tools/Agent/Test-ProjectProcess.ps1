$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$work=Join-Path $repo ('out/agent/project-process-test-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Force $work
Add-Type -Path "$PSScriptRoot/ProjectProcess.cs"
# Exercise the actual argument/encoding bootstrap without opening an Editor.
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile("$PSScriptRoot/ProjectLaunch.ps1",[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw $errors[0] }
$definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-ProjectBuild'},$true)
. ([scriptblock]::Create($definition.Extent.Text))
$repositoryRoot=$repo
$Log=Join-Path $work 'build.log'
$child=Join-Path $work "中文 space's.ps1"
$source=@'
param([string]$Project,[switch]$EditorHost)
if ($Project -cne "中文 项目's" -or !$EditorHost) { exit 8 }
Write-Host '正在编译项目：中文路径'
Write-Output '标准输出：构建完成'
[Console]::Error.WriteLine('标准错误：警告信息')
& cmd.exe /d /c echo 原生输出：中文路径
for ($i=0; $i -lt 1500; $i++) {
    [Console]::Out.WriteLine("输出 $i " + ('x'*80))
    [Console]::Error.WriteLine("错误 $i " + ('y'*80))
}
exit 7
'@
[IO.File]::WriteAllText($child,$source,[Text.UTF8Encoding]::new($true))
$code=Invoke-ProjectBuild $child @('-Project',"中文 项目's",'-EditorHost')
if ($code -ne 7) { throw "Child exit code changed: $code" }
$text=Get-Content -LiteralPath $Log -Raw -Encoding UTF8
foreach ($expected in @('正在编译项目：中文路径','标准输出：构建完成','标准错误：警告信息','原生输出：中文路径','输出 1499','错误 1499')) {
    if (!$text.Contains($expected)) { throw "Unicode output missing: $expected" }
}
if ($text.Contains('#< CLIXML') -or $text.Contains('#<CLIXML') -or $text.Contains([string][char]0xfffd)) { throw 'Log contains serialization markup or replacement characters.' }
[IO.File]::WriteAllText($child,"throw '启动失败：中文原因'",[Text.UTF8Encoding]::new($true))
if ((Invoke-ProjectBuild $child @()) -ne 1) { throw 'Terminating script failure was reported as success.' }
if (!(Get-Content $Log -Raw -Encoding UTF8).Contains('启动失败：中文原因')) { throw 'Failure details lost encoding.' }
Write-Output "unit-test PASS: UTF-8 stdout/stderr/native output, concurrent pipe drain, quoted arguments and failure exit codes. Evidence: $work"
