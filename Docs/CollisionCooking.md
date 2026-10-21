# 跨构建配置的三角碰撞资源

`.hcollision` 的 Jolt 序列化数据依赖精确 `JPH_VERSION_ID`，包含构建配置相关 ABI。RelWithDebInfo 导入器生成的原生 payload 不能直接解释为 Shipping Release payload；运行时始终严格拒绝这种错配。

新导入器写入 v2 容器：小端 64-bit `HECMESH2` magic、Jolt ABI、顶点数、payload 字节数；随后为连续三角形的 xyz float32 顶点，以及该开发配置生成的 Jolt `SaveWithChildren` 数据。顶点为引擎局部坐标、米制；必须非空、数量为 3 的倍数、有限值。文件总长限制 128 MiB，所有计数和范围先检查再分配或读取。

HCooker 使用自身目标配置的 Jolt，从 v2 原始顶点重新构造加速结构。输出 Shipping v1 容器（`HECMESH1`、目标 ABI、原生 payload），去掉仅制作过程需要的原始顶点。打包不会覆写开发资源。因此同一个开发工程支持开发运行与 Shipping 发布，无需用 Release 导入器破坏开发资产。

旧 v1 文件若 ABI 与 cooker 一致，校验实际 Mesh payload 后保留；ABI 不一致时明确报错，要求从原始 GLB/FBX 正常重新导入生成 v2。不提供掩码 ABI、运行时重烘焙或静默替换碰撞体的兜底。v2 在运行时也必须 ABI 一致；可移植数据仅由离线工具使用。

实现：`Runtime/Platform/PhysicsCollisionFormat.h`、`PhysicsMeshCooker.cpp`、`PhysicsCookedMesh.cpp`；打包入口 `Programs/HCooker/Cooker.cpp`。

验证入口：

- `HCooker --self-test`：真实双三角形生成、v2 转目标 v1、foreign ABI 的 v2 重烘焙、同 ABI 旧格式直通、异 ABI 旧格式拒绝、截断/非法数量/非有限顶点拒绝。
- `hectl test PhysicsScriptContact`：v1/v2 运行时严格 ABI 拒绝及既有真实物理接触契约。
- 正常开发配置重新导入 `SourceAssets/Collision/Architecture.glb` 和 `SourceAssets/EnvironmentUpgrade/GalleryDetails.glb` 后，运行 `hectl package -Project Examples/AsterCourtyard`；Shipping 实际启动仍是跨配置工作流的最终证据。

本文实现时仅完成静态检查，构建、自测及最终打包运行结果以主线验收记录为准。

Shipping 目录类型修复：Collision 已接入 cooker 分类与运行时 catalog 的枚举、解析、格式化及校验，并新增 `RuntimeResourceCatalog` 契约测试。审查发现既有 PipelineProfile 类型也存在 cooker/runtime 接入缺口，但本演示 staging manifest 未包含该类型，本轮未扩修，需单独跟进。
