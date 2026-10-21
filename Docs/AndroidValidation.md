# Android 实现验证记录

本次验证日期：2026-09-16。完整 Android shipping cook、ARM64 引擎、C# 宿主与 APK 发布已通过。最终安装包包含应用名称和游戏图标，默认传感器横屏。已在 Xiaomi 24129PN74C / Android 16 / Adreno 830 真机完成启动、菜单点击、进入游戏和后台恢复的有界验证；长时间性能与完整平台画面对照尚未验收。

## 最终产物

- 路径：`Project/Dist/Android/com.hengine.starbloomsurvivor-Signed.apk`
- 大小：79,043,224 字节（约 75.4 MiB）。
- SHA-256：`fb3a21230141198232eaba8b379e39f6c86956bfb4736bcc06f915b07b677127`
- ARM64，最低 Android API 29，目标 API 36；GPU 还须满足 Vulkan 1.3 渲染器能力检查。
- applicationId：`com.hengine.starbloomsurvivor`，versionCode：1。
- Release 构建、非 debuggable，Development 签名；签名验证通过 APK Signature Scheme v3。正式渠道应配置自己的 Release 密钥。
- `Project/Dist/Android/android-build.json` 保存产物和内容身份；`Project/Saved/Build/android.json` 记录发布成功。

## 已执行验证

下表命令均从仓库根目录执行。PowerShell 验证使用 `. Tools/Android/Toolchain.ps1; $tc = Import-HeAndroidToolchain` 加载仓库本地工具链；不会修改系统或用户环境变量。

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| build-only：工具链准备 | `.\hectl.bat android setup -AcceptAndroidLicenses` | PASS；免管理员权限安装，重复执行 PASS |
| static-check：发布环境 | `.\hectl.bat android check` | PASS |
| static-check：最终环境 | `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Android/Build.ps1 -Project Project -Action Check` | PASS |
| build-only：完整发布 | `.\hectl.bat android build` | PASS；重新 cook、交叉编译、C# 注册表和宿主、签名 APK、事务性发布 |
| codegen | 同上命令的反射生成阶段 | PASS；273 个生成输出，无手改生成文件 |
| static-check：最终安装包 | 同上命令内的签名、zipalign、VerifyPackage 校验 | PASS；12 个 AArch64 原生库、16 KB ELF 加载段、完整资源与哈希 |
| static-check：最终 Manifest | `Tools/Android/VerifyManifest.ps1 -Apk Project/Dist/Android/com.hengine.starbloomsurvivor-Signed.apk -Aapt2 "$env:ANDROID_SDK_ROOT/build-tools/36.0.0/aapt2.exe" -ApplicationId com.hengine.starbloomsurvivor -DisplayName 'Starbloom Survivor' -VersionCode 1 -MinimumSdk 29` | PASS；标识、名称、图标、入口、横屏、最低 SDK、非调试配置 |
| unit-test：触控模型 | `dotnet run --project Tools/Android/Tests/AndroidContracts.csproj --configuration Release --artifacts-path out/agent/android-contract-tests` | PASS，14 项断言 |
| unit-test：包校验器 | `py -3.11 Tools/Android/Tests/test_package.py` | PASS，9 项测试 |
| static-check：差异 | `git -c core.autocrlf=false diff --check` | PASS |

此前 2026-09-15 的 Windows Vulkan 回归：

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| runtime-manual：Game 有界启动 | `.\hectl.bat game check -GraphicsApi Vulkan -StrictGraphicsApi` | PASS，构建和退出码 0 |
| runtime-manual：Editor 有界启动 | `.\hectl.bat editor check -GraphicsApi Vulkan -StrictGraphicsApi` | PASS，构建和退出码 0 |

## 日志

- `out/agent/android-toolchain/setup-final.log`：最终重复安装与工具完整性检查。
- `out/agent/android-toolchain/preflight-final.log`：发布前环境检查。
- `out/agent/build-publish/02A39F226444/android/build.log`：最终 APK 构建、签名和包校验。
- `out/agent/build-publish/02A39F226444/android/android/host-package.log`：Windows 主机 shipping cook 和资源验证。
- `out/agent/build-publish/02A39F226444/android/android/native/codegen/result-Release.json`：反射生成结果。
- `out/agent/android-toolchain/windows-game.log`、`windows-editor.log`：此前桌面有界启动。

最新全屏适配验证见 [自适应 UI](UILayout.md#验证记录)。以下黑屏修复记录为全屏适配前的历史验收。

## 2026-09-16 真机黑屏修复

初版在 Prefab 实例化时调用 CrossGuid 的未初始化 JNIEnv，触发 SIGSEGV。Android 现在通过系统 getrandom 生成 UUIDv4，保留 CrossGuid 的值类型与序列化，不修改第三方源码。另修复 .NET Android 增量发布的空静态 JNI 回调表，使用运行时注册；引擎日志通过平台层接入 logcat。

真机还发现 Vulkan 横屏旋转错误与连续 SUBOPTIMAL 重建。交换链使用可用的 identity transform，由呈现系统旋转；这种有意的方向差异不再触发重复重建，OUT_OF_DATE 与窗口变化仍正常处理。

最终 APK 在完整 shipping cook 的基础上，增量重建原生库并重新发布宿主，重新执行所有包校验后更新到 Project/Dist/Android。上述 SHA-256 对应手机实际安装的最终包。

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| build-only | `cmake --build out/agent/build-publish/02A39F226444/android/android/native --target HGame -j 4` | PASS，最终原生修复 |
| build-only / static-check | `out/agent/android-toolchain/Publish-DeviceFix.ps1` | PASS；脚本记录完整 dotnet publish、签名、zipalign、Manifest 和资源/ELF 检查命令 |
| runtime-manual | `out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb install -r out/agent/android-device-fix/com.hengine.starbloomsurvivor-Signed.apk` | PASS，同签名覆盖安装 |
| runtime-manual | `out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell am start -W -n com.hengine.starbloomsurvivor/org.hengine.runtime.GameActivity` | PASS，冷启动及后台返回 HOT；进程 19994 保持 |
| runtime-manual | `out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell input tap 1778 1000` | PASS，菜单进入实际游戏；场景、敌人、HUD 显示 |
| runtime-manual | `out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell input swipe 800 925 1000 925 1500` | 输入命令成功；后续截图显示升级界面，不作为精确位移或多点触控断言 |
| runtime-manual | `out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell input keyevent KEYCODE_HOME`，随后执行上述 am start | PASS，恢复显示升级界面；仅 Surface 恢复时重建交换链，无每帧重建 |
| runtime-manual：Windows 回归 | `.\hectl.bat game check -GraphicsApi Vulkan -StrictGraphicsApi` | PASS，最终 Vulkan 改动后重新构建并有界启动退出 |

UUID 原生单元测试（真实手机执行，PASS：4 线程、4,000 个 UUID、版本/variant/唯一性/序列化）：

```powershell
& out/agent/android-toolchain/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang++.cmd -std=c++20 -static-libstdc++ -I Engine/Source/ThirdParty/crossguid Engine/Source/ThirdParty/crossguid/Guid.cpp Engine/Source/Runtime/Platform/Android/AndroidGuid.cpp Tools/Android/Tests/AndroidGuidTest.cpp -o out/agent/android-toolchain/android-guid-test
& out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb push out/agent/android-toolchain/android-guid-test /data/local/tmp/he-android-guid-test
& out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell chmod 700 /data/local/tmp/he-android-guid-test
& out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell /data/local/tmp/he-android-guid-test
```

日志：`out/agent/android-toolchain/device-crash.log`、`device-final-package.log`、`device-final.log`、`device-fix-windows.log`。
截图：同目录 `device-menu-preview.png`、`device-final-gameplay-preview.png`、`device-resumed-preview.png`。测试启动的应用已停止，手机保留最终 APK 和应用数据。

尚未完成：同时多指操作、音频听感、锁屏与方向反转组合、16 KB 页设备实际装载、长时间性能、Windows 同场景完整画面对照，以及 AAB 和生产密钥签名。返回键在升级界面会显示退出确认；此轮不宣称已覆盖所有游戏状态的暂停交互。

工具链安装、发布密钥和真机验收步骤见 [Android 发布说明](Android.md)。
