# HEngine Vulkan-Headers intake

- Upstream: https://github.com/KhronosGroup/Vulkan-Headers
- Tag: `v1.4.352`
- Archive: `https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/refs/tags/v1.4.352`
- Archive SHA-256: `EA33EA99DCA80EDF0264B011B65286C0D9239918290C5C3E845AB8C161454015`
- Header version: `VK_HEADER_VERSION 352`

Only the public `include/` tree and upstream license/readme files are vendored.
HEngine supplies the small header-only CMake target locally so normal builds do
not depend on an installed Vulkan SDK or configure-time network access.
