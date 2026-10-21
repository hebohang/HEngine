# 原创材质平台缓存：无窗口官方编译入口

2026-09-14：已在临时目录实际生成 DX12-only 缓存，未启动 GUI、HEngine 或 GPU，
未构建或修改发布资源。`EffekseerMaterialEditor.exe` 无可确认的无窗口编译 CLI：
1806 官方入口在参数解析后创建 MainWindow/OpenGL，故本轮没有启动该程序。

可执行入口是已安装 `EffekseerCoreGUI.dll` 的官方 SWIG 包装：
`Effekseer.swig.CompiledMaterialGenerator.Initialize(directory)`、`Compile(dst,src)`。
包装调用 Viewer.dll 的 CSharp_Effekseerfswig_CompiledMaterialGenerator_* 导出。
官方实现只加载目录中的材质编译 DLL，调用 MaterialFile/MaterialCompiler/CompiledMaterial；
构造函数不创建窗口或图形设备。DX12 compiler 通过 LLGI CPU 编译 shader。

上游源码依据：[MaterialEditor 入口](https://github.com/effekseer/Effekseer/blob/93dd29c7b4e353fcef6186fe4ac74c9757243550/Dev/Cpp/EffekseerMaterialEditor/effekseerMaterialEditor.cpp)、
[官方缓存生成器](https://github.com/effekseer/Effekseer/blob/93dd29c7b4e353fcef6186fe4ac74c9757243550/Dev/Cpp/Viewer/CompiledMaterialGenerator.cpp)、
[官方 C# 调用](https://github.com/effekseer/Effekseer/blob/93dd29c7b4e353fcef6186fe4ac74c9757243550/Dev/Editor/EffekseerCoreGUI/IO/MaterialCacheGenerator.cs)。

## 本机已执行的 PowerShell 路径

从仓库根启动独立 PowerShell 进程执行，变量和 DLL 查找路径仅影响该进程：

```powershell
$materialAudit = Join-Path ([IO.Path]::GetTempPath()) ('he-material-cache-audit-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $materialAudit | Out-Null
$materialTools = Join-Path $materialAudit 'dx12-tools'
New-Item -ItemType Directory -Path $materialTools | Out-Null
Copy-Item -LiteralPath 'Engine/Tools/Effekseer/bin/tools/EffekseerMaterialCompilerDX12.dll' -Destination $materialTools
Copy-Item -LiteralPath 'Project/SourceAssets/VFXAuthoring/MaterialCurveValidation/OriginalMaterial/OriginalCyan.efkmat' -Destination $materialAudit
$materialBin = (Resolve-Path 'Engine/Tools/Effekseer/bin').Path
$env:PATH = $materialBin + ';' + $env:PATH
[Environment]::CurrentDirectory = $materialBin
[void][Reflection.Assembly]::LoadFrom((Join-Path $materialBin 'EffekseerCoreGUI.dll'))
$materialGenerator = New-Object Effekseer.swig.CompiledMaterialGenerator
try {
    if (!$materialGenerator.Initialize($materialTools)) { throw 'Initialize failed' }
    if (!$materialGenerator.Compile((Join-Path $materialAudit 'OriginalCyan.efkmatd'), (Join-Path $materialAudit 'OriginalCyan.efkmat'))) { throw 'Compile failed' }
} finally { $materialGenerator.Dispose() }
```

必须使用新输出文件：官方生成器会按 GUID 保留已有平台缓存，不应把旧缓存当作本轮
shader 重编译证据。Initialize/Compile 的 true 也不足以证明平台齐全，需检查平台表
及标准/model VS/PS 非空。

## 实测结果

临时根：`C:/Users/51906/AppData/Local/Temp/he-material-cache-audit-1abae3f0522d43179f8818f12f3db91d`。
`OriginalCyan.efkmatd` 8615 字节，SHA256：
`4a2230b19e1f4bcec192f61613ecf586d2584922c7f12a8838804f1523d4398a`。
`audit.json` 保留逐 shader hash/长度，源码副本和 DLL 均保留。

- 容器 magic=eMCB，version=1612，platforms=[3]（DirectX12）。
- 嵌入 originalData 与已批准原创 `OriginalCyan.efkmat` 逐字节一致。
- Standard VS/PS 长度 2216/1428，Model VS/PS 3120/1428，均包含 DXBC。
- Refraction 四段为空，与该原创材质未启用 refraction 一致。
- SDK `Effekseer.CompiledMaterial.h` 平台号 DirectX12=3，Vulkan=30；
  容器版本由 CompiledMaterialVersion162=1612 决定。

本机 `tools` 只有 DX9/DX11/DX12/GL/Metal compiler DLL，没有
`EffekseerMaterialCompilerVK.dll`。SDK 有 Vulkan compiler 源码，可经 glslang 离线编译，
但本轮没有现成可调用 VK DLL，未构建、未宣称生成 Vulkan 缓存。

## 后续资源验收方案（尚未运行）

以原创效果和上述官方生成的缓存制作私有测试包，material 路径仍引用
OriginalCyan.efkmat，但包仅包含相应 `.efkmatd`，不附外部 `.efkmat`，以阻止源文件
fallback 掩盖结果。内嵌 originalData 是官方缓存必要结构，不能误删。

1. DX12：compiled-only 正例应加载和绘制，检查实际材质 draw/截图与清理。
2. Vulkan：同一个 DX12-only 包为 wrong-backend 负例，应明确失败且不留下 handle。
3. Vulkan compiled-only 正例仍需官方 VK 编译器产物；不能改 platform 标签或手写
   shader payload 冒充。可由后续授权的官方 SDK 离线 host 构建解决。

证据类型：离线官方编译及格式/字节验证 PASS；两后端 runtime 验收 NOT_RUN。

## 实际driver入口（尚未运行）

material_curve_acceptance.py 新增 `--compiled-cache <绝对路径/OriginalCyan.efkmatd> --compiled-cache-sha256 4a2230b19e1f4bcec192f61613ecf586d2584922c7f12a8838804f1523d4398a`。两项必须同传且hash匹配；缺省compiledCoverage=NOT_RUN，不能将source正例当compiled覆盖。缓存加入输入hash闭环。

私有临时包保留原efkefc及其他依赖，只移除唯一外部efkmat并添加同路径efkmatd。DX12真实play→2帧→截图/有效draw→stop，与source第2帧按通道delta>16像素差比较；退出画面与source已停止背景比较。Vulkan先加载source并pause，替换同key为compiled-only，要求reload明确拒绝且旧实例公开状态不变；同key新play证明原缓存仍在，再用fresh key要求wrong-backend拒绝。全部owned handle在finally清理，外层原Session/Storage生命周期继续覆盖。Vulkan compiled-only正例仍NOT_RUN，不伪造平台缓存。

unit-test：`py -3.11 Tools/VFX/material_curve_acceptance_test.py` PASS（3 tests，私有包结构/efkefc保持、缺失歧义依赖、两后端driver mock分支及清理）；语法检查PASS。此为driver准备，未build/GPU/实际runtime。
