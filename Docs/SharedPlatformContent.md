# 共享场景与 UI 发布

## 目标与当前边界

项目内容由 Editor 编辑一次，平台后端负责加载、绘制和输入，导出阶段负责资源转换和能力检查。
这不是“任意原生功能自动变成 Web 功能”的承诺。尚未实现的功能必须报错，或在项目配置中明确声明降级。

设计参考：

- [Unreal Engine cooking](https://dev.epicgames.com/documentation/en-us/unreal-engine/cooking-content-in-unreal-engine)：按目标平台转换资源。
- [Godot 导出](https://docs.godotengine.org/en/stable/tutorials/export/exporting_projects.html)：项目内容、导出配置与平台运行模板分离。
- [Unity 平台条件](https://docs.unity.cn/ScriptReference/RuntimePlatform.html)：区分运行时平台能力判断和编译期差异。

本次落地的共享范围是启动场景来源、场景组件能力检查、Editor `.hui` 基础控件，以及便携端的暂停界面。
PC 保留 `SurvivorHUD.hui` 原有暂停和设置面板；共享基础控件尚不能替代完整桌面界面。界面差异由原生表现适配器处理，玩法继续调用共享 C# 会话。
现有主菜单、升级、结算、其他 HUD 仍使用旧的表现适配器，未宣称整个游戏 UI 已迁移完成。
材质、光照、物理、后处理也没有因此获得完整的跨平台实现。
当前仍保留 Survivor 专用内容导出器和表现适配器；这些改动不能使任意新项目自动获得四端运行能力。

## 单一场景来源

`Project.heproject` 的 `GameConfig` 现在指向 `Config/Game.CSharp.json`。
其 `Game.DefaultScene` 为 `Assets/Scenes/MainGameCSharp.he`，Editor 默认入口、Windows、Android、Web、微信使用同一份场景源。
Web 的 `catalog.json.scenePath` 由配置导出，不再硬编码另一个 `MainGame.he`。
Editor 已保存的工作区可能仍打开旧场景；应打开上述共享场景编辑。

`Tools/Build/SharedContent.py` 检查所有发布配置的启动场景是否一致、父节点是否有效，以及便携端能否处理组件。
Web/微信实际实现的静态 Mesh、Transform 等组件由工具中的能力表定义。
当前游戏特有的脚本接入和原生组件降级在 `Config/SharedContent.json` 明确列出；新的未知组件或脚本默认失败。
该配置是对已有适配代码的声明，不会为配置中写入的脚本自动生成实现。
便携渲染仍使用固定光照，场景刚体、点光源与后处理等不会与原生效果相同。降级原因进入构建日志和内容清单。

## 编辑共享 UI

在 Editor 的 UI 面板打开 `Assets/UI/SharedPause.hui`。可以修改标题、按钮文字、颜色、大小、父子层级和锚点，保存后从构建与发布面板重新构建目标平台。
保持 `shared_resume`、`shared_abandon`、`shared_restart` 的 ElementId，事件由同一份 C# `WebGameSession` 处理。
资源中修改显示文字不需要修改平台代码。改变按钮行为需要修改共享 C# 逻辑。

`Config/SharedUI.json` 声明参加共享导出的文档。Android 原生游戏通过 ScriptDependency 声明共享暂停文档的打包依赖；PC 使用原有桌面暂停面板。新增/替换文档也要修改游戏的加载路径和依赖。

后端关系：

| 目标 | 同一份 `.hui` 的使用方式 |
| --- | --- |
| Android | 构建时生成 RML/RCSS，原生 RmlUi 加载 |
| Windows | 支持同一生成后端，但当前游戏保留原有桌面暂停及设置 UI |
| Web | `Platform/Web/HuiDom.js` 创建浏览器控件 |
| 微信 | `Platform/Web/HuiDocument.js` 解析布局、在 Canvas 绘制并返回点击区域 |

通用 UI 后端没有 Survivor 的按钮行为或模式判断。
原生 `SceneServices.SetUiVisible` 管理整份文档的显示和输入，避免隐藏面板挡住主菜单。
显示时遵循现有 RmlUi 的独占活动文档语义，会隐藏其他文档；它不是多文档叠加显示接口。

支持 UIRes v1 的 Canvas、Panel、单行 Text、Image、Button；每个轴独立处理固定锚点和拉伸锚点，支持可见性、父级裁剪、背景/文字/边框颜色、圆角、图片和按钮事件。
支持的颜色为 RGB/RGBA 十六进制、rgb/rgba 和 transparent。字体使用平台默认字体，字形和精确文字度量不保证逐像素一致。
当前共享子集要求 `FontPath`、`ClassName`、`ExtraStyle` 为空；自定义字体、外部样式、多行排版尚未实现，会明确拒绝导出，不会静默忽略。
原生现有 UI 不受这个子集限制；只有登记在 SharedUI 中的文档受检查。

`CompileSharedUI.cjs` 先检查全部共享文档，再生成原生伴随文件。不要手工修改生成的 RML/RCSS。
`--check` 只读检查。Editor 的发布环境检查和实际构建都会执行能力检查。
原生 C# 构建也生成共享 UI，避免只从命令行启动游戏时使用旧的伴随文件。

## 验证命令

```powershell
node Tools/Build/TestSharedUI.cjs
py -3.11 Tools/Build/TestSharedContent.py
node Tools/Build/CompileSharedUI.cjs Project --check
py -3.11 Tools/Build/SharedContent.py Project
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile web
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile windows
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile android
```

Windows 与 Android 发布应串行运行：Android 当前复用原生资源打包步骤，使用同一份受锁保护的 host package 缓存。

`TestSharedUIBrowser.cjs` 在实际导出的 C# Web 游戏中检查资源文字/颜色修改、横竖屏、暂停/继续/结算。
`TestWeChatLandscape.cjs` 在模拟 WX API 的浏览器中运行实际导出小游戏；这不是微信真机证据。
`TestSharedUINative.ps1` 使用自有有界 Game 会话检查桌面菜单/暂停光标、无按键镜头旋转、设置、暂停/继续/结算；在 finally 中停止会话。
构建回执不能代替 Android 真机和微信开发者工具/真机验证。

## 后续迁移顺序

保持当前主菜单和 HUD 外观，逐块提取共享资源与共享绑定逻辑；补齐字体、文本布局、滚动列表、重复模板、动态绑定和平台能力条件后再替换对应旧实现。
最后移除项目内重复的界面构造代码。渲染、物理和脚本宿主的能力统一独立推进，不能用 UI 迁移完成来代替这些验证。
