# 可编辑特效配方工具

入口 `Tools/VFX/authoring.py describe|validate|build|build-library`。使用项目可用的 Python 3.10+，例如：

```powershell
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' Tools/VFX/authoring.py describe
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' Tools/VFX/authoring.py build Tools/VFX/recipes/explosion-ember.recipe.json
```

`Tools/VFX/recipes` 有六模板（hit/explosion/trail/telegraph/aura/environment）各三个 High 变体，颜色、尺度及攻/持/衰时序均不同，随机种子相同。explosion/environment 使用 GPU 作者节点，另有六个明确指定的 Low CPU 回退配方。CPU 回退是独立创作，不能据此假设任意 GPU 效果可自动转换。

v1 严格区分编译参数（RGBA 0..255、标称半径米、时间秒、粒子数、方向、种子、simulation/quality）、播放默认值（scale/speed/intensity）和只检索的 semantic（purpose/style）。未知字段、非有限参数、预算超限、GPU 缺回退、外部依赖均明确拒绝。地面环模板只接受向上法线。方向由模板应用于发射方向/速度；CPU explosion 使用零半径球形出生点、出生旋转和局部 Z 轴速度作径向扩散，只接受默认 direction=[0,1,0]，非默认方向明确拒绝。其 radius_m 表示有限寿命末端的近似粒子中心半径上界，旋转角采样不保证球面面积均匀；精灵尺寸、透视和淡出会影响可见边缘，仍需实际视觉验收。当前模板为有限效果，持续效果通过 sustain_s 控制，不假装无限循环或任意属性动态修改。

播放默认值均显式写入每个配方：scale=1（范围(0,1000]）、speed=1（[0,16]）、intensity=1（[0,1]）。它们保存在PROVENANCE.recipe，改变这些默认值不会改变编译的EFKE粒子源。调用者播放时应用scale/speed，之后通过 `HEngine.VFX.SetParameter(handle,"intensity",value)` 或Agent `vfx.control` 的parameter action应用intensity。intensity对基础RGBA逐分量衰减，保留基础Color，不是HDR放大或光度功率；GPU效果必须全部GPU节点具有编译后的NotBind_Root颜色继承。

运行时命名参数radius_m范围[.01,50]，需要配方v1的compile.radius_m元数据，通过target/authoredRadius映射uniform scale；它不是实测粒子外边界。编译radius_m仍控制模板空间形态，两者不能混淆。目录 `catalog()` 在嵌入式环境直接调用当前 `HEngine.VFX.Describe()`，也允许显式传入实时Describe callable；每次重新获取，不缓存GPU validated状态。外部离线CLI没有运行时provider时返回unavailable，不能从模板名称或编译成功推断GPU可用。

intensity更新后，`py -3.11 Tools/VFX/authoring.py build-library` build-only PASS，24项均实际重新生成，完整日志 `out/agent/vfx-authoring/build-library-intensity.jsonl`。`py -3.11 Tools/VFX/test_authoring.py` unit-test PASS，6项，新增intensity范围/类型/缺字段拒绝、播放值不写入粒子源、实时目录provider更新回归。随后24项no-op检查changed=0/deleted=0，所有作者源和运行输出hash/mtime零扰动，证据和逐包hash见 `out/agent/vfx-authoring/noop-library-intensity.json`。运行时intensity视觉/命名参数测试由主代理执行，此处不宣称通过。

输出在 `Project/SourceAssets/VFXAuthoring/<id>`：原配方、可编辑 `.efkproj`、编译后的 `.efkefc`、程序纹理、来源/许可、build.json。运行包位于 `Project/Assets/Effects/Authoring/<id>.efkpkg`，与现有 VFX loader 使用同一格式，包有显式 main_effect。

工具复用现有 toon.make_sprite/build helpers、Effekseer CLI 与确定性 ZIP；固定 1.80.6 提交 `93dd29c7b4e353fcef6186fe4ac74c9757243550` 的 exe/core SHA256，版本文件在 `recipes/toolchain.lock.json`。GPU XML 来源为官方 mass 样例的 EDIT，用同版本 EffekseerCore 的 `Effekseer.IO.EfkEfcXml.Decompress` 解码，并保留既有 CC0 许可证据。编译使用真实 `Effekseer.exe -cui -in <xml> -o <efkefc>`，没有手写 BIN_ 或不可重保存的 GPU 二进制补丁。

整个候选在临时目录编译及校验 EFKE chunk 后建立持久发布 journal 与旧版备份，再逐文件原子替换，运行包最后发布，build.json 最后记录完成。编译失败不更改旧源或旧运行包；发布中途 I/O 失败恢复整个旧版及时间戳，进程中断则下次构建先恢复，发现外部编辑冲突保留材料并拒绝覆盖。来源/生成器/工具及输出 SHA256 全部相同时直接返回 changed=0；不会无意义改写时间戳。build --force 用于独立重复编译检查。

`validate-runtime <recipe> --capabilities <当前VFX.GetStatsSnapshot的JSON>` 单独检查当前粒子容量与 GPU 支持/启用状态。100000 是编译器输入限制，绝不是生产运行容量承诺。即使容量通过也仍需同时播放与视觉/性能验收。

## 本次证据

- unit-test：`& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' Tools/VFX/test_authoring.py`，PASS（5项，含真正两次 GPU 编译的输出确定性、无变化 hash/mtime 保持、注入编译失败及第三次文件发布 I/O 失败后的完整回滚、当前运行容量约束）。
- build-only：同解释器 `Tools/VFX/authoring.py build-library`，24配方实际编译通过。记录 `out/agent/vfx-authoring/build-library.jsonl`，每个源目录有独立输入/输出哈希。
- build-only：再次 `build-library`，24项 unchanged、changed=0，记录 `out/agent/vfx-authoring/noop-library.jsonl`。

GPU/CPU 双后端视觉、生命周期与性能仍由主代理使用当前引擎单独验收。所有来源与 build.json 的 `runtime_validation` 均明确 `not_verified`，不能把编译成功当作运行可用。发布格式使用既有 .efkpkg，没有新增平行 Registry；导入外部资源仍由 intake 工作流完成。

初始运行发现并修正两个作者缺陷：1.80.6 必须使用共享 `DrawingValues/ColorAll/Fixed`；旧 Sprite/ColorAll_Fixed 在新版本 XML 中被丢弃。GPU `IsRendered=False` 会阻止 Effekseer 创建发射器，现改为 `IsRendered=True` 且 CPU Renderer Type=None。进一步消除 GPU owner 从 CPU 模板继承的缩放和速度，GPU模拟以单位 owner 变换运行。每次编译会调用官方 EDIT 解码器回读，核对颜色、GPU开关、owner变换、速度、散布、发射量/率、寿命、淡入淡出、尺度，并检查颜色确实进入 BIN_。旧 FAIL 截图与初始报告仍保留；修正版全库记录分别为 `build-library-corrected.jsonl` 和 `build-library-identity-owner.jsonl`。
# GPU 速度单位核查（2026-09-13）

后续颜色API审计确认GPU节点应使用 `RenderColor/ColorInheritType=3`（NotBind_Root），才能让根颜色作用于已发射粒子。作者源现显式设置3，编译后的官方EDIT回读必须等于3。6个GPU配方已重新使用固定1.80.6工具编译，日志 `out/agent/vfx-authoring/build-library-root-color.jsonl`，逐包hash与回读值见 `gpu-root-color-hashes.json`。`py -3.11 Tools/VFX/test_authoring.py` unit-test PASS，5项，包含真实编译重复/no-op及失败发布回滚；本次没有重跑GPU场景，不宣称新的运行时颜色验证通过。

当前SDK `Parameter/Effekseer.GpuParticlesParameter.cpp` 原样读取 LifeTime 和 InitialSpeed，速度只乘 magnification；HEngine `EffekseerRuntime.cpp` 将秒转换为 `DeltaFrame = deltaSeconds * 60.0f`，Manager再把 `GetNextDeltaFrame()` 传入GPU emitter，DX12更新shader执行 `position += velocity * deltaTime`。因此作者源的GPU InitialSpeed采用每60Hz帧的距离，`radius_m / life_frames` 没有缺失60倍换算。`radius_m` 对爆炸表示寿命末端轨迹半径，半寿命只达到约一半，淡出也会影响可见边缘；不等于每帧都应达到该半径。主代理实际preview-gpu-dx12-03已有绘制和模拟，但视觉体量尚未验收。本次核查保持包冻结，没有以编译/单位核查代替视觉通过。
