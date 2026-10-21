# 微信小游戏首次贯通记录（2026-09-09）

更新于 2026-09-10：已配置用户提供的 AppID，并在微信开发者工具 2.02.2608070 / 基础库 3.17.2 中运行真实 HE 场景、Wasm 与 Sokol/WebGL 2。截图和实际触摸验证了移动、变色；重新编译后的存档与方块显示位置保持一致。**Android/iOS 真机、实际扬声器输出和微信后台恢复仍待验证**。以下 2026-09-09 内容保留为首次实现记录，最新平台证据见末尾。

Android 后续反馈：小米 15 / 微信 8.0.77 一直停留在微信启动页，没有进入游戏画面，首次真机试运行未通过。已推送下述诊断版，不能将模拟器 PASS 或推送成功视为该问题已修复。

## 修改前审计，2026-09-09

- 已跟踪文件干净；用户未跟踪的 RenderingBenchmarks、Imported、SourceAssets 等资源保留。初始清单：`out/agent/wechat/evidence/initial-status.txt`。
- Application.cpp 同时管理窗口、SimulationClock、RuntimeRenderer、ImGui/RmlUi、音频。窗口经 Platform/GLFW，桌面主循环不能直接用于微信的帧回调宿主。
- RenderWorld 包含相机、变换、骨骼 CPU 数据，但 RenderMeshSection 持有 Mesh/Material，RenderPath 和 GPU 场景索引仍属桌面约定。Mesh/Material 与 RenderDevice/RenderGraph/NVRHI 资源存在间接耦合，不能直接跨后端共享 GPU 对象。
- NativeScriptComponent 只有 Bind<T> 函数指针。NativeScriptSystem 首次 Update 调 OnCreate，无场景类型工厂、序列化属性和可靠 Stop 回收路径。ScriptableEntity 通过 Entity/Level 访问 ECS。
- ComponentBase.h 的 pybind11/embed.h 泄漏至所有组件；Runtime CMake PUBLIC 链接 pybind11、桌面图形及音频/物理/UI 等依赖。独立目标必须在进入该构建图之前分流。
- SceneDocument 已有受限 JSON/二进制 JSON 解析、UUID/路径校验和取消检查；应复用，不另建场景文件格式。
- RuntimeResourceCatalog 管理路径/版本/依赖，Registry 管理 CPU loader、主线程 finalize、lease/group、失败缓存、代际失效和取消。首版使用 host 异步 IO 后 AcquireBlocking，零线程模式明确拒绝 QueueAsync。
- HShaderCompiler 已用 DXC 产生桌面 DXIL/SPIR-V、验证及缓存；桌面使用 Vulkan 1.3/DX layout 等选项，不能复用为 ES 3.00 缓存。SPIRV-Cross 已在 ThirdParty 中，无需修改其源码。
- RmlUi/ImGui 绘制与 NVRHI 相关；首版状态反馈用场景 sprite，物理/粒子/导航/蒙皮/桌面 UI 都不进入目标。
- 工具：VS2026 v145/MSVC 14.51.36231；PATH CMake 3.23.1（脚本实际定位 4.3.1-msvc1）；DXC 使用仓库 HShaderCompiler 自带版本，SPIRV-Cross 使用 Vulkan SDK 1.3.211.0；Emscripten 开始时未安装；微信开发者工具 1.06.2307260。
- 桌面基线：`hectl game check -Frames 5 -GraphicsApi DX12 -StrictGraphicsApi`，runtime-manual PASS；启动 READY、exit=0。

## 实施顺序

1. 独立工具链、宿主加载与 Sokol GLES3 探针，尽早检查微信环境。
2. HLSL→DXC→SPIRV-Cross 的目标 Shader 与反射/缓存验证。
3. SceneDocument/Registry/ECS/NativeScript 最小场景接入及生命周期。
4. 触摸、声音、存档、前后台与导出，故障测试和 Windows 回归。

## 权威依据与尚待核实

- 腾讯官方 API 类型仓库固定 `4cae82af7f3c4339d1f11aea8e672fb16051d24a`：https://github.com/wechat-miniprogram/minigame-api-typings 。WXWebAssembly.instantiate 接收包内 path，类型定义返回 Promise<Instance>；宿主适配同时识别标准 `{instance,module}` 结果并验证 exports。
- https://developers.weixin.qq.com/minigame/dev/guide/performance/perf-webassembly.html 与 Canvas.getContext/projectconfig 页面当前检索不可访问。没有将旧包体限制或 WebGL2 支持矩阵当成事实；必须运行时查询与实测。
- https://emscripten.org/docs/api_reference/module.html ：使用 instantiateWasm/预创建 context 接入宿主，避免假定微信有 DOM。
- https://github.com/floooh/sokol ：仅 sokol_gfx，不接入 sokol_app；固定 commit `c24221dc065f34b26c6abd7b9194182120dba140`。支持 GLES3 不是微信兼容性证据。

## 实现与边界

- `CMake/WeChat.cmake` 在进入桌面依赖图前选择独立目标。Windows 反射生成器产生带 manifest 的输出，Wasm 只消费其中必要的生成头；不会执行目标 Wasm 作为构建工具。宿主 Python 仅运行工具，目标编译依赖审计覆盖 1,333 个实际源文件/头文件，未发现 CPython、pybind11、Windows SDK、NVRHI、DX12 或 Vulkan。
- `Platform/WeChat/LightLevel.cpp` 是严格能力配置下的 Level 实现，复用 HE Entity、entt registry、Transform/Sprite/NativeScriptComponent、NativeScriptSystem 和 SceneDocument，不启用桌面物理、音频、导航等系统。不是完整桌面 Level 的移植。
- `LightRenderer` 接收现有 CPU `RenderCamera`/`RenderPrimitive2D` 与独立 CPU 像素资源，不接收 Mesh/Material、设备或 GPU 索引。桌面 RenderWorld 的 Mesh/Material 间接耦合仍保留，未宣称全仓渲染输入已解耦。
- 只接入 `sokol_gfx` 的 GLES3 实现；wx 创建 context 后通过 Emscripten GL 注册。没有 sokol_app、共享桌面 GPU 设备或替换 NVRHI/RenderGraph。Sokol GPU image/view/sampler/pipeline 随轻量渲染器回收。其微信兼容性仍需平台实测，没有证据要求改用直接 GLES。
- 原生玩法增加显式名字工厂，启动直接注册 `TouchSample`，避免静态注册被链接裁剪。生成反射负责组件 Type/Properties 与 TouchSampleState 序列化；`NoPython` 元数据只排除 Json 属性的 Python 绑定。复制不共享实例，移动转移实例，Stop/销毁、自删除、ECS 压缩和失败回收有契约检查。
- 同一份 `Project/Source/Game/GameLayer/TouchSample.cpp` 编入 Windows HGame 和 Wasm。触摸目标、移动、计数和颜色状态都由 C++ 更新。触摸开始/结束在两帧之间发生也会保存计数。Windows 已加载同一个 `.he` 样例到 Ready；该验证不意味着已给桌面增加鼠标到本样例的输入桥。
- 资源使用 SceneDocument、Catalog、Registry、lease/group 和版本键；Catalog 显式记录 Scene→Texture 硬依赖。微信 host 异步读取包内资源后放入 MEMFS，Registry 零线程模式同步解码 PNG，QueueAsync 明确拒绝。CPU 资源和 Sokol GPU 实例独立所有权，不绕过 HE 生命周期。
- 包内路径为 `Assets/...`；C++ 根为 `/project`。下载缓存本版不启用，没有远程地址、登录或广告等接口。持久存档使用集中配置的 wx storage key，与包内资源/MEMFS 分离。清单含字节数和 SHA-256，导出验证文件哈希，运行时验证读取类型与长度；**运行时没有重复做密码学哈希校验**。损坏存档/IO 失败显式报告，不静默重置。
- host 处理 WXWebAssembly 两种返回形状、初始化错误、画布/DPR、单触点、尺寸变化、后台暂停/恢复、输入清理、手势音频及中断、串行异步存档和关闭回收。暂停后第一帧 dt=0，C++ 再限制 dt≤50 ms。

首版能力配置 `wechat-webgl2-sprite-v1`：1–8 个实体、恰好一个 TouchSample、单张 PNG（每边最多 1024）、固定正交相机、纹理四边形、深度测试、直 Alpha 混合和颜色状态反馈。样例是一个棋盘 sprite 与程序生成的短 WAV，无外部美术授权依赖。

尚未实现通用 Mesh/PBR、骨骼、阴影、粒子、RmlUi/ImGui 绘制、复杂透明排序、父子变换、CameraComponent 导出和音频资源的 HE Registry 集成。当前状态反馈是 sprite 颜色，尚无独立文字/按钮 UI。导出拒绝 Python、其他原生类型、材质/Shader 配置以及能力配置之外的组件/字段；不能把任意现有 HE 游戏直接导出。编辑器继续使用桌面路径，本轮未做编辑器回归。

## Shader 与 Wasm 的实际决策

HShaderCompiler 下增加 Windows 宿主脚本 `compile_webgl.py`，离线执行 HLSL→DXC SPIR-V→SPIRV-Cross GLSL ES 3.00。只编译样例 VSMain/PSMain；不转换全部桌面 Shader，不使用 Slang。编译日志、SPIR-V、最终 GLSL、反射、描述头和 manifest 位于 `out/agent/wechat/shaders`。

- DXC 使用 `-spirv -fspv-target-env=vulkan1.0 -fvk-use-gl-layout -O3 -Zpc`，Cross 使用 `--es --version 300 --flatten-ubo --no-420pack-extension`。固定配置无宏变体；缓存身份包含源及递归 include 内容、入口/阶段、工具二进制版本哈希、选项和平台能力配置。Wasm 构建回执校验源依赖、二进制及 Shader 身份，阻止陈旧产物混用。
- 检查两个阶段 location/type；Cross 原输出 VS/PS 名字不同，ES 3.00 实际链接失败。现在通过其正式 `--rename-interface-variable` 选项统一名字，没有零散文本替换。
- 检查 SPIR-V 的 Rows[4] 偏移 0、stride 16，Tint 偏移 64，总 80 字节；再检查最终 GLSL 的 vec4[5]、属性名与组合 sampler 名，生成 Sokol 描述。运行时查询/链接由 Sokol 执行，日志保留诊断。GLM 矩阵显式打包为四行，HLSL 点乘；正交投影直接采用 GL [-1,1] 深度，无额外 Y/深度补丁。
- RGBA8 当作线性数值纹理，UV 顶部对应图片首行，直 Alpha 混合；不声明已有 sRGB/PBR 色彩管理。浏览器像素检查覆盖采样、常量 tint、变换位置、UV 与混合，微信驱动编译/绘制仍未验证。
- Emscripten 单线程、静态链接、非共享内存，无 SIMD、atomics、bulk-memory、sign-ext、nontrapping-fptoint。最终模块经 `wasm-opt --mvp-features` 验证。C++ 异常保留 JS 模拟处理，不依赖 Wasm EH；`WASM_BIGINT=0` 避免 JS i64 BigInt ABI 要求。
- `ENVIRONMENT=web,shell` 经无 DOM VM 验证。先前 `web` 单环境会在微信式宿主的初始化阶段报错。不是 Node 目标，也没有给游戏提供 process/require/fs。
- 初始线性内存 32 MiB、增长上限 128 MiB；**栈 1 MiB，STACK_OVERFLOW_CHECK=2**。真实失败重试发现默认 64 KiB 栈无法容纳 HE JSON 读取器的 64 KiB 栈块及调用帧，表现为后续 locale 虚函数损坏。修复栈容量后原哈希实现保持不变。保留函数名与断言帮助首次微信调试，尚未按发布尺寸优化。

## 固定依赖与本机工具

完整下载地址与 SHA-256 在 `Tools/WeChat/dependencies.lock.json`，setup/构建会校验。

| 依赖 | 本次版本/来源 |
| --- | --- |
| Emscripten | 4.0.23-git，commit `7a5d93b50f6a3a35e85a0d2fc9e667b8498e6aed`，固定 Windows 二进制归档 |
| sokol_gfx | commit `c24221dc065f34b26c6abd7b9194182120dba140`，只下载头与许可证到 out/agent |
| DXC | 仓库自带 1.7.2212.12 (`8c9d92be7`)，exe/DLL 均锁定 |
| SPIRV-Cross | Vulkan SDK 1.3.211.0 Windows Bin，exe SHA-256 锁定 |
| 本机 Windows 构建 | VS2026 v145，MSVC 14.51.36231，CMake 4.3.1-msvc1，Ninja 1.13.2 |
| 工具解释器 | Python 3.11.1，Node 24.19.0；目标不链接 Python |
| 微信开发者工具 | 已安装 1.06.2307260；未登录，实际基础库版本未测 |

保留仓库现有 entt/glm/json/spdlog/crossguid/stb_image。实际参与编译的文件哈希位于 Wasm build-receipt.json，导出包含其许可证。没有改动 Engine/Source/ThirdParty。

## 从干净本地环境构建与导出

前提：完整本仓库与已有第三方依赖，VS2026 C++/Windows SDK（无时按项目脚本回退 VS2022）、CMake/Ninja、Python≥3.10、Node≥20、上述固定 SPIRV-Cross。使用本仓库的 `hectl` 工具定位 VS；不用 VS 的 out/build 缓存。首次下载约 642 MB Emscripten 归档，还需要解压和桌面 host 构建空间。

从仓库根 PowerShell 执行（示例路径按实际安装调整）：

```powershell
$env:HE_WECHAT_PYTHON = 'E:/Python/Python311/python.exe'
$env:HE_WECHAT_NODE = 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe'
$env:HE_WECHAT_SPIRV_CROSS = 'E:/VulkanSDK/1.3.211.0/Bin/spirv-cross.exe'
.\hectl.bat wechat doctor
.\hectl.bat wechat setup
.\hectl.bat wechat check
```

setup 只下载固定 Emscripten/Sokol；SPIRV-Cross 需本机安装或配置为锁定二进制。下载中断可重跑 setup 续传，哈希不匹配拒绝继续。已有 Python/Node 符合要求时可自动定位，环境变量优先。

`check` 顺序执行：严格场景校验→Shader→Windows codegen→独立 Wasm build→MVP 验证→无 DOM 真 Wasm 检查→依赖回执→优化的 Windows 契约目标→真实场景解码验证→本地导出→原生/导出/host 故障测试。它不启动或操作微信账号。`build` 止于 Wasm/回执，`validate` 只作静态能力检查，`shaders` 只作离线 Shader，`export` 构建并导出（不运行完整故障测试）。

集中配置为 `Project/Config/WeChat.json`。AppID 目前为空，需填入用户自己的真实小游戏 AppID；不要填 touristappid。改资源/配置后重跑：

```powershell
.\hectl.bat wechat export
# 可选自定义配置和本地输出；输出必须在本仓库 out/agent 子目录
.\hectl.bat wechat export -Source Project/Config/WeChat.json -Report out/agent/wechat-export
```

输出 `out/agent/wechat-export` 包含 game.js、game.json、project.config.json、he-config.js、host.js、engine/hegame.js/.wasm、Shader、真实 .he/PNG/WAV、许可证和资源清单。以前的目录保留为同级 `.previous-*`，不会静默覆盖用户源资源。浏览器诊断入口被 packOptions 排除。

## 本地验证证据

所有以下路径均相对仓库根。主日志在 `out/agent/wechat/evidence`；完整 Windows 构建/进程日志在 `out/agent/logs`，运行命令均回收所启动的 Game。

| 证据类型 | 精确命令 | 结果与日志 |
| --- | --- | --- |
| build-only / codegen | `hectl.bat codegen run -CodeGenHost DevGame -Configuration RelWithDebInfo --json` | PASS；codegen-noop.json：changed=0、deleted=0、249 输出 hash/mtime 零扰动 |
| build-only | `hectl.bat wechat check` | PASS；wasm-build.log、shader-build.log、dependency-audit.log；独立 Wasm 与 native Release 编译 |
| unit-test | `hectl.bat wechat check` 内的 test_wasm.cjs | PASS；wasm-no-dom.log：真实 Wasm、无 DOM/Node API、异常回执、缺文件、真实 HE 场景/反射/工厂至预期的无 GPU 错误、清理 |
| unit-test | `hectl.bat wechat check` 内的 HEngineWeChat.exe / test_export.py / test_host.cjs | PASS；native-contracts.log、export-tests.log（10 项）、host-tests.log；生命周期/复制移动/自删除/链接保留/属性重启、失败缓存与版本、Python/未知能力拒绝、宿主故障回执 |
| runtime-manual（自动驱动的辅助浏览器集成观察） | 下方 browser_check.cjs 命令 | PASS；browser/result.json、console.log、initial.png、moved.png；真 Wasm+WebGL 绘制和交互，不是微信测试 |
| runtime-manual | `hectl.bat game check -Frames 5 -GraphicsApi DX12 -StrictGraphicsApi` | 修改前基线 PASS，Ready/exit=0 |
| runtime-manual | `hectl.bat game check -Frames 30 -GraphicsApi DX12 -StrictGraphicsApi` | PASS；windows-dx12-regression.log，默认项目启动回归 |
| runtime-manual | `hectl.bat game check -Frames 30 -GraphicsApi Vulkan -StrictGraphicsApi` | PASS；windows-vulkan-regression.log，严格 API 无回退 |
| runtime-manual | `hectl.bat game check -Frames 30 -GraphicsApi DX12 -StrictGraphicsApi -StartupScene Assets/WeChatSample/Touch.he` | PASS；windows-sample-dx12.log；实际记录 Ready scene=Assets/WeChatSample/Touch.he，不代表桌面触摸/渲染视觉验收 |
| unit-test | `hectl.bat test RuntimeResourceRegistryLifecycle` | PASS；windows-resource-regression.log |
| unit-test | `hectl.bat test RuntimeResourceCatalog` | PASS；windows-catalog-regression.log |
| unit-test | `hectl.bat test SceneDocumentIncremental` | PASS；windows-scene-regression.log |
| unit-test | `hectl.bat test CornellSceneSerialization` | PASS；windows-serialization-regression.log |

辅助浏览器检查需要本机 Chrome 与 Node 的 playwright/pngjs 模块。当前机器精确命令：

```powershell
$env:NODE_PATH = 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules'
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/WeChat/browser_check.cjs
```

其他干净机器可把 playwright/pngjs 安装到独立工具目录再设置 NODE_PATH，以 `HE_WECHAT_CHROME` 指向 Chrome；这些只是诊断工具，不进小游戏。脚本创建临时 localhost 服务，finally 关闭浏览器和服务，不构建网页发布系统。

本地样本测量：Windows，i7-11800H，Chrome 152.0.7977.83 headless，ANGLE/NVIDIA RTX 3060 Laptop/D3D11，480×720/DPR1。最终导出 23 文件、1,727,538 字节（包括诊断入口、许可证与清单，未压缩；不是微信上传包大小），Wasm 1,529,301 字节，JS 152,851 字节。该次 localhost 首帧可交互 71 ms；前 30 帧后记录的 14 次 CPU 模拟/渲染提交耗时为 0–1 ms，Date.now 毫秒粒度，样本很短，**不是 GPU 耗时或稳定帧率基准**。Wasm 线性内存 33,554,432 字节，不是进程总内存或 GPU 内存。浏览器有一次播放和 ended 事件，不代表手机扬声器已听测。微信首次可操作时间、帧耗时、包体限制和真机内存均未测。

## 2026-09-09 微信导入阻碍与下一步（历史记录）

实际执行了本机 CLI 导入：

```powershell
& 'E:/Program Files (x86)/Tencent/微信web开发者工具/cli.bat' islogin
& 'E:/Program Files (x86)/Tencent/微信web开发者工具/cli.bat' open --project C:/WorkSpace/HEngineDev/out/agent/wechat-export
```

结果 `login:false`，open 返回 `code:10 需要重新登录`，日志为 wechat-islogin-retry.log / wechat-import.log。因此：工程目录已形成，但**没有成功在开发者工具运行，也没有 Android/iOS 真机运行**。本次只开启了本机 CLI 服务端口并启动开发者工具；没有上传、提交审核、改账号后台或读取无关凭据。

后续按顺序操作：

1. 用户在开发者工具扫码登录，并在集中配置填写真实 AppID，重跑 `hectl wechat export`。本机工具版本较旧，记录实际工具/基础库版本，不凭版本号假定支持。
2. 导入 `out/agent/wechat-export` 为小游戏，点击编译。在 Console 检查 `GameGlobal.HEWeChatSession.isReady()` 和 `.getMetrics()`；若 WXWebAssembly/WebGL2/API 缺失，保留完整错误，不能降级后仍标作本配置通过。必要时升级工具后复测，而非绕过错误。
3. 检查棋盘对象、点击变色、拖动移动、手势声音；停止/重新编译确认位置与状态恢复；按住时切后台再回来确认不继续移动、不跳步；旋转/尺寸变化后触摸命中一致。保存 Console/画面、工具版本、基础库与性能口径。
4. 在用户可用的 Android/iOS 微信设备上分别做相同流程，另测音频中断、断开后恢复与冷启动。真机调试/预览可能触发平台上传；本次未授权上传，因此只准备步骤，须由用户明确决定该操作。开发工具模拟成功也不能代替这两项。
5. 微信通过后再决定扩展独立 UI、资源异步解码/缓存、相机与多纹理；通用 Mesh/PBR 和透明排序需要新的能力配置与对应 Shader/资源验证，不应放宽当前导出校验冒充支持。

## 2026-09-10 微信开发者工具验证

用户提供 AppID `wx54887856924392ee`，已写入 `Project/Config/WeChat.json` 并生成到导出工程。用户已开启本机服务端口、允许 Codex 调试访问，并单独授权发送本次调试包生成手机预览；没有授权上传发布版本或提交审核。

第一轮发现：微信读取包内 `Assets/WeChatSample/Touch.he` 返回 `readFile:fail permission denied`。当时将包内场景命名为 `Touch.he.json`，资源清单增加 `packagePath`；host 按包内名字读取后，仍挂载到 `/project/Assets/WeChatSample/Touch.he`。模拟器因此运行到 120 帧，HE 错误列表为空。这只是模拟器证据；后续 Android 暴露该 JSON 载体的字节差异，当前实现已改用下文的 `.he.bin`。

重新导出前必须关闭该工程窗口：IDE 的文件监听会令原子目录发布报 WinError 32。旧 `cli open` 与新 `open_project_window` 可能各自持有窗口，需要分别按项目路径关闭；不要手动覆盖或删除事务恢复材料。导出成功后再打开。

本机入口：`E:/Program Files (x86)/Tencent/微信web开发者工具/wechatide.cmd`。先使用 `-c Codex check_wechatide_status --skill-version 0.3.9` 检查登录和访问授权；开窗采用 `-c Codex open_project_window --project C:/WorkSpace/HEngineDev/out/agent/wechat-export --window-mode fullMode`。若截图/求值等待 automator 超时，执行一次 `cli.bat auto --project C:/WorkSpace/HEngineDev/out/agent/wechat-export` 后本机接口可用。小游戏触摸使用 `automation_game_action`，不能用 WXML 页面选择器。

本轮证据位于 `out/agent/wechat/evidence`：

| 证据类型 | 命令/动作 | 结果 |
| --- | --- | --- |
| build-only + unit-test | `hectl.bat wechat check` | PASS；wechat-check-appid.log，带 AppID 导出、资源映射契约和原有检查 |
| runtime-manual | `node Tools/WeChat/browser_check.cjs`（使用上文 Node/NODE_PATH） | PASS；browser-check-appid.log，修复后的辅助浏览器回归 |
| runtime-manual | `wechatide.cmd -c Codex get_simulator_console --project C:/WorkSpace/HEngineDev/out/agent/wechat-export --command "grep -n HE"` | PASS；wechat-console-current.log，wechat-host / WebGL 2 / frames=120 / errors=[] |
| runtime-manual | `wechatide.cmd -c Codex automation_game_action --project C:/WorkSpace/HEngineDev/out/agent/wechat-export --action swipe --start-x 270 --start-y 580 --end-x 400 --end-y 420 --duration 500 --coordinate-space image --image-width 546 --image-height 1179` | PASS；wechat-touch.log，方块从蓝色变红色且向右上移动 |
| runtime-manual | `wechatide.cmd -c Codex simulator_refresh --project C:/WorkSpace/HEngineDev/out/agent/wechat-export` 后读取 wx storage 并截图比较 | PASS；wechat-save-before-restart.log / wechat-save-after-restart.log，Score=1、Translation=[0.20616114,0.27725118,0]；重启后红色区域边界一致 |

截图为 wechat-simulator.jpg、wechat-moved.jpg、wechat-restored.jpg；结构化结论为 wechat-verification.json。微信模拟器首次可操作时间记录为 557 ms，前 30 帧后 90 次 CPU 提交采样为 0–1 ms（Date.now 毫秒粒度），只代表该次 Windows 模拟器运行，不代表手机性能。平台基础库启动另有 `jsbridge not ready` 日志，但 HE 随后正常启动；未将其隐藏或视为 HE 运行失败。

经用户明确允许后执行 `wechatide.cmd -c Codex auto_preview --project C:/WorkSpace/HEngineDev/out/agent/wechat-export`：PASS，已推送开发者手机预览，平台报告调试包大小 1,892,929 字节；日志 wechat-phone-preview.log。此结果只证明预览包生成/推送成功，手机端运行仍等待用户确认。没有执行 upload 或审核提交。完整本次模拟器证据已按 Storage 工作流登记于 `out/agent/wechat/verification-20260910-015522`。

### Android 启动诊断（2026-09-10）

根据用户上述失败反馈增加 `boot-diagnostics.js`：在加载生成 JS 前启动原生阶段提示与 30 秒 watchdog，首个成功渲染帧后关闭提示；同步入口异常、Wasm 失败和首帧失败显示原生错误弹窗，并仅在本地 `he-wechat-last-boot-error` 存储诊断。不会创建第二个屏幕画布、改玩法存档或发送遥测。

同时修复宿主的超时范围：原来 Wasm factory 完成后即清除计时器，资源/存档 Promise 若不回调则无限等待。现在资源/存档等待同样受启动超时限制。此为代码检查发现的真实缺陷，**尚无证据证明它就是该 Android 启动页卡住的原因**。

- build-only / unit-test：`hectl.bat wechat check` PASS，日志 wechat-check-android-diagnostic.log；新增 test_boot.cjs 验证首帧结束、阶段超时、错误只报告一次与缺少原生 UI API。
- unit-test：`node Tools/WeChat/test_host.cjs` PASS，host-tests-android-diagnostic.log；新增真实悬挂资源 Promise 在启动 deadline 内失败的契约。
- runtime-manual：`node Tools/WeChat/browser_check.cjs` PASS（使用前述 Node/NODE_PATH），browser-check-android-diagnostic.log。
- runtime-manual：诊断版微信模拟器仍运行至 120 帧且 HE errors=[]，wechat-boot-full.log。
- 手机预览：同一 `auto_preview` 命令 PASS，调试包 1,895,841 字节，wechat-android-diagnostic-preview.log。等待用户打开最新预览并提供 HE 阶段/弹窗；若从未出现 HE 提示，应继续排查游戏入口执行之前的微信包启动过程，而非猜测渲染修复已生效。

用户随后确认小米 15 / Android / 微信 8.0.77 仍停在微信启动页，完全无 HE 提示：Android `runtime-manual` FAIL，根因未定位。提示不可见本身不能证明入口没有执行（原生提示也可能不可用或被启动页遮挡）。红/蓝方块是样例状态反馈，不能作为工程版本标识。

为消除双窗口干扰，针对同一 `out/agent/wechat-export` 先执行 `wechatide.cmd -c Codex close_project_window --project ...` 和 `cli.bat close --project ...`，再仅以 `cli.bat auto --project ...` 打开。新接口 `automation_viewport_action --project ... --action remote --auto` 在新窗口下超时；切到旧 CLI 窗口后返回 `cant find runtimeid by projectpath`，日志 `out/agent/wechat/evidence/wechat-android-remote.log`。这是本地自动化接口失败，不是手机错误日志，也未建立真机调试连接。下一步在该工程窗口手动触发「真机调试」并手机扫码，采集首次启动错误；不重复发送普通预览代替诊断。

### Android 资源阻塞修复（2026-09-10）

用户手动建立真机调试并提供完整日志。可确认入口执行过 `entry → load-js → webgl2 → wasm → wasm-ready → resources`；明确的 HE 失败是 `Packaged resource size mismatch: Assets/WeChatSample/Touch.he`，诊断阶段为 `resources`。因此手机已执行 JS 并完成 Wasm 实例化，但尚未进入 HE 场景/首帧。`getSystemInfo: jsbridge not ready` 是在此之前发生的基础库日志，不能用它替代这条明确的资源错误。本次手机启动页没有显示原生 HE 提示，但没有肉眼可见提示不等于入口未执行。

本地原场景为 426 字节，导出到 `.he.json` 后本地字节相同；旧错误没有记录手机读到的实际字节数。怀疑微信包对 JSON 文本的处理改变了载荷，具体是压缩、换行还是其他转换尚未通过手机原始字节确定，不能当作已证实的转换机制。

修复：包内场景使用 `Assets/WeChatSample/Touch.he.bin` 原样复制，继续按原 `.he` 路径挂载并通过既有 SceneDocument 加载。已从安装的微信开发者工具 2.02.2608070 的 `app.asar/js/common/miniprogram-builder/utils/white_ext_list.js` 只读核实 `.bin` 位于 `GameWhiteList`；未修改工具自身或平台白名单。保留严格长度检查，错误增加包内路径、预期长度与实际长度。新增测试验证 BOM/中文/CRLF 原样传递及资源身份/哈希保持，损坏长度必须在 C++ 初始化前失败。

此修复仍需手机打开新包验证；不能将本地测试、包生成或模拟器成功视为 Android 绘制成功。iOS 仍未验证。

本轮执行结果：

- build-only / unit-test：`hectl.bat wechat check` PASS，包含 11 项导出测试、宿主/诊断故障契约、无 DOM Wasm 与 Windows 优化 C++ 样例契约；证据 `out/agent/logs/wechat-resource-fix-20260910-021736/`。
- runtime-manual（辅助浏览器）：`node Tools/WeChat/browser_check.cjs` PASS，真实 Wasm 绘制、触摸状态、存档重载；Node/NODE_PATH 使用本文前述路径，同目录 `browser-result.json`。不是本轮微信模拟器或真机通过证据。
- 调试包生成：`"E:/Program Files (x86)/Tencent/微信web开发者工具/cli.bat" preview --project C:/WorkSpace/HEngineDev/out/agent/wechat-export --qr-format image --qr-output C:/WorkSpace/HEngineDev/out/agent/logs/wechat-resource-preview-20260910-021944/preview.jpg --info-output C:/WorkSpace/HEngineDev/out/agent/logs/wechat-resource-preview-20260910-021944/preview-info.json` PASS。CLI 报告 TOTAL=2,007,795 字节，仅代表该命令包体口径。二维码、清单及完整日志在该目录，已按 Storage 工作流登记。
- 本轮仅修改微信导出/JS 桥接/测试；没有修改 C++ 或桌面渲染路径，没有重复运行 DX12/Vulkan 全量回归，既有桌面证据见上文。新包 Android 首帧仍等待用户扫码确认。

### Android 首帧图形诊断（2026-09-10）

用户打开 `.he.bin` 修复包后收到 `HE 启动失败 / first-frame / Error: frame: WebGL draw failed`。这确认已经越过之前的资源长度检查并完成 HE 场景/玩法初始化，但第一次图形提交仍失败，Android 可见绘制仍为 FAIL；没有据此宣称真机运行通过。

原渲染器只在整帧结束查询一次 GL 错误并丢弃错误码，无法区分初始化遗留错误与本帧错误。现在在微信平台的 `SokolRenderer.cpp` 启用固定版本 Sokol 的 GL 检查点和完整日志，保留首次错误的枚举名、数值、函数/行号；初始化各资源分别检查，首帧额外检查 pass、pipeline、bindings、uniforms 和 draw。只在初始化/首帧开启密集 GL 查询，随后仍保留帧边界错误检查。通过 HE 原有异常/错误返回契约停止并把详细信息传至弹窗，不吞掉 GL 错误、不修改第三方头文件、不替换渲染器。该包用于定位真机差异，尚未确定底层失败调用。

验证证据 `out/agent/logs/wechat-graphics-validation-20260910-023235/`：

- build-only / unit-test：`hectl.bat wechat check -Report C:/WorkSpace/HEngineDev/out/agent/wechat-graphics-export` PASS；包括优化 C++ 原生样例、无 DOM Wasm、导出/JS 宿主契约。
- runtime-manual + fault-injection：`node Tools/WeChat/browser_check.cjs C:/WorkSpace/HEngineDev/out/agent/wechat-graphics-export` PASS。辅助浏览器保留正常渲染/交互/存档检查，另在真实 WebGL `bufferData`、`clearBufferfi`、`uniform4fv`、`drawArrays` 后调用非法 `enable` 来生成真实 `GL_INVALID_ENUM (1280)`（没有伪造 getError）。四个首次调用和第三帧绘制共五个故障用例均保留位置并停止，首帧后的常规错误检查也有效；详情 `browser-result.json`。
- 调试包生成：`"E:/Program Files (x86)/Tencent/微信web开发者工具/cli.bat" preview --project C:/WorkSpace/HEngineDev/out/agent/wechat-graphics-export --qr-format image --qr-output C:/WorkSpace/HEngineDev/out/agent/logs/wechat-graphics-validation-20260910-023235/preview.jpg --info-output C:/WorkSpace/HEngineDev/out/agent/logs/wechat-graphics-validation-20260910-023235/preview-info.json` PASS，CLI TOTAL=2,074,786 字节。

原 `out/agent/wechat-export` 在两个关闭接口调用后仍报 Windows WinError 32，用户也看不到对应窗口。没有杀开发者工具进程；本次使用独立且稳定的 `out/agent/wechat-graphics-export`，以新二维码识别本次诊断版本。最早一轮故障注入测试发现 Sokol 的 uniforms/draw 没有内部 GL 检查点，已补上 HE 边界检查后重跑通过。Windows 桌面 DX12/Vulkan 代码未修改，本轮没有重跑该路径；iOS 仍未验证。

### 启动提示与有界诊断（2026-09-10）

用户反馈上一诊断二维码仍停在微信启动页，无弹窗，尚未拿到该包的新 GL 错误。代码审计发现上一版显式启用 `SOKOL_DEBUG` 会进入依赖的 `while (glGetError() != GL_NO_ERROR)` 清错循环；持续错误可能阻塞单线程。这是确定存在的诊断风险，但未证实手机本次正卡在该循环。已撤掉此宏，保留 HE 自己的有界检查点；运行时 Shader driver 消息仍打印，Sokol 静态错误保留日志编号。

宿主现在先在同一个 WebGL 2 屏幕画布上 clear，并等待两次 requestAnimationFrame，再启动 Wasm。等待有 2 秒 deadline、失败/取消清理，不引入第二个屏幕 canvas。该背景是启动辅助帧，不计入游戏帧、不作为 HE 绘制验收；真实纹理场景仍需随后成功绘制。此调整旨在避免首次游戏 draw 之前的错误提示被微信启动页遮挡，真机实际显示效果仍待确认。

每个阶段写入本地 `he-wechat-last-boot`，失败仍写入 `he-wechat-last-boot-error`，均带由运行代码/Wasm/配置/资源计算的 `buildId`，不覆盖玩法存档、不发送自定义遥测。`Project/Config/WeChat.json` 集中设置 `enableDebug: true`；宿主按该配置调用 `wx.setEnableDebug({enableDebug:true})` 请求手机 vConsole，失败仅记录，不阻止游戏。签名核对腾讯 API typings 固定版本 `4cae82af7f3c4339d1f11aea8e672fb16051d24a` 的 `types/wx/lib.wx.api.d.ts`，对应官方文档 https://developers.weixin.qq.com/minigame/dev/api/base/debug/wx.setEnableDebug.html 。该开关对正式版也可能生效，未来准备发布配置时需明确关闭；本次仅生成已获授权的调试预览。

验证证据 `out/agent/logs/wechat-bootstrap-validation-20260910-024145/`：

- build-only / unit-test：`hectl.bat wechat check -Report C:/WorkSpace/HEngineDev/out/agent/wechat-bootstrap-export` PASS。新增测试覆盖首帧让出执行权、持久阶段/版本、调试显式开关、RAF不回调、取消、GL错误和原生 UI 缺失。
- runtime-manual / 故障注入：`node Tools/WeChat/browser_check.cjs C:/WorkSpace/HEngineDev/out/agent/wechat-bootstrap-export` PASS。正常 HE 绘制/交互/存档仍通过；启动记录确认 `bootstrap-presented` 位于 `wasm` 之前。原五项真实 GL 故障检查通过；另一个明确模拟的 `getError` 持续返回 1280 用例也及时返回 `Sokol setup` 错误，没有无界清错，并持久记录错误。没有将模拟错误当作手机驱动的实际错误码。
- 调试包生成：`"E:/Program Files (x86)/Tencent/微信web开发者工具/cli.bat" preview --project C:/WorkSpace/HEngineDev/out/agent/wechat-bootstrap-export --qr-format image --qr-output C:/WorkSpace/HEngineDev/out/agent/logs/wechat-bootstrap-validation-20260910-024145/preview.jpg --info-output C:/WorkSpace/HEngineDev/out/agent/logs/wechat-bootstrap-validation-20260910-024145/preview-info.json` PASS；CLI TOTAL=2,022,322 字节。

当前诊断工程为 `out/agent/wechat-bootstrap-export`，用新的二维码进入，不再要求用户找不可见的占用窗口。Android 游戏绘制仍未通过，iOS 未验证；需要此次手机弹窗/vConsole的具体 GL 错误或可见场景结果继续定位。

### Android CURRENT_PROGRAM 查询兼容（2026-09-10）

用户在小米 15 / 微信 8.0.77 上给出版本 `he-9df9ea137f31` 的精确错误：`scene / Sokol setup: GL_INVALID_ENUM (1280) at _sg_gl_reset_state_cache:11033`。固定 Sokol 源码的上一行是 `glGetIntegerv(GL_CURRENT_PROGRAM, ...)`。这是查询路径失败的真机证据；尚不能区分微信原生 getParameter 报错和返回值触发 Emscripten 的类型校验错误。顶部 FPS/Jank 面板及错误弹窗可见，不是场景绘制通过证据。

在 HE 的微信平台实现内适配 Sokol 的这一个整数查询：使用 Emscripten `glUseProgram` 维护的 `GLctx.currentProgram.name`，其他查询原样转发。在接管独占上下文时显式 `glUseProgram(0)` 建立初始状态；后续读取实际程序绑定，不固定返回零，不清除或吞掉 GL 错误。未修改 Sokol/Emscripten 依赖或桌面 renderer。该约定要求此上下文的程序绑定经 Emscripten；未来若引入直接 JS Shader 绘制，需要同步管理或重新建立状态。

本轮证据 `out/agent/logs/wechat-program-validation-20260910-024829/`：

- build-only / unit-test：`hectl.bat wechat check -Report C:/WorkSpace/HEngineDev/out/agent/wechat-program-export` PASS，Wasm、Windows 优化 C++ 玩法契约、严格导出与 JS 宿主检查通过。1333 个构建依赖输入检查无禁用依赖。
- unit-test（辅助浏览器集成）：`node Tools/WeChat/browser_check.cjs C:/WorkSpace/HEngineDev/out/agent/wechat-program-export` PASS。主动令 CURRENT_PROGRAM 查询抛错，完整纹理绘制、变换/透明、触摸、音频事件、存档重启、前后台和 resize 仍通过；另六项既有 GL 故障检查仍通过。模拟拒绝查询不是本轮真机成功证据。
- 调试包生成：`"E:/Program Files (x86)/Tencent/微信web开发者工具/cli.bat" preview --project C:/WorkSpace/HEngineDev/out/agent/wechat-program-export --qr-format image --qr-output C:/WorkSpace/HEngineDev/out/agent/logs/wechat-program-validation-20260910-024829/preview.jpg --info-output C:/WorkSpace/HEngineDev/out/agent/logs/wechat-program-validation-20260910-024829/preview-info.json` PASS；CLI TOTAL=2,022,602 字节；新 buildId `he-a1d1ac5b7b31`。

当前工程 `out/agent/wechat-program-export`。Android 新包绘制等待用户扫码确认；iOS 未验证。本轮未重新验证微信模拟器绘制或 Windows DX12/Vulkan，保留前述历史证据，不能用本轮原生玩法契约替代桌面图形回归。

### Android 用户确认（2026-09-10）

runtime-manual：用户扫描 `he-a1d1ac5b7b31` 的预览二维码，在小米 15 / Android / 微信 8.0.77 上反馈“正常了”。据此记录该包成功进入游戏、启动及可见画面验证 PASS。该反馈未逐项确认触摸、声音、存档重启和前后台恢复，不把这些项目标记为真机通过；iOS 仍未验证。本条更新前述“等待用户扫码确认”的状态，保留此前失败及定位过程作为证据。
