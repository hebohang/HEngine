# Android 原生发布

## 架构

Android 采用 ARM64 原生 Runtime、Vulkan/NVRHI 与 .NET Android 宿主。场景、材质、光照、阴影、后处理、Effekseer、RmlUi、物理与导航复用桌面实现，默认 Desktop 画质。当前 Android 配置使用 CSharpScript 插件和现有 C# 游戏逻辑。

最低 Android API 29；GPU 还必须满足现有 Vulkan 1.3 渲染器的能力检查。系统版本本身不保证 GPU 能力。此路径没有 OpenGL 降级，实际效果和帧率需要目标手机验收。

- `VulkanSurface` 隔离 GLFW 与 ANativeWindow；Surface 销毁前等待 GPU 并释放交换链，恢复后复用设备和 GPU 资源。
- `Window::WaitForFrame` 管理帧可运行条件。切后台暂停渲染和音频，恢复时重置时钟采样基点，避免追赶后台经过的时间。
- `ImGuiPlatform` 隔离窗口和输入，NVRHI ImGui 绘制实现共享。
- Activity、JNI 与 Android 原生类型位于 `Runtime/Platform/Android`。Activity 管理 Surface 和 .NET，工作线程运行引擎，脚本复用现有 ABI。
- Windows 主机负责反射生成、Shader 编译和 cooker，NDK 编译目标 Runtime；第三方依赖通过独立 CMake 组合，不修改第三方源码。

## 横屏与交互

默认传感器横屏，游戏视口铺满窗口（允许延伸到刘海区域），控件留白由 UI Slot 配置，不再由 Activity 固定为 16:9。UI 的锚点、边距和尺寸由引擎 Canvas/Slot 布局处理，详见 [自适应 UI](UILayout.md)。

菜单单指点击；游戏中左下摇杆移动、右侧拖动视角、右下跳跃、右上暂停。返回键在游戏中暂停，其余状态确认退出。手指角色固定至抬起，取消、切后台和窗口变化清空输入，短按边沿进入有界事件队列。游戏通过 `input.touchControls` 控制覆盖层显隐。

## 环境与构建

Android 工具链由引擎管理，默认安装到 Git 忽略的 `out/agent/android-toolchain`。使用免安装 ZIP 和本地 .NET SDK 的文件式工作负载，不需要管理员权限、Android Studio、全局 JAVA_HOME 或系统 PATH 修改。VS/CMake 和 Python 仍使用引擎既有主机构建环境。

先阅读 [Android SDK 许可](https://developer.android.com/studio/terms)。从仓库根目录执行以下命令；`AcceptAndroidLicenses` 表示接受本次安装所需 SDK 包的许可：

```powershell
.\hectl.bat android setup -AcceptAndroidLicenses
.\hectl.bat android check
.\hectl.bat android build
```

首次 setup 需要联网和数 GB 磁盘空间。下载包校验后解压，重复执行复用已有工具，不升级全局 .NET。版本与下载哈希位于 `Tools/Android/toolchain.lock.json`：.NET SDK 10.0.302、Microsoft OpenJDK 21.0.12.1、NDK r30、SDK command-line tools 22.0、platform android-36 和 build-tools 36.0.0。工作负载安装使用 SDK 自带 manifest，并禁止下载新版 manifest。platform-tools 由 SDK Manager 提供，是独立于 APK 编译器的设备工具。

`HE_ANDROID_TOOLCHAIN_ROOT` 可指定另一处持久缓存。高级用法仍支持显式 `JAVA_HOME`、`ANDROID_SDK_ROOT`/`ANDROID_HOME`、`ANDROID_NDK_HOME`，优先于对应的本地默认路径；自定义安装也需匹配项目固定 NDK 和 .NET 版本。环境只在构建进程及其子进程内生效。

初次安装和 NuGet restore 完成后，构建复用本地工具与缓存；新项目引入的 NuGet 包仍可能需要下载。不要删除缓存后期待离线首次构建。

也可使用 `Win-PackAndroid.bat` 或编辑器发布面板的 android 配置。配置位于 `Project/Config/Android.json`，默认输出 `Project/Dist/Android`；中间产物和日志位于 `out/agent`。`icon` 指定项目内 PNG 路径，当前复用 `Resources/Windows/Game.png`；未设置时使用系统默认应用图标。

参考：[NDK 下载](https://developer.android.com/ndk/downloads)、[Android SDK](https://developer.android.com/studio)、[.NET Android](https://learn.microsoft.com/en-us/dotnet/android/)、[原生 Vulkan](https://developer.android.com/games/develop/vulkan/native-engine-support)。

### 为什么不直接放进 ThirdParty 源码目录

SDK/JDK/NDK 是主机构建工具，游戏运行时不需要安装它们。放进 ThirdParty 并不能消除依赖，只会将数 GB 二进制纳入源码分发。Android SDK 许可对整体再分发有限制，开源组件另按各自许可处理，因此本引擎保存安装脚本和版本锁，从官方源按需下载，并保留包内许可文件。

其他引擎也采用外部工具链管理：

| 引擎 | Android 工具链管理 |
| --- | --- |
| [Unity](https://docs.unity3d.com/6000.0/Documentation/Manual/android-sdksetup.html) | Hub 的 Android 模块安装配套 SDK/NDK/OpenJDK，也支持自定义路径。 |
| [UE5](https://dev.epicgames.com/documentation/unreal-engine/automated-android-sdk-ndk-and-jdk-setup-in-unreal-engine) | Turnkey 自动准备 SDK/NDK/JDK，并按引擎版本要求校验。 |
| [Godot](https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_android.html) | 配置 JDK、Android SDK 路径并使用导出模板；构建引擎本身需要更多工具。 |

当前游戏使用 C#，所以 APK 内还需要 .NET Android 运行库。消除它需要重写游戏脚本或引入另一套编译/运行方案；这与复用 Windows 游戏行为的目标不相称。构建流程自动准备它，手机端无需额外安装 .NET。

## 签名与资源

默认 APK 使用 Development 签名，用于安装验证。正式发布设置 `signing: Release`；商店 bundle 设置 `format: aab`。Release 从环境读取以下值，密码不写入 JSON 或命令参数：

- `HE_ANDROID_KEYSTORE`：已有 keystore 绝对路径。
- `HE_ANDROID_KEY_ALIAS`：别名。
- `HE_ANDROID_STORE_PASSWORD`、`HE_ANDROID_KEY_PASSWORD`：对应密码。

升级保持 applicationId 与签名密钥，递增 versionCode。构建不会上传商店或创建、覆盖签名密钥。

发布依次执行主机 shipping cook、ARM64 编译、Android C# 注册表生成、.NET Android publish、签名校验、包内资源哈希与所有原生库的 16 KB ELF 段校验；APK 另执行 zipalign，以及应用标识、名称、图标、启动 Activity、横屏和非调试配置校验。全部通过后生成 `android-build.json` 并事务性替换输出，失败保留上一版。

包中携带 cooked 内容、许可证和移动端运行库，不复制 Windows EXE/DLL。内容按 manifest 身份提取到应用私有目录并验证 SHA-256；存档单独位于 saved 目录，资源更新不覆盖存档。卸载应用会删除私有数据。

## 验收

使用 Android SDK 的 adb 列举设备后，明确指定设备执行 `install -r` 安装实际 Signed.apk。启动组件为 `com.hengine.starbloomsurvivor/org.hengine.runtime.GameActivity`，原生日志标签为 HEngine。AAB 需通过商店测试轨道或 bundletool 转为设备 APK。

真机覆盖菜单点击、多指移动/旋转/跳跃、暂停恢复、切后台/锁屏、Surface 重建、方向变化、音频、同签名升级与存档保持，以及与 Windows Vulkan 的同场景对照。另用 16 KB 页设备验证原生库装载。

```powershell
dotnet run --project Tools/Android/Tests/AndroidContracts.csproj --configuration Release --artifacts-path out/agent/android-contract-tests
py -3.11 Tools/Android/Tests/test_package.py
.\hectl.bat game check -GraphicsApi Vulkan -StrictGraphicsApi
.\hectl.bat editor check -GraphicsApi Vulkan -StrictGraphicsApi
```

模型单元测试不替代 Activity 编译、APK 签名和真机运行；仅原生交叉编译成功不代表 Android 发布成功。
