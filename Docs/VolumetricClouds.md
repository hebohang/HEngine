# Starbloom Survivor 体积云

实现位于 `SceneCloudPass`，场景数据来自 `EnvironmentLightComponent` 的正式反射字段，经 `RenderWorldExtractor` 规范化后进入不可变 `RenderWorld.Clouds`。渲染器不查询 ECS。游戏使用 `MainGame.he` 的环境实体，生成源为 `Project/Tools/GenerateVerdantRift.py`；不要为了修改云重新生成整个场景，生成源尚有原有 Exposure / ExposureEV100 差异。

## 使用与调参

正常启动进入菜单，点击“开始冒险”。默认第三人称俯角从 -30° 调整为 -20°，保留地面战斗空间；按住右键向上转动可看到更大天空。菜单环境没有启用云。游戏仍使用 `Assets/Textures/Hdr/VerdantRift.hdr` 和原有 IBL、曝光、GI 设置；HDR 是渐变与太阳，没有烘焙云。

场景 Inspector 的 Environment Light 包含 Clouds Enabled、Bottom、Thickness、MaxDistance、Coverage、Density、Scale、Roundness、Erosion、Seed、WindAngle、WindSpeed、EvolutionSpeed、LitColor、ShadowColor、Ambient、SilverLining。沿现有场景保存流程持久化。旧场景缺少字段时关闭云。非有限美术值回到默认值，其余按字段范围夹紧；画质非法值报错，经设置事务回滚。

晴天预设：底高 90、厚度 95、距离 5000、覆盖 .42、消光密度 .035、尺度 260、圆润度 .75、侵蚀 .12、种子 7301；风向 25°、风速 1.8、形变 .015；受光色 (1,.97,.9)、阴影色 (.56,.66,.87)、补光 .65、边缘透光 .3。主光使用快照中第一盏方向光，缺少时用固定上方方向及白光回退。降低侵蚀可保留大轮廓，提高覆盖会连成云层，应避免遮满蓝天。

项目设置 → 渲染 → 体积云，提供中文标签、帮助、页筛选、保存与重置，复用 RenderSettingsService 的 Engine / Project / User / Session 继承、revision/hash、requested/effective、失败回滚和最后有效配置。引擎默认 Off，项目默认 Balanced；场景开关还必须为真。预设控制内部比例和采样，选择 Custom 后才可编辑这三项。

| 档位 | 宽、高各缩放 | 总像素比例 | 主步数 | 每个非空点最多光照步数 | 1080p 纹理 payload |
|---|---:|---:|---:|---:|---:|
| Off | — | 0 | 0 | 0 | 0 |
| Low | .25 | 1/16 | 24 | 1 | 6.18 MiB |
| Balanced | .5 | 1/4 | 40 | 3 | 23.98 MiB |
| High | .5 | 1/4 | 80 | 5 | 23.98 MiB |
| Custom | .25–1 | 1/16–1 | 16–96 | 0–6 | 最大约 95.17 MiB |

其他稳定 ID：`Rendering.Clouds.HistoryWeight`（默认 .85，上限 .95）、`DetailDistance`（1800）、`Temporal`（true）、`DebugView`（0 正常、1 不透明度、2 散射、3 内部网格）。云内部降分辨率不改变场景、游戏 UI 或其他效果尺寸。Custom 全分辨率超出初始 64 MiB 目标。

## 渲染与资源

`CloudRayMarch` → `CloudTemporalResolve` → `CloudUpsampleComposite` 位于 Scene TAA 之后、Primitive2D / TransparentForwardHDR 之前；MSAA HDR resolve 和 VFX 在后，最后后处理、ToneMapping、FXAA/SMAA、UI。因此 FXAA 不影响云自身历史，云不被整屏 TAA 重复累计。

模块一次生成确定性的可平铺 64³ R8 Texture3D，通过公共 NVRHI 上传，运行不依赖文件、下载或 Agent。天气分布、高度剖面和低频三维形状构成密度场；远处淡出细节。射线支持透视/正交，先与云层相交，再由正常 Z 深度裁剪；深度 1 为无限天空，不使用 far=400 限制云。非抖动射线映射到实际 raster 深度坐标。指数消光按实际步长积分，透射过低提前退出，空密度跳过二次光照。当前采用固定预算等距主步进，没有宣称具备自适应粗细步进或带 mip 的噪声。

内部保存 L、T 和代表云距离/几何距离。空云 L=0,T=1，HDR 合成为 L+T*C。历史用有限深度位置、上一提交相机与风位移重投影，做邻域约束、深度/透明度拒绝与运动降权。上采样按几何深度和云代表深度拒绝漏云；MSAA 合成使用 Texture2DMS + SV_SampleIndex 读取真实样本深度，并继续写多采样 HDR，无提前 resolve。

每个 SceneRenderer 独立历史与缓存。上一矩阵、风位置、ping-pong 和采样序列只在成功提交后推进；Abort 不推进。场景 generation、投影、camera cut、艺术参数、质量与尺寸改变使历史失效。材质 shader 发布也重置云历史。风仅更新常量，不重建图或噪声。资源与绑定集按实际 submission 退休，Collect 只释放 GPU 已完成资源。关闭云不添加云 Pass；同尺寸画质变更只更新常量并重置历史，比例/开关变化走现有可回滚图事务。

动画采用 RenderWorld 的 presentation time，当前明确在玩法暂停时继续缓慢漂移；自动化可固定步长。动态云或尚未收敛的历史要求连续渲染。风和形变都设为 0 时，历史收敛后可复用静态视图。

## 实测与复现

2026-09-10 本机：i7-11800H、RTX 3060 Laptop 6 GiB、驱动 591.74，Windows，VS2026 v145。DX12 与 Vulkan 严格选择都运行成功。以下性能矩阵使用 DX12、Deferred、FXAA、1920×1080；记录显示 main scene graph（sceneOffscreen=false），云计时确在该图，不是仅计 presentation graph。

原始证据：`out/agent/rendering-evidence/cloud-matrix-20260910-033928/`。每组固定 run seed 7319、云 seed 7301、120Hz 模拟、120 帧预热、780 帧总计；真实 MainGame 敌人波次继续运行。每组得到 656 个预热后去重 GPU 样本。场景和设置输入、SHA256、逐帧 CSV、逐 Pass CSV、进程内存 CSV、截图和 world metadata 均在各子目录。数据按 gpuSample 关联，GPU 返回延迟不与同一行 CPU 时间混同。

| 镜头 | Low 平均/P95 | Balanced 平均/P95/P99/最大 | High 平均/P95 |
|---|---:|---:|---:|
| 默认战斗 | .143/.150 | .292/.309/.315/.329 | .463/.484 |
| 大面积天空 | .230/.240 | .718/.970/1.341/1.457 | 1.092/1.484 |
| 密集云 | .231/.238 | .780/.807/1.222/1.262 | 1.310/1.690 |
| 复杂轮廓 | .168/.173 | .461/.481/.486/.498 | .808/.838 |

单位 ms，包含三个云 Pass 和稳态空操作 NoiseUpload。Balanced 各组超过 1.5 ms、2 ms 的样本比例均为 0。推荐 Balanced 的 GPU 预算，但最终美术与运动验收尚未完整通过，不能据此宣布全部交付完成。所有档位/各 Pass 的平均、P95、P99、最大、超限率以及整图、整帧指标见 `summary.json`。

Balanced 的 RayMarch / Temporal / Composite 平均分别为：默认 .165/.049/.079；天空 .583/.051/.084；密集 .645/.052/.084；轮廓 .330/.051/.081 ms。整图 Pass sum 平均为 Off→Balanced：默认 2.953→3.058、天空 3.061→3.093、密集 2.952→3.141、轮廓 3.083→3.120。对应 GPU frame span 3.270→3.369、3.453→3.439、3.327→3.510、3.414→3.462。笔记本动态频率和跨次运行波动使整图差值不等于云 Pass 总和，不能用它反推云成本或声称稳定 60 FPS。

纹理 payload 口径为当前 LT/guide + 两套历史 LT/guide，合计每内部像素 48 字节，加 64³ R8；不含 GPU 分配对齐、RenderGraph 池缓存、PSO/描述符及同时存活的退休资源。Balanced 进程 local usage 采样峰值约 939 MiB，是整个进程，不是云。首次启用/resize/档位变更的精确显存峰值尚未单独测定，不能写成 0。矩阵初版没有云专属 CPU 计时；后续 `cpuCloudRecordMs` 计量云 prepare/import/构图/NVRHI 录制作用域，排除帧槽等待、队列提交、Present，不包含通用图调度，亦不是整个游戏 CPU 帧时间。

最终 Balanced 重复采集见 `out/agent/rendering-evidence/cloud-cpu-final-20260910/<scenario>/analysis.json`：各组 656 GPU / 660 CPU 样本，CPU 为云作用域经过时间，可能包含 OS 调度，不是 CPU 周期数。

| 镜头 | CPU 平均/P95/P99/最大 ms | 重复 GPU 平均/P95/P99/最大 ms |
|---|---:|---:|
| 默认 | .0165/.0410/.0474/.0669 | .296/.308/.313/.321 |
| 天空 | .0169/.0412/.0484/.0520 | .700/1.104/1.391/1.485 |
| 密集 | .0167/.0410/.0490/.0571 | .751/1.148/1.377/1.448 |
| 轮廓 | .0162/.0389/.0490/.0766 | .449/.467/.471/.488 |

重复采集 CPU 超过 .2 ms、云 GPU 超过 1.5/2 ms 比例均为 0。两次采集的尾部波动均保留，不挑选较低一组替代。

可复现命令（PowerShell 7，仓库根目录；独立 out/agent 产物）：

```powershell
& .\Tools\Agent\hectl.ps1 test RenderSettings
& .\Tools\Agent\hectl.ps1 game check
& .\Tools\Agent\hectl.ps1 editor check
& .\Tools\Agent\Validate-Clouds.ps1 -GraphicsApi DX12
& .\Tools\Agent\Validate-Clouds.ps1 -GraphicsApi Vulkan
& .\Tools\Agent\Validate-Clouds.ps1 -GraphicsApi DX12 -FailureProbe
& .\Tools\Agent\Benchmark-Clouds.ps1 -Scenario sky -Quality Balanced
& E:/Python/Python311/python.exe Tools/Agent/Analyze-Clouds.py <benchmark-output-directory>
& E:/Python/Python311/python.exe Project/Tools/GenerateVerdantRift.py --check
```

Benchmark 的 Scenario 支持 default/sky/dense/contour，Quality 支持 Off/Low/Balanced/High。脚本临时更改实际 MainGame 镜头和覆盖，finally 原字节恢复；不要同时运行或在运行时编辑该场景。流程脚本使用本次自建会话，在 finally 停止，不接管用户会话。存储使用现有 Storage.ps1 外层登记和回收。

已完成运行证据：DX12 `cloud-flow-20260910-034904-741`、Vulkan `cloud-flow-20260910-035119-576`。真实菜单启动、横移、右键转向、AA 切换、反复云档位切换、641×359 独立视图、Forward MSAA4、1279×719 resize、暂停/恢复、重新开局、返回菜单均有 transcript。此前输入消费时机错误的 `cloud-flow-20260910-032829-408` 为失败记录，不作为验收通过。

补充事务证据：`cloud-flow-20260910-035755-378` 使用 `HE_DEV_RENDER_FAIL_CANDIDATE_ONCE` 在云关闭候选图已分配后注入一次失败；请求返回 transaction_failed、rolledBack=true，hash/revision 和原 Balanced 设置保持不变，随后重试成功，完整流程正常退出。该目录的 `motion-review.html` 可逐帧播放实际相机转动捕获，播放速度是检视速度，不是原始游戏帧率。

反射输出使用正式 Codegen Run，`out/agent/logs/cloud-codegen-verification.json` 记录 249 个 manifest 输出 changed=0、deleted=0、hash/mtime 改动=0；没有手改 `_generated`。`RenderSettings` 逻辑测试覆盖预设、范围、反射场景往返、旧数据默认、JSON 设置往返、配置影响分类及事务失败恢复。生成器 `--check` 只在内存中构造验证，不覆盖现有场景。

## 当前验收边界

运行/事务成功与视觉成功分开记录。受本轮 AGENTS.md 最多检查两张新图限制，已检查图像用于发现初始天空不可见和自动化输入问题；修复后的序列尚未完整人工检查。不能声称已经证明无拖影、闪烁、毛发/树叶白边，也未完成逆光、地图边缘、高地、云层内及长期风动画的完整视觉验收。Inspector/设置页面编译接入不等于已验证鼠标编辑保存重开全过程。

透明对象和 VFX 在云后绘制，当前内容要求主要战斗透明物体位于云前；不支持穿云透明深度排序。没有云对地阴影、动态 IBL、行星大气或飞行穿云优化。云 shader 使用正常 DXIL/SPIR-V artifact 构建缓存；直接编辑云 shader 后需重建/重新创建 renderer，不提供云 shader 专用实时 PSO 替换。未执行正式 package，也未从包外目录运行，不能把开发 Game 成功写成 Shipping 通过。
