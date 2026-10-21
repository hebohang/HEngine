# Runtime UI motion

HEngine 的运行时 UI 仍由 RmlUi 6.2 和 DX12/NVRHI 后端负责。本层只编排页面状态、离散 DOM 变更与表现时间，不引入第二套 UI Runtime，也不向脚本或 RenderGraph 暴露 D3D12 类型。

## 职责边界

- `PresentationClock`：应用级、无缩放的表现时间。每个可见帧调用 `BeginFrame`/`CompleteFrame`；暂停不影响它，最小化时以零增量重置实时时间基准，超长帧限制为 100 ms。固定步长 capture 直接传入固定 delta。
- `RmlUiManager`：文档所有权、事件/GIL、延迟 Reload/Unload、路径隔离和 GPU frame 边界。
- `UIAnimationDirector`：验证并执行一个 `UiMotion v1` 时间线，持有唯一 ticket 和输入锁；异常、取消、Reload 或销毁时回滚可恢复的 DOM 状态并无条件释放 ticket。
- `UIScreenStack`：维护 `Hidden`、`PreEntering`、`Entering`、`Active`、`Exiting`、`Suspended` 状态以及 Push/Replace/Pop/Modal 操作。
- RCSS：负责 transform、opacity、颜色等连续插值。Python 只在 track marker 上增删 class、切换 display 或发送事件，不逐帧写 style。

RmlUi 的 `GetElapsedTime()` 使用 `PresentationClock`。脚本可读取 `HEngine.RmlUI.GetPresentationTime()` 和 `GetPresentationDelta()`；游戏暂停或 time scale 不会改变返回值。

## UiMotion v1

资源扩展名使用 `.uimotion.json`，入口由 `RmlUI.LoadMotionAsset(projectRelativePath)` 加载。路径必须是项目内规范化相对路径；绝对路径、UNC、`..`、保留 Windows 名称和符号链接逃逸均被拒绝。文件上限 1 MiB，JSON 深度上限 64、节点上限 20,000。

```json
{
  "format": "UiMotion",
  "version": 1,
  "budgets": {
    "maxActiveTimelines": 1,
    "maxTracksPerTimeline": 256
  },
  "timelines": {
    "settings_enter": {
      "duration": 0.36,
      "interrupt": "replace",
      "unlockMarker": "ready",
      "tracks": [
        { "at": 0.0, "action": "display", "target": "$target", "value": "flex" },
        { "at": 0.0, "action": "class", "target": "$target", "name": "screen_entering", "enabled": true },
        { "at": 0.30, "action": "marker", "name": "ready" }
      ]
    }
  }
}
```

根、timeline 和 track 的未知字段会报错。duration 必须位于 0–10 秒；每个 timeline 最多 256 tracks。action 为 `class`、`display`、`marker`、`sound`、`event`。`$source`/`$target` 在执行时解析。interrupt 为 `cancel`、`replace`、`reverse` 或 `safe_complete`；v1 的 reverse 会保留已提交的页面栈、完成旧页面的 class/display 清理后启动相反时间线，不反向采样正在运行的 RCSS 曲线。Marker 回调中请求的新时间线会延迟到当前 dispatch 退出后开始，旧 track 始终绑定旧 ticket，不会串入新页面。

可选 `renderTransitions` 是非重叠的连续区段，每段包含 `start`、`end`、`from`、`to` 和白名单 `effect`。当前 effect 为 `diagonal_sticker_wipe`、`star_petal_reveal`、`halftone_dot_reveal`；进度由表现时钟逐帧插值，单帧只产生一次 C++ 参数更新。取消、异常、脚本停止和正常结束都会调用 `ClearRenderTransition()`。

标准按钮流程为：Click → ticket/input lock → confirm class → exit class → `switch` marker → stack/DOM 切换 → enter class → `ready` marker 解锁。`reduceMotion: "skip"` 和 `reduceFlash: "skip"` 可跳过单个 track。

## Python API

```python
motion = HEngine.RmlUI.LoadMotionAsset("Assets/UI/MyScreen.uimotion.json")
director = UIAnimationDirector(document_id, motion, HEngine.RmlUI)
director.register_screen("home", "home_panel", visible=True)
director.register_screen("settings", "settings_panel", modal=True)
screens = UIScreenStack(director)

screens.ShowModal("settings", "modal_enter")
# Update / UpdatePaused 均调用：
director.Update()
```

主要 API：`PlayTransition`、`CancelTransition`、`SkipTransition`、`IsTransitioning`、`GetScreenState`、`ResetToScreen`、`Shutdown`，以及 `PushScreen`、`ReplaceScreen`、`PopScreen`、`ShowModal`、`CloseModal`。当 `is_input_locked()` 为真时，应忽略 click、mousedown 和 submit。`Shutdown` 必须在 `RmlUI.Unload` 前调用，它会清理 active/pending ticket 和全屏 shader transition。Director 在执行 track 前通过 `GetClass`/`GetStyle` 保存真实状态，取消时恢复原 class/display。

`OnUIEvent(documentId, elementId, eventType)` 保持兼容。可选的 `OnUIEventEx(documentId, elementId, eventType, payload)` 接收精确 target/current ID、documentId、eventType、property，以及 RmlUi 提供时的 animationName/elapsedTime。animation/transition 事件不提升到最近 button。回调内 Reload/Unload 继续使用管理器的延迟 DOM mutation 契约。

## SVG 与 Lottie

RmlUi SVG 与 Lottie 插件在构建中启用。SVG 使用 LunaSVG v3.0.1 + PlutoVG v0.0.4；Lottie 使用 rlottie v0.2 的 CPU BGRA 光栅化，经 RmlUi 转为预乘 RGBA 后上传 NVRHI 纹理。版本和提交见 `Engine/Source/ThirdParty/RmlUiPluginDependencies.json`。

制作预算：SVG/Lottie JSON 单文件不超过 1 MiB；推荐 Lottie 画布不超过 512×512、30 fps、单片段不超过 3 秒、同屏活动实例不超过 4。当前项目素材是原创 `starbloom-starburst.svg` 与 `starbloom-burst.lottie.json`。Cooker 会从 `.hui` 的 RML/RCSS 结构依赖以及 markup 引用闭包发现资源，包内必须同时存在许可证。

rlottie v0.2 不支持的 After Effects 表达式、脚本、部分 3D/插件效果应在导出前烘焙；不允许依赖网络图片或绝对路径。加载失败会由 RmlUi 明确记录，页面应提供同尺寸 SVG/PNG 或 sprite sheet fallback。

## 渲染能力与限制

`HEngine.RmlUI.GetRenderCapabilities()` 返回显式能力字典。当前 NVRHI 后端支持：

- 基础 geometry、预乘 RGBA texture、layout/render 分辨率映射与 transform；
- scissor 与 D24S8 Clip Mask（Set、SetInverse、Intersect）；
- 最多 8 个逻辑 Layer；物理 `RGBA8_UNORM + D24S8` 资源按 frame slot 隔离并按实际深度懒分配；
- opacity、固定 9-tap blur/drop-shadow，以及 brightness、contrast、invert、grayscale、sepia、hue-rotate、saturate；
- `SaveLayerAsTexture`、`SaveLayerAsMaskImage` 和全视口 MaskImage alpha 合成；
- 预编译 Custom Shader 白名单 `starbloom-sheen`、`starbloom-halftone`；RML 不能提供路径、源码或任意 HLSL；
- geometry/texture 首次上传采用 record/submit/abort 两阶段确认，资源、handle 与辅助 target 遵守 frames-in-flight 延迟释放。

高级路径的语义边界：blur/drop-shadow 是 sigma 上限 32 的固定成本近似，不是多级精确高斯；标准 linear/radial/conic gradient shader 暂未实现；backdrop 优先采样 RenderGraph 明确声明的 LDR scene snapshot，不包含同一 UI pass 中更低 z-order 已绘制 UI；`SaveLayerAsTexture` 只保存正常 RmlUi 调用路径中的 RGBA8 active layer；MaskImage 为全视口 alpha，Layer 暂无 MSAA resolve。未知 filter/shader 会明确拒绝并计入 `UnsupportedFeatureRequests`，不会静默显示错误内容。

高质量运行时链位于 tone mapping 之后：

`RuntimeUiBackdropCopy → RuntimeUiRender → UiTransition → UiComposite → OffscreenOutputCopy → UI(ImGui) → Capture → Present`

`RuntimeUiRender` 清透明黑并写显示编码的预乘 `RGBA8_UNORM`，同时提供 D24S8；`UiTransition` 将 coverage 同时乘到 RGB/A；`UiComposite` 使用 `One / InvSrcAlpha`，不再做第二次 sRGB 编码。主 offscreen 包含 RmlUi、不包含窗口 ImGui；capture 包含两者。Direct 路径保留为无特效/低质量 fallback，OffscreenOnly 继续不渲染运行时 UI。三个原创全屏效果都在 `reduce_motion` 或 `reduce_flash` 下退化为单调低频 opacity fade。

UI color、stencil、transition target 是 RenderGraph Persistent 资源。framebuffer/binding cache 为每次 graph build 记录 generation 与使用过的 frame slots；旧代次只在对应 slot 的上一提交确认完成后释放，因此连续 graph rebuild 不会永久钉住 4K 纹理。Resize/teardown 在 queue idle 后清缓存并递增诊断 generation。

颜色约定：RmlUi 生成和加载纹理为预乘 alpha `RGBA8_UNORM`；混合为 `One / InvSrcAlpha`。最终 LDR sRGB 转换只发生一次。

## 无障碍与性能

- `reduce_motion` 禁止大幅位移、进出循环动画和 HUD fill transition，但保留静态 focus/selected 边框。
- `reduce_flash` 禁止 confirm flash、damage/xp flash、loading glow 和快速 sparkle；颜色/轮廓仍表示状态。
- Hover、`:focus` 和脚本 `selected` 使用同等级轮廓。当前引擎尚无原生 gamepad 输入层；键盘焦点可用，手柄导航不能声明已完成。
- 稳态避免 Python 每帧遍历整棵 DOM。HUD 值已有 cache；时间线仅在 marker 到达时产生调用。
- 推荐同时活动 timeline 1、tracks 256、Lottie 4、SVG 1 MiB、Layer depth 8。性能验证使用 `-RenderStats` 分别比较 `RuntimeUiBackdropCopy`、`RuntimeUiRender`、`UiTransition`、`UiComposite`、draw calls、上传数量与 graph cache generation。

## 新建页面

1. 在 RML 中给 screen 根节点稳定 ID，初始隐藏；所有交互元素提供 ID 和可聚焦 button 语义。
2. 在 RCSS 中定义 `screen_entering`/`screen_exiting` 或页面专属 class，只动画 transform、opacity 和颜色。
3. 在 UiMotion 增加 enter/exit timeline，`switch` marker 负责 display/stack 切换，`ready` marker 解锁。
4. `register_screen` 后通过 `UIScreenStack` 操作，不在 click 回调立即隐藏旧页面。
5. 为 reduce motion/flash 添加 track 或 RCSS 替代；中英文长文本和 1280×720、1920×1080、超宽至少各检查一次。
6. 运行逻辑测试、Game/Editor check、DX12 smoke、capture 和 package，并检查日志中没有 RmlUi/NVRHI validation 警告。
