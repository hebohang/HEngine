# 第三方库

当前版本信息以仓库内已 vendored 的源码快照为准；没有明确 semver 的库会标注为 `未发现明确版本号（本地快照）`。

| 库 | 版本 | 备注 |
| --- | --- | --- |
| assimp | 6.0.4 | `assimp/CMakeLists.txt` |
| Box2D | 2.4.1 | `Box2D/CMakeLists.txt` |
| CrossGuid | 0.2 | `crossguid/README.md` |
| D3D12MemoryAllocator | 2.0.1 | `D3D12MemoryAllocator/include/D3D12MemAlloc.h` |
| DirectX SDK | 9.29.1962.0 | `Windows/Include/DirectX/dxsdkver.h` |
| DirectXTex（当前未接入） | jan2023 | 仅 README 记录，仓库当前未 vendored 独立目录 |
| Effekseer | 未发现明确版本号（本地快照，仓库原记录为 1.7.3.0） | 源码内未找到统一版本宏 |
| embree | 4.1.0 | `embree/include/embree4/rtcore_config.h` |
| EnTT | 3.9.0 | `entt/include/entt.hpp` |
| freetype | 2.13.2 | `freetype2/include/freetype/freetype.h` |
| glad | 0.1.35 | `Glad/include/glad/glad.h` |
| GLFW | 3.4.0 | `GLFW/CMakeLists.txt` |
| glslang | 11.9.0 | `glslang/CHANGES.md` |
| glm | 0.9.9.9 | `glm/glm/detail/setup.hpp` |
| ImGuizmo | 1.84 WIP | README 同时说明 latest stable tagged version 为 1.83 |
| imGuIZMO.quat | 3.0 | `ImGuizmoQuat/readme.md` |
| imgui | 1.92.8 WIP | `imgui/imgui.h`；当前仓库使用的是自维护 fork：<https://github.com/hebohang/imgui> |
| imgui-node-editor | 0.9.3 | `imgui-node-editor/docs/CHANGELOG.txt` |
| imgui-notify-v2 | 未发现明确版本号（本地快照） | 源码内未找到统一版本宏 |
| Jolt Physics | 5.5.0 | `JoltPhysics/Jolt/Core/Core.h` |
| magic_enum | 0.8.0 | `magic_enum/include/magic_enum.hpp` |
| nlohmann_json | 3.12.0 | `json/CMakeLists.txt` |
| pybind11 | 2.10.3 | `pybind11/include/pybind11/detail/common.h` |
| Python | 3.11.1 | `Python/include/patchlevel.h` |
| RecastNavigation | 1.6.0 | `recastnavigation/CMakeLists.txt` |
| RmlUi | 6.2 | `RmlUi/CMakeLists.txt` |
| shaderc | v2022.1 | `shaderc/CHANGES` |
| SimpleIni | 4.19 | `simpleIni/SimpleIni.h` |
| miniaudio | 0.11.25 | `miniaudio/miniaudio.h` |
| spdlog | 1.17.0 | `spdlog/include/spdlog/version.h` |
| SPIRV-Cross | 0.49.0 | `SPIRV-Cross/CMakeLists.txt` |
| SPIRV-Headers | 1.5.5 | `SPIRV-Headers/CMakeLists.txt` |
| SPIRV-Tools | v2022.2 | `SPIRV-Tools/CHANGES` |
| stb | stb_image v2.27 / stb_image_resize2 v2.18 | `stb_image/stb_image.h` 和 `stb_image/stb_image_resize2.h` |
| Tracy | 0.13.1 | `Tracy/public/common/TracyVersion.hpp` |
| Vulkan Memory Allocator | 3.0.1 | `vulkanmemoryallocator/include/vk_mem_alloc.h` |
| VulkanSDK | 1.2.198 | `VulkanSDK/include/vulkan/vulkan_core.h` |

## 备注链接

Imgui fork: <https://github.com/hebohang/imgui>

GLFW commit: <https://github.com/glfw/glfw/commit/8f470597d625ae28758c16b4293dd42d63e8a83a>
