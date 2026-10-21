# GpuSparks20k 非法 CPU filter=331 的资源只读诊断

2026-09-14。本轮未修改源码或发布资源，未启动 HEngine/GPU、未构建。
仅用当前官方 Effekseer 工具在临时目录读取 EDIT、重新编译并做差分定位。

## 结论

没有发现资源把 CPU sampler 写成 331。原官方样例、派生 GpuSparks20k 与官方
重编译版本的 CPU renderer-common 均为七组 `(filter=1, wrap=0)`，即 Linear/Repeat。
已发布派生文件的 CPU 参数前缀除 seed/owner lifetime 外完全保留官方原样例。
这支持继续排查运行时状态写入、复制、数组/ABI或加载解释路径；不能只靠此静态检查
锁定具体运行时 bug，也不应盲目替换压力包来隐藏错误。

## 来源与版本

- 生成器 `Tools/VFX/generate_effekseer_stress_assets.py:37` 固定官方样例 SHA256：
  `f4e94742a8c1b4dab03f4c427b07cb0a4a5650139b95f4fd6584e300326f79ce`。
- `patch_runtime_chunk`（约 379 行）验证 GPU 参数块从 BIN offset 1003 起与已知构造
  完全一致，再复制 1003 字节前缀，只写 seed 与 owner lifetime。
- 官方原样例与派生包 BIN 版本均为 1800；当前官方工具重编译输出为 1810。
  SDK `Utils/Effekseer.BinaryVersion.h` 明确把二者定义为 Version18Alpha1/Version18。
  发布标签 1.80.6 不意味着所有示例使用同一最终二进制格式。
- 原样例 BIN 长 1262，重编译 1309；派生 BIN 长 1430，重编译 1309。
  因版本与 GPU 参数导出变化，重编译并非可直接发布的字节等价替换。

## CPU 节点与 common 参数

官方与派生 EDIT 均只有一个 Node：GpuParticles/Enabled=True，同时默认
DrawingValues/Type=2（sprite）。没有第二个独立 CPU-only 节点；同一节点仍包含
CPU sprite/common 数据。RendererCommonValues 显式字段只有 ColorTexture、
AlphaBlend=2、FadeOut/Frame=0，Filter/Wrap 使用默认值。

SDK `Effekseer.EffectNode.cpp:235` 先加载 renderer-common，约 306 行再读取该节点
GPU 开关/参数。因此 GPU-enabled 不等于不存在 CPU renderer common。
`Effekseer.Base.Pre.h:194` 的合法 filter 仅 Nearest=0、Linear=1；wrap 为 0/1/2。
`Effekseer.EffectNode.h:236` 起按版本读取 sampler；>=1600 读取七组。
数组本身以 `{}` 初始化，未序列化的其余 slot 保留零初始化值。

定位不依赖猜测 offset：读取官方 EDIT 后官方重编译；再仅把
RendererCommonValues/Filter 设为 0，再编译。两份 BIN 长度一致，唯一不同字节是
offset 810，从 1 变 0。这证明默认 filter0 的位置。

| 资源 | renderer-common 段起点 | sampler 起点 | 七组 filter/wrap |
| --- | ---: | ---: | --- |
| 原官方 1800 | 727 | 767 | `(1,0)` × 7 |
| 派生 20k 1800 | 727 | 767 | `(1,0)` × 7 |
| 官方重编译 1810 | 770 | 810 | `(1,0)` × 7 |

重编译版本包含 material/emissive/texture indexes/blend/sampler 的 96 字节连续段，
在原官方 BIN 中只有一个完全相同的匹配。派生前缀实际变化 byte offsets 为
126/127/128/129、226/227、230/231；与生成器承诺的 seed/lifetime 一致。
前缀任意 byte 对齐 int32 搜索也没有 331；该搜索仅补充证据，主要依据是已定位的
真实 sampler 字段。

## 保留证据及命令

临时目录（有意保留）：
`C:/Users/51906/AppData/Local/Temp/he-gpu-common-audit-qgg_uxo4`。
`report.json` 含原始/重编译 INFO、EDIT、BIN hash 与长度、EDIT common；
`cpu-common.json` 含差分定位、七组字段值、前缀差异。
`official` 和 `derived` 子目录保留原文件、decoded.efkproj 和官方重编译文件；
`official/probe-nearest.*` 为单字段差分探针，绝非待发布资产。

实际调用形式：

```powershell
pwsh -NoProfile -File Tools/VFX/recipes/InspectCompiled.ps1 -InputFile <临时原始efkefc> -OutputFile <临时decoded.efkproj> -CoreAssembly Engine/Tools/Effekseer/bin/EffekseerCore.dll
Engine/Tools/Effekseer/bin/Effekseer.exe -cui -in <临时decoded.efkproj> -o <临时official-recompiled.efkefc>
```

官方源重编译容器 SHA256：
`872b6ad70dc0711804eb1c40b740c16652863332cfd492ceced1ddca9a5ef66f`。
派生原容器 SHA256：
`f1540dba1d1f0d52dea7ee28ef2ec8fec2c507111ce5454a7baa46ed0a27c3f0`。
派生 EDIT 重编译容器 SHA256：
`42f279c0010a2e44fde77e8bf536eb628586e269656d6a001d5dd0d6eb686cc7`。

证据类型：离线资源转换/字节检查 PASS；运行时根因定位 NOT_RUN。
