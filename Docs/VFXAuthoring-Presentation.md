# 表现组合实施与边界

`Project/Scripts/PresentationPlayer.py` 提供 `PresentationService`。可编辑版本1 JSON 示例为 `Project/Assets/Effects/Authoring/Compositions/impact.json`，使用既有 `.efkpkg`、Audio Play3D、`.hprefab`、PointLight 和 Transform；没有新建 Registry。`PresentationDemo.py` 是可挂载的薄包装，PythonScriptComponent.Path 为 `Scripts.PresentationDemo.py`。Editor/Agent 通过现有场景事务挂脚本，脚本生命周期负责统一停止，不能直接改写场景文件绕过revision/undo。

公开调用：`service.load(path)` 验证并保留候选快照；`service.play(path, request_id, target, camera, parameters)` 调度；`update(seconds)` 推进；`cancel(request_id)` / `stop()` 清理；`status(request_id)` 查询。非法参数、缺依赖、错误版本在播放前失败。同request_id同参数返回原状态，不重复播放；不同参数复用ID明确报错。最近128个请求保留幂等状态，容量不足明确失败。

轨道支持粒子、音效、临时灯、相机震动，以及 SpriteRenderer.Color 的有限材质颜色过渡适配。任意 Mesh 材质动画未有对应公开服务，明确拒绝，不假装支持。所有轨道使用秒计时，有限结束；paused 属性同时暂停调度、VFX和Audio。附着位置使用 Entity.GetWorldPosition，offset为世界空间米，不把子树局部坐标误作世界坐标。骨骼挂点需要调用者传入代表挂点的实际实体。

震屏每次先移除上次偏移再合成新偏移，取消和结束还原；目标已销毁时不访问已释放组件。临时灯实例由组合拥有，结束/取消销毁；粒子音效显式停止。原始Sprite颜色在结束/取消恢复。同目标并行Sprite颜色轨道会拒绝，避免互相覆盖。该服务应由场景单一拥有者管理，同一相机与其他直接改Transform的玩法系统仍需约定更新次序。

`consume_animation_events(animator, mappings, target, camera)` 消费当前Animator事件并按事件名映射组合路径。消费后不重复注册回调，后续重播事件生成新的请求ID。玩法仍负责判定命中与伤害。

源码核查 HCooker/Cooker.cpp：`.json` 分类Config且递归扫描字符串依赖；当前Game.json的CookedJsonExtensions不含.json，因此组合保持可由json.load读取。Python绝对模块导入和资源字面量沿用现有依赖扫描。实际Shipping cook运行尚待主代理验证，不把源码检查称作打包PASS。

unit-test：`& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' Tools/VFX/test_presentation.py` PASS，6项（完整结束清理/震屏恢复、幂等与实体销毁、音效失败回滚粒子、错误reload保持旧资源、Sprite颜色恢复、暂停传播）。这些使用假HEngine验证调度契约，尚不是实际Game/Editor视觉或音频证据。

绑定补充位于 EcsBinding.hpp：PointLight/Sprite GetComponent和HasComponent、Entity.GetWorldPosition、Animator Pause/Resume/Step/ConsumeEvents。当前编译与实际运行由主代理统一完成；脚本不得以未编译的新接口报告运行PASS。

`impact.json` 当前加入第五条 `sprite_color` 演示轨，0.6秒内从场景owner初始蓝色过渡至橙色，完成后恢复初值。`PresentationAcceptance.he` 的owner有对应SpriteRendererComponent。它验证已实现的Sprite颜色适配，不代表任意Mesh材质参数动画。添加后6项组合单测与资源验证仍PASS；实际场景颜色/恢复需主代理验收。
