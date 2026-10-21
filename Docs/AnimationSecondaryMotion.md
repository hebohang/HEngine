# 次级骨骼运动

`AnimatorComponent` 的可选 `SecondaryBones` 在运行时动画混合完成后，
对指定骨骼及其子树施加世界空间弹簧运动。适用于发片、围巾、衣摆、饰物。
资源中的骨架与动画不被修改；每个 Animator 保存独立的模拟状态。
没有配置次级骨骼时保留原有动画路径。

```json
{
  "SecondaryBones": [
    {
      "Bone": "Hair.Back",
      "Tail": [0, 0.2, 0],
      "Stiffness": 110,
      "Damping": 14,
      "GravityScale": 0.03,
      "MotionDrag": 0.2,
      "MaxAngleDegrees": 24,
      "Radius": 0.012
    }
  ],
  "SecondaryColliders": [
    {"Bone": "Head", "Offset": [0, 0.08, 0], "Radius": 0.085}
  ],
  "SecondaryWind": [0.5, 0, 0.1]
}
```

骨骼名必须来自实际导入的骨架。`Tail` 是绑定姿态下的骨骼局部坐标，
不要求骨骼统一朝向；Blender 骨骼通常沿局部 +Y。
同一链条按父骨骼到子骨骼排列，每个骨骼只配置一次。
球体 `Offset`、`Radius` 和发梢接触半径 `SecondaryMotionBone.Radius` 都使用
骨架局部单位，随角色缩放一起转换到世界空间；例如厘米骨架置于 0.01
缩放下，`Radius=0.8` 表示 0.8 厘米。风力为世界空间加速度。
`MotionDrag` 默认 0，非零时按发根的世界空间速度施加反向加速度，
用于跑动形成可调后掠；阻尼作用于发梢相对发根的速度。

实现采用最多 30 个 120 Hz 子步、阻尼、长度/角度约束与球形末端碰撞。
超过 250 ms 的时间跳变、超过 2 m 的骨骼原点移动，或单帧骨骼轴转过
100° 时会重置历史状态；普通低帧率会完整积分已流逝的时间。
动画停播或暂停时不继续模拟，恢复时从当前姿势继续。

这是骨骼弹簧，不是逐顶点布料或发丝求解器：碰撞只约束骨骼末端，
没有自碰撞或逐发丝求解；发片材质另有各向异性着色，面部形变由独立系统驱动。
配置的球体与长度/角度约束冲突时，长度和角度约束优先。

示例：`Examples/AsterCourtyard/Tools/prepare_character.py` 创建发梢与围巾骨骼，
`Tools/assemble_demo.py` 写入反射配置。不要绕过反射添加专用序列化分支。

Python 脚本可通过 `entity.GetComponent('Animator').SecondaryWind` 调整风力。
本次同时补齐 `Camera`、`FollowCamera`、`SpringArm` 的 `GetComponent` /
`HasComponent` 别名；完整的 `*Component` 名称也可用，引用遵守已有的
实体销毁及组件替换失效规则。

验证命令：

```powershell
.\hectl.bat test AnimationPoseContract
.\hectl.bat test PythonRuntime -Project Examples/RenderingTests
```
