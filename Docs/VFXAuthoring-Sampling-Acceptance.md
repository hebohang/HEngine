# 阶段 6C 采样导入与截图验收

## 当前实际状态（2026-09-14）

已完成原生导入与实际GPU采样验收，状态按专项区分：

| 专项 | 当前证据 | 结论与边界 |
| --- | --- | --- |
| Raster overview | `sampling-vulkan-05/report.json`、`sampling-dx12-01/report.json` | 两后端七阶段命令及人工视觉PASS |
| Raster min/mag | `sampling-filter-vulkan-01/report.json`、`sampling-filter-dx12-01/report.json` | 两后端命令及人工视觉PASS；不代表所有LOD逐像素等价 |
| DepthShadow | `material-depth-shadow-vulkan-01/report.json`、`material-depth-shadow-dx12-01/report.json` | 每后端10/10实际比较PASS，nativeErrors为空；限CPU opaque/MASK的Depth与DirectionalShadow slice0 |
| Reference | `material-reference-dx12-03/report.json` | DX12实际22/22 PASS，availability=EXECUTED，nativeErrors为空 |
| Reference | `material-reference-vulkan-02/report.json` | status=FAIL、0项执行；availability=NOT_SUPPORTED_BY_CURRENT_BACKEND_PROFILE。当前Vulkan profile不支持该RT路径，不能标PASS |

Raster报告位于 `out/agent/rendering-evidence/vfx-authoring/`；DepthShadow与Reference报告位于 `out/agent/vfx-authoring/`。Reference DX12旧01/02及早期采样失败保留为历史证据，不能覆盖最新03结果，也不能用DX12通过推导Vulkan支持。以上均为 `runtime-manual` 证据；具体候选SHA、原始回执和命令以各报告为准。

已有
`Tools/Content/generate_sampling_fixture.py` 的 12 面板、8×8 定量网格纹理、2×2
法线纹理足够复用，不需要新导入器。原始
`Project/SourceAssets/VFXAuthoring/Sampling` 不会被驱动改写。

## 当前覆盖与缺口

| 入口 | 实际覆盖 | 不代表 |
| --- | --- | --- |
| `test_sampling_fixture.py` | 小源资产内容、稳定身份、无变化生成 | 原生导入/GPU |
| `test_sampling_reference.py` | sRGB 解码后插值、负 UV、mirror、min/mag/mip 独立数学参考 | 渲染器实际像素 |
| `test_sampling_import.py` | 实际原生转换、UV1/color/transform/sampler 字段、内存 merge 覆盖保留、UV3 拒绝 | 完整 publish/journal 或截图 |
| `sampling_acceptance.py` | 调用原有 `content_sync.synchronize`，包含原生转换、`--validate-scene`、资源 hash/引用检查、事务发布；七阶段及 UV3 拒绝；三次真实 Game 截图 | 自动视觉通过、RT/depth/shadow 专项图像或性能测试 |

驱动按 initial → noop → material → geometry → rename → add → delete 发布。
noop 要求 changed=false、newResources=0、场景 hash/mtime 不动。其余阶段核对
发布资源 hash、UV1/COLOR_0、六种 minFilter、UV1 selector、transform rotation、
baseColorFactor、实体数量、改名结果及几何资源变化。noop 后向 `uv0` 实体写入真实
已有脚本路径和一个真实已发布材质覆盖，后续每阶段验证两者保留。
UV3 必须由 texture_sampling 诊断拒绝，且已发布场景/state hash 不动。

此驱动不是全事务故障注入测试；before/after-scene 崩溃恢复仍属既有 Content 测试。

## 固定候选运行命令

先由主代理按项目构建技能生成当前 HAssetImporter 与 HGame；此工具不构建。
以下 PowerShell 从仓库根目录运行，`--output` 必填且必须是白名单下新的目录。

```powershell
$samplingImporter = (Resolve-Path 'out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe').Path
$samplingImporterHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $samplingImporter).Hash.ToLowerInvariant()
$samplingGameHash = (Get-FileHash -Algorithm SHA256 -LiteralPath 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe').Hash.ToLowerInvariant()
py -3.11 Tools/Content/sampling_acceptance.py --importer $samplingImporter --importer-sha256 $samplingImporterHash --existing-build-sha256 $samplingGameHash --backend DX12 --output out/agent/rendering-evidence/vfx-authoring/sampling-dx12-01
py -3.11 Tools/Content/sampling_acceptance.py --importer $samplingImporter --importer-sha256 $samplingImporterHash --existing-build-sha256 $samplingGameHash --backend Vulkan --output out/agent/rendering-evidence/vfx-authoring/sampling-vulkan-01
```

精确底层导入形态由 `content_sync.py` 负责：

```text
HAssetImporter.exe <absolute Project> <output/source/sampling.gltf> <Project/.he-content/staging/unique> --scene
HAssetImporter.exe <absolute Project> <staging/candidate.he> <Project/Assets/Imported/documentId> --validate-scene
```

现有纯 CPU/转换测试可分别执行；第二条需要当前 importer，但不启动 GPU：

```powershell
py -3.11 Tools/Content/test_sampling_fixture.py
py -3.11 Tools/Content/test_sampling_reference.py
py -3.11 Tools/Content/test_sampling_import.py --importer $samplingImporter --project Project --report out/agent/rendering-evidence/vfx-authoring/sampling-import-01/report.json
```

## 输入、发布输出与证据

新驱动为每次运行生成独立 UUID namespace，重映射 fixture 的 `extras.he_id`，同一次
运行的所有阶段保持身份。这是测试资产身份隔离，资源转换、命名、merge、journal、
发布和验证均复用已有 Content 流程。源文件放在 `--output/source`；每阶段保存其
源文件快照、conversion.json、convert.log、validate.log、sync.json、state.json、
Imported.he。真实资源位于 `Project/Assets/Imported/<documentId>`，记录位于
`Project/.he-content/<documentId>`。这些新导入资源按已有 Content 不主动删除旧版本。

initial/material/geometry 的 `gpu/sampling.png` 是 1280×720 截图；各目录同时有
`Capture.he`、`capture.json` 与 Session 命令回执。捕获场景保留原生发布的几何/材质，
只补现有 Preview 的相机、白色方向光和曝光实体；相机位置 [2.5,1.9,8]，朝 -Z。
不执行导入场景上的脚本覆盖；捕获副本移除 PythonScriptComponent。捕获结束只移除
本次 `Project/Assets/Effects/Authoring/SamplingAcceptance/<namespace>/Capture.he`，
避免它成为后续源对象删除的外部引用。不要直接启动带测试脚本覆盖的 Imported.he。

每次 Session 必须在 finally stop，主报告写入外层 Storage 完成之前；进程期间固定
Game hash，整个导入序列开始/结束及各阶段固定 importer hash。失败非零退出并保留
不超过 2400 字符的主错误，详细日志留在磁盘。整次运行由
`Storage.workflow_scope(..., 'sampling-acceptance', [output])` 登记。
只有 stop 成功才移除运行场景；stop 失败保留原位置，在 capture.json 记录
`scenePath`、`sceneRetainedForRecovery=true` 和 `cleanupError`，Storage 标记 unknown，
异常立即终止后续导入。导入错误路径不删除发布资源或捕获恢复材料；Content 自身仅回收
该次转换的 staging/candidate，不是 GPU 场景依赖目录。

## 如何判读结果

`report.json.status=PASS` 仅表示真实导入断言和三次捕获命令通过。
`visualStatus=NOT_REVIEWED` 不会被驱动自动提升。需查看三张图确认面板可见、UV0/UV1
不同、顶点色渐变、变换/独立 wrap 边界、材质色变化、geometry 面板宽度增加。
此构图主要放大采样，不能用它宣称 mip/minification 的全部 GPU 模式已验证；六种
minFilter 的导入字段覆盖与独立数学参考必须与 GPU 图像验收分开描述。
Realtime GI 仍为既有常量材质近似，不能声称其支持精确 UV/纹理。独立DepthShadow与Reference专项结果见顶部状态表；
该采样驱动自身不自动建立这些结论，Vulkan Reference仍未通过。

## 历史静态验证与后续运行记录

早期静态验证：`py -3.11 Tools/Content/sampling_acceptance.py --help` PASS；临时目录
自检确认运行身份隔离/跨阶段身份稳定、更新后 manifest hash 一致、失败断言有效，
该段为早期静态验证。现已完成真实运行：Game15/Importer02 的 sampling-vulkan-05、sampling-dx12-01 均七阶段命令PASS，根人工raster overview PASS，三次捕获双流无native错误。准确SHA/命令见实施记录。

清理回归：`py -3.11 Tools/Content/test_sampling_acceptance.py` — unit-test PASS（3 项，
mock Session，无引擎/GPU）：stop 成功仅移除运行副本、stop 失败保留可恢复场景并记录
错误、start 失败仍调用 stop；三项均核对原导入场景和归档副本未被改写/删除。

独立八面板过滤验证已在Game16/Importer02执行：sampling-filter-vulkan-01 与 sampling-filter-dx12-01 均command及manual visual PASS。非mip混叠、mip频带灰度过渡、nearest/linear放大边界区别可见；两API像素MAE 0.007446，>16差异40pixels，未要求逐像素相等。此结果限raster min/mag，仍不代表RT或所有LOD精确一致。
