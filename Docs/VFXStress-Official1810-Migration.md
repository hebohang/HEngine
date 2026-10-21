# Stress：官方 1810 可重建作者源迁移

2026-09-14，已发布授权的 GpuSparks20k/50k 新包，未运行 HEngine/GPU 或引擎构建。
正常生成入口 `Tools/VFX/generate_effekseer_stress_assets.py` 不再拼接旧 runtime 前缀
或手写 GPU 二进制块；只由当前官方 Effekseer.exe 从作者 XML 导出 EFKE。
原官方样例未改动。旧坏包保留在
`out/agent/rendering-evidence/vfx-authoring/legacy-stress-1800`（root 已登记 .he-keep）。

## 为什么必须迁移

原样例 BIN 声明 Version18Alpha1=1800，但 common 前保留了 16 字节碰撞参数；当前
SDK 对 <1801 不消费这些字节，导致 CPU common 起点从正确 727 提前到 711，读到
texture index -1 作为 sampler。这不是合法作者 filter=331 或后续材质参数污染。
此前 `VFXGpuStress-Sampler-Audit.md` 证明 sampler 的实际资源字段合法；后续实际加载
日志和 loader 分支才确定这项兼容性冲突。没有为所有 1800 数据加入猜测性跳过。

新官方输出 version1810，CPU common=770、sampler=810、GPU node=1050；七组 CPU
filter/wrap 均 Linear/Repeat。保留原共享 CPU sprite/GPU-enabled 节点，不删除 CPU
渲染路径绕过问题。仍不宣称该 CPU sprite 是 GPU 粒子效果的完整 fallback。

## 作者源与参数

`Project/SourceAssets/VFXAuthoring/Stress/<name>` 包含完整 `.efkproj`、官方 `.efkefc`、
`roundtrip.xml`、来源许可、PROVENANCE.json 和原配方径向 alpha 纹理。
manifest.json 汇总输入/输出 hash、已解析 runtime 字段和全部 32 个梯度采样差异。

两版本：particle life=200；rate=100/250，对应名义 20k/50k；owner life=1200；
total emit=2,000,000；seed=1806。方向 +Z，spread60°，speed=[0.035,0.10]，
damping=[0.01,0.03]；起始 uniform scale=[0.75,1.15]，结束=[0.15,0.35]，线性 easing；
shapeSize=.03，emissive1，fadeIn8/fadeOut35。运行时重力 Y=−.0006；作者值 Y=−.036，
官方 exporter 按 /60 转为 runtime。旋转与其余 force 默认值保持旧配方。
名义数量是 life×rate，不伪称 runtime 活粒子计数。

保留四个原作者色标及位置 0/.35/.72/1。官方 exporter 使用 float 转 byte 截断，
前 31 个采样对旧 Python round 配方最大差 1/255。官方 Gradient.Sample 在 t=1
返回零，末采样由旧 `[92,18,8,32]` 变为 `[0,0,0,0]`：这是明确接受的迁移差异，
不能说全梯度只差 1/255。全 128 通道有 50 个变化，最大 byte 差 92，平均绝对差
1.53125。新旧全部采样和逐通道差保存在 manifest；是否有可见尾段突变须新画面复核，
不能依据 fadeOut 推断通过。

## 固定工具与重建验证

生成器固定验证 Effekseer.exe、EffekseerCore.dll、上游 EFKE/纹理 SHA256。
许可文本仅将 checkout CRLF 规范为 LF 后校验既有批准 hash，不修改原许可文件。
两个工具 hash 分别为 `cc2ea1f64d86b203ee9924c72252421996a848d2416ebbf8432abe9e4f7e30c6`
和 `d8de02503e9e847b673bcf5201ebc8c977a033ea3689a089ca611d980f90b264`。

```powershell
py -3.11 Tools/VFX/generate_effekseer_stress_assets.py
py -3.11 Tools/VFX/generate_effekseer_stress_assets.py --check
py -3.11 Tools/VFX/generate_effekseer_stress_assets.py
```

已执行：官方编译、EDIT 回读、从回读 EDIT 再编译与首次 BIN 逐字节一致，全部 PASS。
第二次 --check PASS；第三次普通生成 changed=0，15 个生成输出的 hash/mtime 零扰动。
日志：`out/agent/vfx-authoring/stress-official-{generate,check,noop}-01.log`。
纯 CPU 负例验证：对两版本分别改 BIN version1800、CPU sampler331、rate999，6 项
全部拒绝；未运行引擎。validation-evidence.json 保存摘要。

公开校验 API：`validate_compiled_stress(full_efkefc_payload, nominal)`。
函数校验真实 BIN，使用唯一结构锚定位、顺序解析全部 GPU 参数、要求尾部恰为零子节点，
并校验 CPU common、seed/owner life。它是本固定 fixture 的有界校验器，不冒充通用
Effekseer parser；拒绝 1800。返回容量/速率/寿命/位置/hash/梯度差异，可供 lifecycle
driver 替换原 offset1003 手拼块断言。

## 新包 hash 与验收边界

| 包 | SHA256 |
| --- | --- |
| GpuSparks20k.efkpkg | `8c8f8bd723ff2196226e85f84179d0e0d918c2e9201b32d18217ffd12cec919a` |
| GpuSparks50k.efkpkg | `6eae4bf6f4219226893aa5ffcd65684f7b6832b5b7f3a409fe114b19dacf7b53` |

当前只有离线资源/参数/重建证据。DX12、Vulkan 的加载、绘制、暂停、回收和新外观
仍需 root 用固定候选实际执行；不得沿用旧 1800 包的画面或生命周期结论。
