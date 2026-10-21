# Editor 特效开发诊断

`Tools/VFX/editor_vfx_acceptance.py` 通过现有 owned Editor mailbox 在空白 3D 编辑场景中播放 CPU/GPU 效果、固定步进、捕获和结束会话。它是开发诊断入口；交付结果及未测范围见 [收尾验收](VFXAuthoring-Delivery.md)。

## 构建与运行

使用 `hectl editor build -Configuration RelWithDebInfo -Parallel 2` 建立独立 Editor 缓存。需要 GPU 诊断时，在该缓存配置 `HE_EDITOR_VFX_GPU_PROBE=ON` 并重新构建 Editor。该选项默认 OFF，仅允许非 PACK、非 DevGame 且启用 GM 的 Editor；运行时还必须显式设置 `HE_VFX_GPU_PARTICLES=1`。驱动仅对子进程设置环境，不改变生产 capability 或父进程环境。

```powershell
$editorHash = (Get-FileHash out/agent/editor-relwithdebinfo/bin/HEngineEditor.exe -Algorithm SHA256).Hash
py -3 Tools/VFX/editor_vfx_acceptance.py --backend DX12 --expected-editor-sha256 $editorHash --output out/agent/rendering-evidence/vfx-authoring/editor-vfx-dx12-new
```

Vulkan 用独立的新输出目录串行执行。驱动创建自己的空场景，核对实际场景身份和可执行文件哈希，使用 `hectl editor check`，在 finally 请求正常结束和 session.stop 并等待退出。不得改为保留用户进程的 open 路由。

## 请求与完成边界

`editor.vfx` 的 `action` 支持 `begin/state/describe/stats/play/status/stop/step/end`。play 接受现有 Assets 内的 `.efkpkg` 相对路径和 int32 seed，status/stop 只接受本会话拥有的 handle。单次 step 为 1–3600 帧，每帧固定 1/60 秒；会话累计上限 36000 帧，并预留正常 end 的 30 帧。最多持有 16 个效果，复用 mailbox 的 256 请求和 32 捕获限制。

begin 要求没有活动效果或待处理场景修改，并绑定当前空白 3D 编辑场景。诊断期间场景替换延后；只有收到 step 才更新特效。驱动检查实际 UpdateSerial 增量，暂停时序号必须稳定。未完成 step 时拒绝 normal end，避免后一个 ticket 掩盖未完成的请求。

正常 end 停止所有拥有的效果，继续 30 帧收尾，再于下一次 Poll 核对活动实例和 CPU allocator 为零，才返回 retired 并恢复实时预览。强制 Shutdown 是 stop 请求，不能冒充正常 retirement。step ACK 仅证明 SDK 更新和 RenderWorld 提交；retired 也不代表 GPU fence 或全部原生物理字节已经回收。

## 证据范围

代表性驱动采样 CPU 命中和 GPU 火花的多个时刻、自然结束、重播、停止及结束空场；记录真实提交计数、计时、资源计数、负面请求、原始截图与完整日志。协议单元测试只覆盖客户端逻辑，不能替代引擎运行。工具保留 `visualStatus=NOT_REVIEWED`，人工审图结论另存，不由截图存在推导视觉通过；严格内存、Release 性能和 Shipping 不在此入口的通过范围。
