# 输入动作（桌面）

项目可在 `Config/Input.json` 定义版本 1 输入映射；未提供文件时使用内置 Move、Look、Jump、Sprint、Crouch、Interact、Pause
以及 UI 的 Navigate、Submit、Cancel。输入每个显示帧采样一次，同一帧多次读取不会消费边沿。
固定更新中多次读取会看到同一边沿；需要单次逻辑时在 Update 中处理，或由游戏记录已处理的帧。
Agent 暂停单步时呈现帧仍然采样输入；测试 Pressed/Released 应先恢复运行再注入，不能在暂停期间注入后等待多个呈现帧才步进。Held 状态可以用于暂停单步的持续输入测试。

```json
{
  "Version": 1,
  "Actions": [
    {
      "Name": "Interact", "Context": "Gameplay", "Type": "Button",
      "DeadZone": 0.15, "HoldSeconds": 0.4, "DoubleTapSeconds": 0.25,
      "Bindings": [
        {"Device": "Key", "Code": 69, "Axis": 0, "Scale": 1.0},
        {"Device": "GamepadButton", "Code": 2, "Axis": 0, "Scale": 1.0}
      ]
    }
  ]
}
```

文件是完整映射，不与默认表自动合并。上述示例只启用 Interact；若使用内置角色，另行定义 Move/Jump/Sprint。
二进制 SDK 的完整绑定码表见下方，无需读取 C++ 头文件。Axis/Vector2 的轴索引为 0/1，
反向输入使用负 Scale。绑定支持 Key、MouseButton、GamepadButton、GamepadAxis、PointerX、PointerY。
Vector2 使用径向死区，Axis 使用绝对值死区。手柄轴先独立应用死区，再与其他绑定组合，防止摇杆漂移
干扰键盘/十字键移动；其他绑定求和后应用死区。Axis/Vector2 超出死区后的力度
按 `(min(length, 1) - DeadZone) / (1 - DeadZone)` 连续映射到 0..1，保留方向；Button 保持阈值判断。
Vector2 长度最大为 1；Pointer 也遵循此归一化语义，不适合作为原始像素位移。

桌面通过 GLFW 标准 Gamepad 映射识别手柄，每帧选择首个已连接手柄；断开时动作回到中立状态。
内置角色保留 Move 的力度：轻推慢走、推满达到 MaxSpeed，冲刺和蹲伏倍率在此基础上生效，斜向不加速。
跟随相机读取 Look（右摇杆），无须按住鼠标右键；满推速度为 `600 * MouseSensitivity` 弧度/秒，
按帧时间积分，遵守相机的 yaw/pitch 开关与俯仰限位。使用现有灵敏度设置和瞄准减速。
UI Navigate 默认支持左摇杆和十字键，Submit 为 A，Cancel 为 B；项目须在菜单中读取这些动作。
未被 GLFW 识别为标准 Gamepad 的设备需要对应映射；本接口不提供震动。

```csharp
// 在 ScriptBehaviour 回调中使用。
if (InputActions.Read("Interact").Pressed) OpenDoor();
InputActions.Rebind("Interact", 0, new(ActionDevice.Key, 70)); // F
InputActions.Save(); // 原子写入玩家存档，不修改项目资源
// 游戏初始化时显式恢复；无存档返回 false，损坏的存档抛出异常，保留当前映射。
bool restored = InputActions.Load();
```

State 提供 Value、Pressed、Released、Held、HoldPerformed、DoubleTapped。
SetContext 只接受映射内已存在的上下文；切换、重绑定、载入玩家存档时抑制已按住的控制，松开后恢复。
若映射不包含当前上下文，使用首个动作的上下文。动作名在映射内必须唯一。

当前新增 C# 接口面向桌面，Web/WeChat 导出时会在 API 审计中报出调用位置。
原始 `Engine.Input` 保持现有行为；该接口不自动改写旧项目脚本。

## 1.1.0 绑定码表

`ActionBinding.Code` 和 JSON 的 `Code` 使用以下数值；范围中的名称和值一一递增。不要使用表外的间隙值。

| Device | 名称 | Code |
| --- | --- | --- |
| Key | Space；Apostrophe；Comma；Minus；Period；Slash；Semicolon；Equal | 32；39；44；45；46；47；59；61 |
| Key | D0–D9；A–Z | 48–57；65–90 |
| Key | LeftBracket；Backslash；RightBracket；GraveAccent；World1；World2 | 91；92；93；96；161；162 |
| Key | Escape；Enter；Tab；Backspace；Insert；Delete；Right；Left；Down；Up；PageUp；PageDown；Home；End | 256–269 |
| Key | CapsLock；ScrollLock；NumLock；PrintScreen；Pause | 280–284 |
| Key | F1–F25 | 290–314 |
| Key | KP0–KP9 | 320–329 |
| Key | KPDecimal；KPDivide；KPMultiply；KPSubtract；KPAdd；KPEnter；KPEqual | 330–336 |
| Key | LeftShift；LeftControl；LeftAlt；LeftSuper；RightShift；RightControl；RightAlt；RightSuper；Menu | 340–348 |
| MouseButton | Button0–Button7（Left=0，Right=1，Middle=2） | 0–7 |
| GamepadButton | A；B；X；Y；LeftBumper；RightBumper；Back；Start；Guide；LeftThumb；RightThumb；DPadUp；DPadRight；DPadDown；DPadLeft | 0–14 |
| GamepadAxis | LeftX；LeftY；RightX；RightY；LeftTrigger；RightTrigger | 0–5 |

`PointerX`/`PointerY` 的 Code 使用 0，表示该指针增量源。这里是动作绑定码；Agent 输入注入应另外用 `hectl agent keys` 查询该运行会话支持的规范键名，再使用小写名称。
