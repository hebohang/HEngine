# Engine 通用 GM 平台

GM 是 HEngine 的显式开发能力。它提供统一命令注册、强类型参数、风险确认、事务状态域、Owner 生命周期、tainted 标记、回执历史、RmlUi 面板、Python Binding 和 Dev Agent RPC。Project 只注册业务状态域和业务命令。

## 构建隔离

`HE_ENABLE_GM` 默认 `OFF`。日常 `hectl game`、`hectl agent`、`hectl test`、`hectl editor` 和 `Win-GenProjects*.bat` 会显式传入 `ON`；`hectl package` 会显式传入 `OFF`。

`PACK_GAME=ON` 与 `HE_ENABLE_GM=ON` 会在配置阶段直接失败。关闭 GM 时，`Runtime/DevTools`、`GMBinding`、GM Panel 和 Dev Agent target 不进入构建图。Cook overlay 还会排除 Engine 与 Project 的 `Assets/DevOnly` 和 `Scripts/DevOnly`，Package 验证会检查 `Game.manifest.json` 与新解包内容中不存在这些路径。

## 使用面板

开发 Game 或 Editor Runtime 中按 `F10` 切换 Engine-owned RmlUi 面板。面板从命令 Schema 自动生成布尔、整数、数字、字符串和枚举控件，显示默认值与范围，并对 `confirm` / `dangerous` 命令要求再次确认。执行结果、错误码、提交状态和 tainted 状态使用统一回执展示；历史最多保留 100 条。

## Dev Agent 与 hectl

Agent wire contract 只增加两个固定 RPC：

```text
gm.discover
gm.execute
```

发现当前项目注册的命令：

```powershell
.\hectl.bat agent start
.\hectl.bat agent gm-discover
.\hectl.bat agent stop
```

项目注册命令后，可通过 `hectl agent gm-execute` 传入 `command`、`arguments` 和 `confirm`。动态 Project 命令不会成为独立 Agent method。

## Project Python 注册

开发构建会在 Python 初始化后查找 `Scripts/DevOnly/GM/__init__.py` 并调用 `register()`。重载可调用 `HEngine.GM.ReloadProjectCommands()`；Package 不会复制该目录。

Project 先注册状态域，再注册使用该域的命令：

```python
HEngine.GM.RegisterStateDomain(
    OWNER,
    {
        "name": "game.profile",
        "displayName": "Profile",
        "persistent": True,
    },
    snapshot,
    commit,
    availability=availability,
    restore=restore,
    refresh=refresh,
    mark_tainted=mark_tainted,
)

HEngine.GM.RegisterCommand(
    OWNER,
    {
        "name": "game.profile.add_currency",
        "displayName": "Add Currency",
        "category": "Profile",
        "scope": "persistent-profile",
        "risk": "confirm",
        "stateDomains": ["game.profile"],
        "taintsCurrentSession": True,
        "arguments": [
            {
                "name": "amount",
                "type": "integer",
                "required": True,
                "default": 100,
                "minimum": 1,
                "maximum": 1000000,
            }
        ],
    },
    handler,
)
```

Handler 接收 `(arguments, staged_domains)`。它只修改 staged JSON；成功后 Engine 统一提交全部状态域。任一 commit 失败时，Engine 使用原快照逆序 restore，并调用 refresh。Project 不应在 Handler 内直接写存档。

Python Handler 返回普通 JSON-compatible 值即表示成功；也可返回 `{ "success": false, "code": "...", "message": "..." }`。状态域 commit/restore 使用相同结果约定。

枚举参数可同时提供 `choices` 与等长的 `choiceLabels`。前者是稳定传输值，后者只用于面板显示，可用于中文等本地化标签。

每个注册必须使用稳定 Owner。重载前调用 `HEngine.GM.UnregisterOwner(OWNER)`；Engine 也会在 Python 关闭前释放所有 Python Owner，避免热重载或解释器退出后遗留回调。

## C++ 注册

C++ 使用 `GMService::GetInstance().GetCommandRegistry()` 与 `GetStateDomainRegistry()` 注册相同的描述和回调。`GMOwnerScope` 可用于 RAII Owner 清理。所有调用入口最终都进入同一个 `DevCommandExecutor`，因此校验、确认、事务、tainted 和历史语义一致。
