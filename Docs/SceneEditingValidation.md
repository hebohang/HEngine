# 场景编辑服务验证记录

日期：2026-09-08；Windows x64，VS2026 v145。所有构建使用 `out/agent` 独立目录，未使用 Visual Studio 共享构建目录。实现与使用契约见 [SceneEditing.md](SceneEditing.md)，批量请求见 [SceneEditing.batch.json](Examples/SceneEditing.batch.json)。

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| unit-test | `.\hectl.bat test SceneEditing` | PASS；最新运行 04:07:27，HEngineTests 构建及契约测试通过 |
| runtime-manual | `.\hectl.bat editor check -Frames 30` | PASS；最新构建通过，Editor 有界运行 exit=0 |
| runtime-manual | `.\hectl.bat game check -Frames 30` | PASS；最新构建通过，Game READY 后 exit=0 |
| runtime-manual | `.\hectl.bat editor capture -Frames 30` | PASS；有界 Editor 启动与截图成功，不代表逐个控件操作已测试 |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-SceneEditing.ps1` | PASS；脚本驱动的图形集成闭环，已停止 Agent 会话并清理临时场景 |
| codegen | `.\hectl.bat codegen run -CodeGenHost Editor -Configuration RelWithDebInfo --json` | PASS；225 输出，changed=0、deleted=0、hashChanges=0、mtimeChanges=0 |
| 静态检查 | `git diff --check` | PASS；仅 Git 行尾转换提示，没有 diff 空白错误 |

`runtime-manual` 表示引擎运行/图形集成证据；其中有界启动和闭环由脚本驱动，截图观察不是无窗口单元测试。未执行发布打包验证，未手动逐一操作全部 Editor 控件。

## 契约覆盖

无窗口测试验证批量临时引用、共享参数、事务 diff 分页、中间失败零修改、过期 revision、幂等重试与冲突、1024 条记录容量不淘汰、删除及 UUID/层级恢复、复制内部引用、世界/局部变换与层级环、奇异变换、非法字段/资源/材质槽位、Prefab、保存重开、Save As 失败绑定不变、dirty、redo 分支、查询投影分页、增量过期、外部编辑历史失效、手动修改通知、作者与运行副本隔离、ECS 生命周期适配及提交/恢复故障注入。

旧 UI 控件的直接写入边界在设计文档明确列出。测试验证服务的手动观察契约与运行副本隔离；没有把它表述为所有旧 UI 交互的端到端覆盖。

## 闭环产物

最新证据目录：`out/agent/scene-editing/20260908-040730/`。

- `request-*.json` / `receipt-*.json`：16 次场景服务调用，合计请求正文 4,993 字节（含一次幂等重试；不含 capture 传输信封）。
- `saved-scene.he`：6 实体、4,613 字节；含父节点、三个可见 Mesh 子对象、相机和灯光。
- `scene-editing.png`：共享缩放与材质覆盖后的预览帧。
- `summary.json`：脚本结果；保存后的投影数据与重新打开结果相等，保存/撤销/重做 dirty 断言通过。

此前已人工观察 `out/agent/scene-editing/20260908-034604/scene-editing.png` 中的三个对象和材质差异。最新图像与该参考图在 3,600 个固定采样点上的平均通道绝对差为 0；这是采样回归证据，不是全像素相等断言。遵守每轮最多检查两张新图像的约束，没有再次打开最新图像。

Editor 截图：`out/agent/captures/editor-relwithdebinfo-20260908-034426-765.png`，仅记录截图生成成功，未进行额外图像观察。

代码生成 manifest 输出列表的 hash/mtime 核对汇总：`out/agent/scene-editing/verification/codegen-noop.json`。构建及运行完整日志位于 `out/agent/logs/`。这次没有新增或修改反射字段，未手工修改任何生成文件。
