# Blender 场景工作流：实际验收记录

日期：2026-09-09。环境：Windows x64，VS2026 v145 / MSVC 14.51，Blender 5.2.1 LTS，Python 3.7.3。实际游戏后端为 DX12/NVRHI，RTX 3060 Laptop GPU，驱动 32.0.15.9174；开启后端验证，1280×720。本文记录实测范围，使用和制作规范见 [README](README.md)，架构核查见 [WORKLOG](WORKLOG.md)。

## 一次命令与当前产物

```powershell
.\hectl.bat content sync -Source Project/SourceAssets/BlenderAcceptance/Playable.blend -Verify runtime -TimeoutSeconds 240
```

当前源：`Project/SourceAssets/BlenderAcceptance/Playable.blend`；配方：同目录 `Playable.verify.json`。生成器为 `Tools/Content/make_acceptance_scene.py`。首次生成/新增对象后用 `-InitializeIds`，明确授权安全保存身份。重新执行生成器会创建新文档，不用于测试已有文档的身份保持。

发布场景：`Project/Assets/Imported/c788e82f-9870-466f-b0bf-4f4d876c7644/Scene.he`。
持久记录：`Project/.he-content/c788e82f-9870-466f-b0bf-4f4d876c7644/state.json`；与源和原生资产一起保存到版本控制。

最终 runtime 报告：`out/agent/content/20260909-111209-72437b87/report.json`，截图为同目录 `content.png`，完整结构化断言为 `verification.json`。最终 author preview：`out/agent/content/20260909-111701-ff5d832a/report.json` 和 `content.png`。

两者绑定相同场景 SHA-256 `9719559cf49fbda2aba254ea014e279199e37533fcdfb82611133598a168eff6` 与导入版本 `f15139c045757bbcc867c398319c96337b32c271e9dccfe219f680caab9bc076`。
运行会话 `18f55dc8ef4bab35b7ba17d5c9c52a0b`，scene generation=2，捕获 render frame=3826；完成 320 个模拟步进后停止。最终 active session 为 stopped，未遗留本任务 Blender/Game/Editor 进程。

预期警告：`world_lighting_not_exported`。Blender World 不转换，示例显式使用点光、方向光及 EV100=10。错误为空。首轮测试发现固定宽高比相机反序列化后投影未重建，导致黑图；已修复 Level 相机生命周期、正交 near/far 顺序，并增加矩阵回归与空白截图检查。失败的早期截图不作为通过证据。

## 命令与结果

以下从仓库根运行，日志保存在 `out/agent/logs/`。44 项工具/工作流自动测试及 4 项现有引擎测试通过；代码生成与构建分别列出。

| 证据类型 | 实际命令 | 结果 |
|---|---|---|
| build-only | `Tools/Agent/Build.ps1 -Target HAssetImporter -Configuration RelWithDebInfo -Project C:/WorkSpace/HEngineDev/Project`（统一入口内部调用） | PASS；独立 editor-relwithdebinfo |
| runtime-manual | `hectl.bat content sync -Source Project/SourceAssets/BlenderAcceptance/Playable.blend -InitializeIds -Verify runtime -TimeoutSeconds 240` | PASS；首次原生发布、真实加载、截图与行为断言 |
| runtime-manual | 同上，去掉 `-InitializeIds` | PASS；最终完整报告见上，约 15.64 s，不含入口前的导入器构建检查 |
| runtime-manual | `hectl.bat content sync -Source Project/SourceAssets/BlenderAcceptance/Playable.blend -Verify visual -TimeoutSeconds 240` | PASS；作者预览，约 15.27 s；不算物理证据 |
| runtime-manual | `hectl.bat game check -StartupScene Assets/Imported/c788e82f-9870-466f-b0bf-4f4d876c7644/Scene.he -Frames 30 -RenderValidation` | PASS；正常退出，content-game-final.log |
| runtime-manual | `hectl.bat editor check -Frames 30 -RenderValidation` | PASS；正常退出，content-editor-final.log |
| unit-test | `python Tools/Content/test_content_sync.py` | PASS，16 项；content-unit-final.log |
| unit-test | `python Tools/Content/test_native_import.py` | PASS，16 项；实际 HAssetImporter，content-native-final.log |
| unit-test | `python Tools/Content/test_workflow.py` | PASS，10 项；实际转换/原生预检/发布，content-workflow-final.log |
| unit-test | `python Tools/Content/test_blender_workflow.py` | PASS，1 项复合场景；真实 Blender 修改、导出、重导入，content-blender-final.log |
| unit-test | `python Tools/Content/test_legacy_import.py` | PASS，1 项含 OBJ/glTF 两个子例；Assimp 烘焙模式与原生网格预检 |
| unit-test | `hectl.bat test CornellSceneSerialization` | PASS；含实际场景相机投影、正交矩阵和材质覆盖加载 |
| unit-test | `hectl.bat test ReflectedResourceSchemaCompatibility` | PASS；旧资源字段兼容 |
| unit-test | `hectl.bat test MeshCpuDecode` | PASS；现有网格解码契约 |
| unit-test | `hectl.bat test SceneDocumentIncremental` | PASS；场景编辑契约 |
| unit-test | `out/agent/editor-relwithdebinfo/bin/Programs/HCooker/HCooker.exe --self-test` | PASS；新增 CollisionAssetPath 闭包断言，content-cooker-self-test.log |
| build-only | `hectl.bat codegen run -CodeGenHost Editor -Configuration RelWithDebInfo --json` | PASS；changed=0，deleted=0，237 个生成输出 hash/mtime 零扰动 |
| build-only | `hectl.bat codegen run -CodeGenHost DevGame -Configuration RelWithDebInfo --json` | PASS；changed=0，deleted=0，237 个生成输出 hash/mtime 零扰动 |
| build-only | `hectl.bat codegen run -CodeGenHost Tests -Configuration Debug --json` | PASS；changed=0，deleted=0，237 个生成输出 hash/mtime 零扰动 |

反射初次更新生成了 Rigidbody3DComponent 对应 3 个输出；上表是最终 no-op。基于每个 host 的 manifest 枚举输出做前后哈希/mtime 比较，结果为 `out/agent/logs/content-codegen-noop.json`，没有以旧 `_generated` 目录作证据。相机头、碰撞字段及受影响目标均经过生成和编译。

另外实际执行 `hectl.bat content plan -Source Project/SourceAssets/BlenderAcceptance/Playable.blend` 返回 planned、changed=false；`content diagnose -Report .../report.json -ErrorCode world_lighting_not_exported` 返回定位后的单条诊断。缺 Source 的 `hectl.bat content sync` 返回 parameter_error JSON、native exit=2。超时退出码 5 与子进程树回收通过真实 Windows 进程测试。

## 验收覆盖

- 首次 .blend→GLB→cgltf→原生场景→游戏截图；内嵌 PNG 进入原生纹理。独立 glTF/base64 缓冲、外部缓冲与 GLB 用实际导入器测试。
- 核对全部 27 个实体的局部 TRS、父 UUID、世界矩阵、共享网格引用和实例材质槽；三份几何分别覆盖普通共享立方体、镜像绕序版本与凹形门框。
- 最终无变化同步检查场景、state.json、12 个原生文件：14 个文件 hash/mtime 变化数=0，记录在 `content-noop-final.json`。相同配置下无新资源或场景改写。
- 实际 Blender 批次改名、移动、换父级、复制对象；原对象 UUID 保持，复制对象获新 ID，共享 mesh ID/path，newResources=0。
- 修改几何只生成新的 mesh/bin；已有材料不改写。修改材料只生成对应 hmat，不改 mesh/bin。删除后同名重建取得新实体身份。
- 引擎侧玩法组件保持；材质覆盖按稳定槽位身份重排；删除被玩法配置/引用占用的实体、丢失覆盖槽以及源拥有字段冲突均拒绝。
- 缺失依赖、路径越界、重复/缺少身份、剪切、不支持扩展、错误索引产生诊断；没有 Assimp 静默回退。覆盖步长、归一化、sparse 属性及 sparse 索引。
- 发布提交前、提交后记录更新前注入故障并恢复；外部编辑与恢复重叠时拒绝覆盖。旧场景引用不可变资源，保留旧版本，不运行危险 GC。测试的是故障注入，未做断电试验。
- Assimp 原接口导入现有 Box.obj 与 CornellBox.gltf 均成功；CornellSceneSerialization 继续验证既有资源加载与实例材料。

真实物理状态（引擎 Y-up，米）：

| 断言 | 实测位置 (x,y,z) | 说明 |
|---|---|---|
| Test Drop | (0.0000074, 0.2799997, 0.0000244) | 240 步后动态盒正常落地 |
| Test Door Walker | (0.210136, 0.900000, 2.914768) | 从门外 z=5.5 出发，经凹形三角门框开口进入房间 |
| Test Stair Walker | (3.220089, 1.650040, -1.916348) | 经台阶行进，胶囊中心高度升高；断言来自实体状态 |

配方额外保持输入 80 步。测试包含现有角色控制器的转向过程，允许约 0.22 m 横向漂移；边界仍限定在门洞和台阶可用范围内，不以截图替代碰撞断言。

## 实测指标与边界

最终场景：27 个实体，12 个原生资源文件，总计 36,432 bytes（不含场景、制作源、状态记录和引擎内置依赖）；3 hmesh、3 bin、4 hmat、1 hcollision、1 png。几何 arena 使用量 7,440 bytes，13 个可见渲染对象，GPU batch 数 4。这些数值来自现有渲染器，资源共享不等于新增了 draw-call 合并功能。

`20260909-111209-72437b87/render-stats.csv` 的 4,035 帧中，CPU wall median=1.191 ms；过滤未完成/预热 GPU sample 后 4,031 个 GPU span median=1.159168 ms。启动加构建检查/就绪合计 8,181 ms，完整验证 13,940 ms。完整统计见同目录 `measured-metrics.json`。这是固定小场景、暂停与受控步进期间的观察，包含轮询期间的呈现帧，不是大型地图吞吐基准。纯加载耗时、进程总内存、其他设备性能没有测量，仍为 null/未验证。

## 尚未验证及首版限制

- 未运行完整 `hectl package` Shipping 构建或从 Game.pak 启动本关卡；HCooker 自测只证明依赖采集/格式契约。一次直接对 loose Project 调用 HCooker 的探测因缺少合成的 Config/DefaultEngine.json 失败（`out/agent/content-cook-acceptance/cook.log`），不能当作包验证。正式打包必须走现有 PrepareCookInput/PackageGame 的 engine/generated/project overlay；无需把 Blender 源或导入工具加入包。
- 未运行 Vulkan 后端、另一台设备或 Blender 4.5。非默认项目的 CLI/Agent 路径已接入，但尚未启动第二个游戏项目作集成验收。
- 基础色内嵌纹理有本次实际游戏证据；normal/AO/MR、emissive、MASK/BLEND 映射复用既有 BasePBR 路径，未逐项制作视觉基准。镜像展示/非均匀缩放有场景与矩阵证据，不宣称完成所有材质/镜像组合的图像覆盖。
- 任意材质图、动画/蒙皮/morph、压缩 mesh、未支持的扩展、非 repeat/linear sampler、UV1、texture transform、顶点色、Geometry Nodes/Collection Instance 未实现通用转换；适配器/导入器要求烘焙、Realize、Make Local 或直接报错，详见 README。
- 默认导入后重启验证会话；没有持续运行中的热更新。没有自动 GC；历史不可变资源保留。Jolt ABI 改变须重新导入 .hcollision。
- 同步锁只协调此导入任务；不支持同时在编辑器保存同一目标 Scene.he。检查持久 UUID 引用，但不能推断脚本在运行时拼接的任意引用。冲突采用拒绝策略，不是双向合并。
- 测试为小场景；未对 65,000 节点或接近 32 MiB 场景限额作性能/中断压力测试。进程超时树回收已测，机器断电/磁盘硬件失效未测。

用户原有 RenderingBenchmarks 等未跟踪资产未改动；保留本任务历史导入版本及原生资源，不以清理为名删除仍可能被引用的内容。未创建提交。
