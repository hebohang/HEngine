---
name: he-vfx-authoring
description: Create or adjust HEngine Effekseer effects with recipes and runtime previews. Use for VFX authoring; external assets use he-effekseer-asset-intake.
---

# HEngine VFX 创作

通常交付可编辑配方、官方工具编译的运行包和预览；用户只要求审查、配方或编译时，完成该阶段即可。普通创作不修改渲染后端。命令从仓库根执行，使用已安装的 Python 3.11+（示例 `py -3`）。

## 先读取当前契约

创建或修改配方时运行 `py -3 Tools/VFX/authoring.py describe`，查阅所选模板的 [配方说明](../../../Docs/VFXAuthoring-Recipes.md) 和同类配方；以当前契约判断支持范围。离线 `runtime_support=unavailable` 是正常结果，无需为消除它启动引擎。只有实际预览时，才在当前 owned session 查询 `vfx.describe`、`vfx.stats`、`vfx.quality`。

火球飞行/碰撞、角色挂点或多轨联动须另接 [表现组合](../../../Docs/VFXAuthoring-Presentation.md)，不能把单个粒子模板宣称为完整技能。

## 需求到独立配方

- 从描述及现有配方确定实际需要改变的用途、颜色、尺度、时序、方向、预算、CPU/GPU、质量档及播放值；只有影响结果的缺失条件才询问。
- 新效果复制最接近的完整 v1 配方到任务独立目录，如 `out/agent/vfx-authoring/<task>/recipes/<unique-id>.recipe.json`，用独立小写 `id` 并检查源/包重名。不要把任务案例写进 `Tools/VFX/recipes/*.recipe.json`，`build-library` 会扫描整个内置库。
- 保留严格 schema：`semantic` 只供检索，`compile` 改编译图像，`playback` 是调用者应用的默认值。标称 `radius_m` 不等于实测粒子外边界；各模板方向、颜色及尺度限制见配方说明。
- 仅在请求多个变体时按请求数量生成，沿用同一模板、固定 seed 及相同镜头/时刻作比较。仅在请求要求或改动影响 Low/GPU 回退时创建或更新 CPU 配方；GPU 配方仍须引用已发布、匹配的 CPU 回退，见 [回退构建与运行](references/runtime.md#cpu-回退)。

## 校验、编译、发布

```powershell
py -3 Tools/VFX/authoring.py validate out/agent/vfx-authoring/<task>/recipes/<unique-id>.recipe.json
py -3 Tools/VFX/authoring.py build out/agent/vfx-authoring/<task>/recipes/<unique-id>.recipe.json
```

替换占位值后按所需阶段执行。`build` 复用 `Tools/VFX/recipes/toolchain.lock.json` 固定的官方工具和真实编译/回读，不手写 BIN_ 或另造编译器。作者源、纹理、来源和 `build.json` 发布到 `Project/SourceAssets/VFXAuthoring/<id>/`；含明确入口与 provenance 的运行包发布到 `Project/Assets/Effects/Authoring/<id>.efkpkg`。

编译成功只是 build-only；检查 build.json/发布结果。工具已有 journal、旧版备份、失败回滚与相同输入 no-op；不要为了修复失败删除 journal、旧资源或改锁定工具。重试前修正明确错误；外部编辑冲突时保留恢复材料并报告，不能覆盖冲突强行发布。`--force` 用于明确需要的重复编译检查。

外部效果、纹理或模型走 [he-effekseer-asset-intake](../he-effekseer-asset-intake/SKILL.md)，遵守既有下载授权和许可/隔离/依赖检查。不能给 `dependencies` 随意添外部路径绕过 v1 拒绝，也不能承诺把任意导入效果转换为配方。保留原件，在独立副本中调整可编辑源；只有编译输出可用时，限制为当前公开播放参数。

## Owned 预览与迭代

需要预览时按 [运行契约](references/runtime.md) 使用当前候选的 owned session，保存真实 exe SHA256 并在 `finally` 中停止；已有任务授权无需重复确认。检查 GPU 会话冲突，只有缺少可用候选或源码改动需要时才用 [he-build-validate](../he-build-validate/SKILL.md) 构建。

默认验证任务选定的后端；仅在请求多后端支持或改动影响其他后端时，分别验证涉及的 DX12/Vulkan。`--gpu-probe` 只用于已授权开发诊断，结果保持 unverified，不能据此启用生产开关。

新效果完整预览覆盖空场、实际起/峰/衰/自然终与显式 stop；单项调整覆盖受影响画面和行为，不顺带跑模板矩阵、无关参数循环或性能验收。按配方时序采集并在同镜头/seed/时刻比较，trail 需要真实移动挂点。逐张检查主体、轮廓、节奏、裁切和残留；大图先生成不超过 1280 px、约 500 KiB 的预览。取景失败调整独立预览场景/镜头，不改用户场景或把出视锥当消散。

## 完成标准与边界

交付本次阶段的配方、源、包和证据路径；编译成功只报告 build-only，运行证据及视觉结论按运行契约记录。环境不允许运行时交付已完成产物并说明预览未执行，不宣称完整创作验收 PASS。文档历史 PASS 只覆盖对应候选和用例。

仅在任务涉及相应范围时查 [质量](../../../Docs/VFXAuthoring-Quality.md)、[模板矩阵](../../../Docs/VFXAuthoring-TemplateMatrix.md)、[生命周期](../../../Docs/VFXLifecycleAcceptance.md)。严格内存、Release 性能、打包及 GPU 生产开关是独立验收。
