# Shader 驱动材质

HEngine 的 Shader Material 由两部分组成：`Project/Shaders` 下的 Surface Material Definition（HLSL），以及 `Project/Assets` 下的 Material Instance（`.hmat`）。Shader 定义代码和参数布局；`.hmat` 选择 Shader、render state，并保存该实例自己的 typed 参数和纹理。

默认 PBR 材质仍使用 `Shaders/BasePBR.hlsl`。只有需要 Fresnel、程序化 UV 图案等代码行为时，才需要自定义 Surface Shader。

延迟渲染的 `Foliage/Subsurface.hlsl` 使用独立的双面叶片透射载荷。静态网格若提供有效光照图、UV1及非零 `StaticLightmapBlend`，引擎在 GBuffer 的 `StaticDiffuse` 目标中保存烘焙漫反射及环境漫反射替代权重，不覆盖透射颜色。该目标为全屏 RGBA16F，提升临时渲染资源占用；在不需要 foliage 静态 GI 的项目中仍按当前渲染路径分配，需结合帧时和预算选择画质配置。此路径尚不覆盖自定义透明 forward 材质，也不把导入的 UE 光照图视为逐像素画质等效的证据。

## 创建方式

1. 在 `Project/Shaders` 新建 `.hlsl`，include 引擎 Surface ABI。
2. 声明 `hmaterial`、`hblend` 和需要暴露的 property。
3. 实现且只实现 `EvaluateMaterial`；顶点路径、光照、阴影和 tone map 由引擎 wrapper 负责。
4. 在 Content Browser 创建 `.hmat`，把 Shader 拖入材质编辑器并保存。

最小 Lit 示例：

```hlsl
#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Lit
#pragma hblend Opaque

#pragma property g_BaseColor "Base Color" color default(0.8,0.2,0.1,1) group("Surface")
#pragma property g_RimColor "Rim Color" color default(1,1,1,1) group("Rim")
#pragma property g_RimPower "Rim Power" float default(4) range(0.25,16) group("Rim")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    surface.BaseColor = g_BaseColor.rgb;
    const float rim = pow(
        1.0 - saturate(dot(input.NormalWS, input.ViewWS)),
        g_RimPower);
    surface.Emissive += g_RimColor.rgb * rim;
}
```

不需要 `#pragma htype`、VS 或 PS。编译器自动生成 CPU static/skinned 和 baseline/heap-direct permutations。材质定义不能声明自己的 `register(...)`、root signature、`hvariant` 或 `hprofile`。

## Pragma

必需项：

- `#pragma hmaterial Surface Lit` 或 `Surface Unlit`
- `#pragma hblend Opaque|Mask|Transparent`

可选 render state：

- `#pragma hcull Back|None|DoubleSided`
- `#pragma hdepthwrite On|Off`
- `#pragma hshadow On|Off`

Property 语法：

```text
#pragma property g_Name "Display Name" type default(...) [range(min,max)]
                 [group("Group")] [id("stable.id")]
                 [semantic(...)] [colorspace(...)] [fallback(...)]
```

Property 名必须以 `g_` 开头；`g_HE` 是引擎保留前缀。`id(...)` 用于 Shader 改版后迁移实例值；省略时 stable ID 等于 property 名。

支持类型：

| 类型 | default 示例 | GPU row 中的有效数据 |
| --- | --- | --- |
| `float` | `default(0.5)` | x，4 bytes |
| `float2` | `default(0,0)` | xy，8 bytes |
| `float3` | `default(1,0,0)` | xyz，12 bytes |
| `color` | `default(1,1,1,1)` | xyzw，16 bytes |
| `bool` | `default(false)` | x 的 uint 值 |
| `int` | `default(2)` | x 的 signed int bits |
| `texture2d` | `default(white)` | x 为 descriptor index，y 为法线编码标志，z 为 UV0/UV1 编号 |

每个 property 固定独占一个 16-byte `float4` row。offset、size、binding signature 和名称宏都来自同一 compiler manifest；不要在 HLSL 中重复声明 cbuffer 或 Texture2D register。

当前 compiler manifest 的 `FormatVersion` 为 2；每个 property 必须记录声明位置 `Source`、`Line` 和 `Column`，用于稳定定位编译与 ABI 诊断。缺少这些字段的 v1 cache 不做隐式迁移，loader 会以单个不支持版本错误拒绝，需由当前编译器重新生成。

Texture2D 使用方式：

```hlsl
#pragma property g_SurfaceMap "Surface Map" texture2d default(white) semantic(BaseColor) colorspace(SRGB) fallback(white)

const float4 texel = g_SurfaceMap.Sample(
    HE_MATERIAL_DEFAULT_SAMPLER,
    g_SurfaceMap_UV);
```

编译器为每个纹理属性生成 `<property>_UV` 和 `<property>_UV_SET`。BaseColor、Normal、MetallicRoughness、Occlusion、Emissive 语义的 `<property>_UV` 从实例 `Sampling` 读取 UV0/UV1 选择及仿射变换；其他 data 纹理目前仅选择 UV set。自定义材质应使用前者采样，并将法线纹理的后者写入 `surface.NormalUvSet`。UV1 法线由像素位置和 UV1 导数重建切线基底；退化 UV 回退到网格切线。发片流向可用对应纹理的 UV set 构建基底。直接写 `input.UV0` 会忽略实例设置。wrap/filter 仍采用当前纹理的 GPU sampler，不能由此单独重写。

`HairCards.hlsl` 的遮罩覆盖率可分别调节 `g_OpacityMipBias`、`g_OpacityIntensity` 和 `g_OpacityThickness`，计算顺序是贴图 `SampleBias`、乘强度、饱和、指数、再乘底色 Alpha。默认 0/1/1 保持旧材质行为；较负的 mip 偏置会保留更多远景发丝细节，也可能增加闪烁，应结合目标项目的抗锯齿和动态镜头检查。

发片 GBuffer 的 `SurfaceId.y` 最高位标记 HairCards；TAA 历史采样允许同一对象的不同发片图元匹配，但仍校验前后视深和法线。当前邻域裁剪继续使用精确图元 ID，脸、眼、皮肤、普通遮罩材质及背景也保持原有精确历史规则。它缓解发片内部遮罩/重叠图元切换，不是完整的发丝级覆盖率抗锯齿；单像素显露与消失仍可能闪烁。

`Skin.hlsl` 可选 `g_SpecularMap`、`g_DetailMaskMap`、`g_SpecularBase` 和 `g_SpecularTiling`，以遮罩混合基础介电反射与缩放采样的细节图；关闭时 SpecularLevel 为 0.5，沿用既有 0.04 F0。Skin 模型的 GBuffer 编码、延迟直射/IBL、反射替换和前向路径共用每像素反射强度。该图控制高光，不改变口腔几何、粗糙度或贴图。

`Skin.hlsl` 还可选粗糙度重映射：贴图值乘以最小/最大粗糙度插值和倍率，再用视角 Fresnel 调整；关闭时沿用旧的 `g_RoughnessFactor × 贴图 G`。需要双面皮肤时选 `SkinTwoSided.hlsl`，它复用同一套 Skin 属性和表面求值，只把剔除改为 None；原单面定义保留给现有材质，材质实例的 `CullMode` 须与所选定义一致。

`BaseColor` 和 `Emissive` 纹理通常使用 `SRGB`；normal、metallic-roughness、AO 和普通 data map 使用 `Linear`。可用 fallback token 为 `white`、`black`、`normal`。序列化纹理路径必须是安全的 project/engine-content-relative 路径。

## Surface ABI

`HEMaterialInput` 提供：

- `UV0`
- `UV1`
- `PositionWS`
- `NormalWS`
- `TangentWS` / `BitangentWS`
- `ViewWS`
- `bFrontFace`
- `TimeSeconds`（单调递增秒数）

`HEMaterialSurface` 提供：

- `BaseColor`
- `Metallic`
- `PerceptualRoughness`
- `AO`
- `NormalTS`
- `Emissive`
- `Opacity`
- `AlphaCutoff`

Lit 统一经过方向光、点光、IBL 和级联阴影，并输出 linear HDR；Unlit 同样输出 linear HDR。最终曝光、tone mapping 和显示编码只在 renderer 的 tone-map pass 执行一次。

## Render state

- `Opaque`：不混合；可复用标准 depth-only 路径。
- `Mask`：GBuffer、Forward、Outline、Depth、Shadow 都调用同一个 `EvaluateMaterial`，并使用同一遮罩规则。实际裁剪阈值为 `max(AlphaCutoff, 0.01)`；1%下限会丢弃压缩/过滤后空白区域的近零Alpha，避免零阈值把整张树叶卡面写入颜色、深度及阴影。
- `Transparent`：只生成 Forward permutations，使用排序透明路径且 depth read/no write。
- `CullMode=None`/`DoubleSided` 会关闭背面剔除。

`.hmat` V2 的顶层 `BlendMode`、`CullMode`、`DepthWrite` 和 `AlphaCutoff` 应与 Shader pragma 一致。顶层 `AlphaCutoff` 是权威值；如果 typed `Parameters.g_AlphaCutoff.Value` 与它不一致，加载会明确报错，避免发片、树叶等遮罩材质静默使用旧阈值。`Parameters`/`Textures` 是 typed 实例值；`Properties` 是过渡期 legacy PBR dual-write，旧资源和固定 PBR 路径仍可读取。未知或已改型的参数进入 `OrphanedParameters`，不会在一次 Save 后静默丢失。

V2 材质声明非 `Shaders/BasePBR.hlsl` 的 `ShaderPath` 时必须设 `Type: "Custom"`；`Type: "Default"` 只渲染基础 PBR。现在加载器会拒绝这一矛盾配置，避免材质参数看似已保存却根本未进入自定义着色器。

精简的 V2 typed 值形状：

```json
{
  "SchemaVersion": 2,
  "Type": "Custom",
  "ShaderPath": "Shaders/FresnelRim.hlsl",
  "Domain": "Surface",
  "ShadingModel": "Lit",
  "BlendMode": "Opaque",
  "CullMode": "Back",
  "DepthWrite": true,
  "AlphaCutoff": 0.5,
  "Parameters": {
    "g_RimPower": {
      "StableId": "rim.power",
      "Type": "float",
      "Value": 4.0
    }
  },
  "Textures": {},
  "OrphanedParameters": {},
  "Properties": {}
}
```

## 热重载与错误处理

Editor 会监听 Project/Engine Shader roots：

- `.hlsl` 变化编译自身。
- `.hlsli` 变化编译所有依赖 Shader。
- 所有 permutations、DXIL ABI 和 manifest 均成功后，才原子发布新 manifest/program generation。
- 编译或 ABI 校验失败时不发布，当前材质继续使用 last-known-good Shader/Pipeline；完整 diagnostics 会保留在材质编辑器中。
- 普通参数或纹理变化只更新实例参数，不重新编译 Shader，也不创建新 Pipeline。

旧版 V2 `.hmat` 曾把纹理属性名本身写入 `StableId`。加载时只对 `StableId == 属性名` 的旧格式允许按名称迁移到当前 manifest 的稳定 ID，并保留纹理 `Sampling`；其他不同稳定 ID 仍作为孤立参数保存，避免错误继承同名新属性。

## 场景实例材质覆盖

`MeshComponent.MaterialOverrides` 可以按 `Mesh::mMaterial` slot 索引保存每个实例的 `.hmat` 路径。空字符串保留 `.hmesh` 作者材质；非空路径必须是安全的 content-relative `.hmat`。这适用于同一份几何的大量材质变体：几何只解码和上传一次，各实例仍保留独立材质参数。内建 5×5 材质展示场景就是用一份 `MaterialBall.hmesh` 加 25 个 slot 0 覆盖实现。

## 示例

- `FresnelRim.hlsl/.hmat`：视角相关 Fresnel emissive rim，固定 PBR 参数无法等价表达。
- `ProceduralChecker.hlsl/.hmat`：程序化 UV checker、纹理调制和一致的 Mask opacity。
- `Plastic`、`Gold`、`RoughCeramic`、`NormalMapped`、`Emissive`、`Masked`、`Transparent`：BasePBR V2 对照实例。

## 当前限制

- 仅支持 `Surface` domain；无 PostProcess Material。
- 无 vertex displacement/WPO、tessellation 或 Mesh Shader。
- ABI v1 的不透明自定义静态材质在活动 descriptor tier 具有完整 Forward/GBuffer GPU 变体时使用 GPU-driven；旧 manifest 缺少变体时保留 CPU 并记录重编译原因。Mask、Skinned 和 Transparent 保留各自 CPU 路径。
- 不允许每材质自定义 root signature 或 register space。
- 同一份 HLSL 由 DXC 生成 DX12/DXIL 与 Vulkan/SPIR-V；Surface manifest v3 按 backend 保存 ArtifactSets，逻辑材质 identity 不随 API 改变。
- Vulkan 当前使用每页 120 SRV 的 bounded descriptor paging；可见材质可跨任意数量页面，但单个材质不会被拆页。GPU-driven、CPU、Skinned、透明、Depth、Mask、Shadow 与 Outline pass 均绑定同一材质页。DX12 HeapDirect 是独立 capability，不会被错误映射到 Vulkan。
- 当前阶段不引入 SPIRV-Cross；SPIR-V 由 DXC 直接生成，并由离线 SPIRV-Tools contract 校验。
- Surface 材质支持 Forward HDR 和 Deferred GBuffer；最终曝光与 Tone Mapping 由统一后处理承担。

## 几何能力与辅助资源

`ResolveMaterialGeometryCapabilities` 根据已发布程序 ABI、coverage、蒙皮状态及活动 descriptor tier 的实际产物决定 GPU 实例能力。GPU 分组区分不可变程序对象；材质 PSO identity 继续包含源 hash、产物 hash、generation、参数布局、vertex path、双面与 alpha 状态。GPU 顶点通过实例 ID 读取既有对象行，像素阶段使用相同身份读取材质参数；描述符页与热更新发布机制不变。

| 材质 | 主绘制 | authored LOD | Depth/Shadow | Motion |
| --- | --- | --- | --- | --- |
| ABI v1 opaque static，完整 GPU 产物 | GPU indirect，Forward/GBuffer | 几何误差与滞回 | 相同几何，阴影保留 CPU | GPU GBuffer 当前/前帧变换与稳定 ID |
| ABI v1 opaque static，旧产物 | CPU，有缺失变体诊断 | 支持 | 保持原行为 | CPU GBuffer |
| Mask | CPU，保留 coverage | 保守 LOD0 | 相同 EvaluateMaterial opacity | CPU GBuffer |
| Skinned | CPU | 保守 LOD0 | 现有骨骼路径 | 当前/前帧骨骼 |
| Transparent | 排序 CPU | 保守 LOD0 | 保留原分类 | 不宣称透明时间重建支持 |

引擎拥有 ABI v1 顶点入口，因此不透明自定义材质不会因 Custom 标签失去静态几何 LOD；这不代表支持顶点位移。路径追踪仍遵循现有材质近似，不能把光栅程序化 surface 支持当成同等光追着色支持。

Forward 在 GI、SSR、局部 probes 均关闭且不透明列表没有自定义 surface 程序时，辅助绘制为 `RasterNormalMotionGuides`：只创建 Normal/Roughness/AO、Motion/PreviousDepth 和精确 SurfaceId 三个 MRT。保留深度、当前/前帧视深与 ID 拒绝条件。Deferred、GI、反射或没有专用 guide 变体的自定义材质使用完整 `RasterGBuffer`，避免以错误内建材质替代。需求变化会重建图。轻量路径少 12 bytes/pixel 名义纹理容量；这不是已测量 GPU 提速或实际显存预算收益。

编译器验证使用 `powershell -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HShaderCompiler`，随后运行 `out/agent/editor-debug/bin/Programs/HShaderCompiler/HShaderCompiler.exe --self-test`。`MaterialHotReloadContract` 补充检查旧产物回退、完整 GPU 变体、descriptor tier 隔离与骨骼保守分类。引擎构建、画面与性能结果以本次升级验收记录为准，编译器自测不代替运行画面验证。

DDS 首次可见卡顿可临时用进程环境变量 `HE_TEXTURE_UPLOAD_TIMINGS=1` 诊断。单张DDS耗时达到2ms时，运行日志记录读取/校验、GPU纹理创建、上传录制/提交及等待四段耗时、载荷字节数和资产路径；默认关闭。逐张日志会与磁盘读取竞争，**不能将开启诊断时的绝对帧时间当作正常运行性能**。`DdsTextureData::Load`对读入的文件缓冲复用所有权，避免校验后再复制完整压缩纹理；`Parse(span)`仍复制输入以保证返回值自持。

## TAA 后合成

透明、Primitive2D、描边和 Effekseer 保留 TAA 后合成。在启用 TAA 且存在这些消费者时，`PostTaaUnjitteredFrame` 发布非 jitter 帧常量，`PostTaaUnjitteredDepth` 重绘同一 LOD、蒙皮和 mask coverage 的单采样遮挡深度，然后后合成几何与 VFX 同时使用非 jitter 投影和对应深度。二次深度逐对象重放 GPU 实例，避免原 jitter/遮挡剔除列表漏掉新投影边界上的遮挡物；这是额外 CPU/GPU 成本，应在诊断中单独测量。没有消费者时不增加这些 Pass。

帧常量沿用 volatile buffer 的第二版本，图上的写依赖全部先前读取和 TAA/云颜色输出；下一帧正常上传恢复 raster 版本。云仍使用自己的非 jitter 光线、raster-depth 坐标映射和独立时间历史。该策略消除后合成几何直接继承帧 jitter 的来源，不声称新增透明 coverage、reactive mask 或透明 motion-vector 时间重建。

## VFX 颜色采样计划

Effekseer 公共纹理 loader 使用 `ColorSpaceType::Linear`，将 Color 类型纹理的 sRGB 编码解码到场景线性 HDR；Normal、Distortion 和其他数据纹理仍使用线性格式。原 renderer 便利 loader 固定使用 Gamma，不能直接满足 HDR 合成契约。保持最终 Tone Mapping、曝光和背景快照顺序不变。

运行对照应使用受控 Agent 会话脚本，所有 start 在 finally 中 stop：固定相机、1280×720、固定步长、曝光 1、关闭自动曝光/Bloom/GI/云，使用中灰 Color 纹理和白色顶点色的平面 unlit sprite。先预热 30 帧，再每步一帧采集 60 帧 HDR 与显示输出；DX12/Vulkan 各执行同一序列。在线性 HDR 的 sprite 内部 ROI，sRGB 0.5 纹理应接近 0.214 而非 0.5（排除边缘、透明背景混合）；显示输出按既有 ACES→sRGB 函数比较，不直接把截图值当线性值。另以中性 normal/distortion 数据纹理作对照，确认数据采样没有 sRGB 解码。

时间稳定性脚本应分别测试固定相机与平移相机，记录透明细边/移动粒子 ROI 的连续帧差异，并对照 `PostTaaUnjitteredDepth` Pass 时间、CPU 录制时间及资源池名义字节；在关闭所有后合成消费者的序列中确认两个 PostTaa Pass 不存在。上述是可重复采样方案，实际 PASS/FAIL 和数值仍须由验收记录提供。

## 薄叶片材质

`Shaders/Materials/Foliage/Subsurface.hlsl` 是可复用的双面 Mask 材质：BaseColor alpha 裁剪在颜色、深度和阴影路径一致；法线、粗糙度、AO、金属度仍走公共 PBR。`g_SubsurfaceColor` 与 `g_TransmissionStrength` 控制模型7的彩色背光透射，正面加少量包裹漫反射；Forward 与 Deferred 共用 `HE_CharacterLight`。视角项 `g_FuzzyCoreDarkness`、`g_FuzzyEdgeBrightness`、`g_FuzzyPower` 与 `g_FuzzyStrength` 复现 UE 内建 `FuzzyShading` 图中的 `1 - CoreDarkness*NdotV + EdgeBrightness*(1-NdotV)^Power`，Normal 输入未连接时使用双面几何法线。延迟路径在有静态光照图绑定时可叠加烘焙漫反射，但仍是薄片近似，不包含厚度图或体积雾；具体 UE 参数留在项目副本中，不能仅凭使用该 shader 判定 UE 视觉等效。UE 内建函数的参数语义与默认 Power=6 见 [Epic 文档](https://dev.epicgames.com/documentation/unreal-engine/shading-material-functions-in-unreal-engine)。

天空显示曝光：`EnvironmentLightComponent.SkyExposureCompensation`（EV，默认0，运行时限制到[-8,8]）仅在 `SceneSky.hlsl` 调整可见天空色，不改变同一HDR的 IBL 卷积、间接光、雾参数或地物曝光。该字段由反射序列化保存并进入 `RenderEnvironment`，适合在保持已校准间接光时匹配参考天空。设置值应与具体场景及参考画面核对。

演出镜头中预先知道会显示的DDS纹理可使用`SequenceComponent.PrefetchTextures`有界预读文件字节。`DdsTextureData::Load`消费已就绪字节后仍执行相同的DDS格式校验和GPU资源创建；该机制只减少同步磁盘读取，不预热材质PSO、上传或首次可见绘制，因此首次切镜头的帧时仍须单独测量。
