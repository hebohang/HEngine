# 子树 Prefab 实施与验收记录

2026-09-13；本记录描述当前实现，编译、单测和实际角色验收由总实施记录登记。源码落盘和静态检查不等于阶段 6A 已通过。

## 工具与格式

现有 `PrefabSerializer::SavePrefab` 保存根及全部 Transform 子节点；现有 InstantiatePrefab 重载保持兼容。先解析/展开/校验整份文档，再分配全部实体 UUID、装配组件，最后启动脚本。组件或脚本失败会拆除本次创建的全部实体。通过现有 RuntimeResourceRegistry acquire/group pin 获取根与嵌套文档，没有平行资源缓存。

文件扩展名仍为 `.hprefab`。旧组件对象按单 root 读取，根 ParentUuid 清空；新保存使用 schemaVersion 2：

```json
{
  "schemaVersion": 2,
  "root": "actor",
  "entities": {
    "actor": {"TagComponent":{"Tag":"Actor"},"TransformComponent":{"ParentUuid":""}},
    "socket": {"TagComponent":{"Tag":"Socket"},"TransformComponent":{"ParentUuid":"actor"}}
  },
  "nested": [{"id":"weapon","path":"Assets/Prefabs/Weapon.hprefab","parent":"socket","overrides":{}}]
}
```

每个实体的模板局部 ID 与运行 UUID 分离；重复保存复用原 local ID 或源场景 UUID。nested 使用 `anchor/childLocalId` 命名空间，嵌套根使用 anchor 本身；重复 local ID、父子环、路径依赖环、外部引用及缺失节点均报错。上限为 32 层嵌套、4096 个展开实体、256 层父子关系；场景事务仍服从现有 1024 实体/8 MiB 限额，不放宽既有预算。

`PrefabInstanceComponent` 由正式反射序列化，保存 TemplatePath、LocalId、InstanceRootUuid、BaselineJson、OverridePathsJson、TemplateRevision、TemplateDocumentJson。最后一项保留嵌套声明和展开 ID 清单，防止保存时静默丢掉嵌套依赖或复活已删除嵌套子节点。此元数据由作者服务管理，不作为任意可编辑组件公开。

## 场景事务

原 `scene.describe/query/plan/apply/undo/redo/save` 服务增加以下 operations；原 revision、requestId、外部场景写入冲突和 writer rollback 保持适用：

- `{"op":"prefab","path":"Assets/Prefabs/Actor.hprefab","temporaryId":"$actor"}`：一次事务创建整个子树，可加 name、parent、components 根覆盖。
- `{"op":"prefab_sync","entity":"实例根UUID"}`：三方比较原 baseline、当前实例、新模板，更新未覆盖字段，保留本地覆盖并加入新子节点。
- `{"op":"prefab_override","entity":"UUID","pointer":"/TagComponent/Tag","mode":"set"}`：显式覆盖，即使当前值恰好等于模板也保留。mode=clear 恢复当前 baseline 并清除该覆盖及子路径。
- `{"op":"prefab_unpack","entity":"实例根UUID"}`：在可撤销事务中移除整实例的 Prefab 元数据，保留实体与引用。

普通 rename/patch/transform/reparent/material 操作会记录覆盖。模板删除有覆盖的字段/组件/实体，或删除仍被其他实体引用的节点时，整个同步返回冲突，不部分写。无覆盖的删除可以传播。多实例通过 InstanceRootUuid 隔离；duplicate 整实例会重映射此 root，单独复制受管理子节点需先 unpack。

prefab plan 返回 `prefabRevisions`，同一 requestId 的 apply 会比较展开内容 token，包含嵌套依赖变化。最多保留 256 份未应用计划，超限明确报错；不会悄悄丢弃旧计划的冲突保护。`prefab_sync.revision` 也可显式传递期望 token。Token 为确定性内容标记，不宣称加密哈希。场景事务撤销实例变更，不会暗中撤销外部模板文件。

## 引用与脚本

只重映射已知引用字段：TransformComponent.ParentUuid、启用目标时的 FollowCameraComponent.TargetUUID、EditorEntityStateComponent.FolderUuid。未声明的普通字符串从不做搜索替换。根 ParentUuid 在保存时解绑；其他外部实体引用需要先移除或改为子树内部引用。

PythonScriptComponent 已进入相同查询/描述/计划/应用/撤销/保存适配，Path 必须是存在的项目内 `.py`（空或 None 表示不绑定）。角色所需的 FollowCamera、CapsuleCollider3D、Rigidbody3D、Animator、Sound、CharacterMovement 也复用反射 canonical/default 适配；编辑仍禁止发生在 play 状态。脚本执行沿用现有 PythonScriptSystem，不在纯文档事务中执行任意脚本。

Python 源码中的硬编码 UUID 不自动修复。C++ `PrefabSerializer::FindInstanceEntity(instanceMember, localId)` 可在本实例查询挂点；公开 Python 绑定由主任务集成。表现组合复用 PythonScriptComponent，组合脚本负责 VFX/音频/灯光等运行行为，不新增空壳 VFX 组件。

## 验证

新增 `UnitTest/PrefabSubtreeTests.cpp`，注册函数 `HEngine::Tests::PrefabSubtreeContract`，建议名称 `PrefabSubtree`。覆盖多实例隔离、相机/父引用映射、覆盖保留与清除、删除字段/实体冲突、新增节点传播、旧单实体兼容、父子与资源环、嵌套保存展开往返、反射元数据往返、scene plan/apply 幂等、undo/redo/save/reopen、模板外部修改冲突、PythonScript 事务与 writer 故障回滚。

待主代理加入新源文件和测试注册后统一执行：

| 证据类型 | 命令 | 当前状态 |
| --- | --- | --- |
| codegen（独立报告） | `.\hectl.bat codegen run --json` | 未执行 |
| unit-test | `.\hectl.bat test PrefabSubtree` | 未执行 |
| unit-test | `.\hectl.bat test RuntimePrefabResource` | 未执行 |
| unit-test | `.\hectl.bat test SceneEditing` | 未执行 |
| runtime-manual | `.\hectl.bat game check` | 未执行 |
| runtime-manual | `.\hectl.bat editor check` | 未执行 |

静态 `git diff --check` 对本次文件返回 0。真实角色模型/碰撞/脚本/挂点/VFX 场景、多个实例、模板传播与保存重开、DX12/Vulkan 及包发布仍必须由主任务完成，不能被这些测试源码代替。
