# Starter Content

FirstPerson 与 TopDown 共用的基础内容，使用仓库已有的 Box 网格和 PBR shader，不引入外部模型、贴图或额外授权资源。

| 路径 | 用途 |
| --- | --- |
| Materials/Floor.hmat | 深蓝灰地面 |
| Materials/Wall.hmat | 灰色边墙与靶座 |
| Materials/Target.hmat | 橙色靶标 |
| Materials/Player.hmat | 青色玩家 |
| Materials/Cover.hmat | 黄色掩体与瞄准标记 |
| Prefabs/Floor.hprefab | 19 × 19 的基础地板 |
| Prefabs/Cover.hprefab | 2 × 2 × 2 的掩体 |
| Prefabs/Target.hprefab | 1.4 × 1.4 × 0.8 的靶标外观 |

共享资源按 `Assets/StarterContent/...` 引用，由 Engine/Content 回退提供。项目创建只复制模板自己的玩法、场景和配置，不复制共享库。视其为只读引擎内容；需要更改时复制资源到项目专有路径，再更新引用，可保持各项目外观独立。

Prefab 使用 schemaVersion 2，仅包含可复用的静态外观，不绑定工程脚本和计分规则。示例场景直接保存组合后的实体，并引用共享材质，方便从第一帧开始检查与编辑；场景实体不是自动同步的 prefab 实例。Box 网格局部范围为 [-1, 1]，Transform.Scale 是半尺寸。

新增模板可继续引用此目录。若添加需要独立维护的复杂角色、音频或特效，应另立资源路径及其来源说明，不替换现有共享资源。
