# Shader 驱动材质

HEngine 的 Shader Material 由两部分组成：`Project/Shaders` 下的 Surface Material Definition（HLSL），以及 `Project/Assets` 下的 Material Instance（`.hmat`）。Shader 定义代码和参数布局；`.hmat` 选择 Shader、render state，并保存该实例自己的 typed 参数和纹理。

默认 PBR 材质仍使用 `Shaders/BasePBR.hlsl`。只有需要 Fresnel、程序化 UV 图案等代码行为时，才需要自定义 Surface Shader。

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
| `texture2d` | `default(white)` | x 的 descriptor index |

每个 property 固定独占一个 16-byte `float4` row。offset、size、binding signature 和名称宏都来自同一 compiler manifest；不要在 HLSL 中重复声明 cbuffer 或 Texture2D register。

当前 compiler manifest 的 `FormatVersion` 为 2；每个 property 必须记录声明位置 `Source`、`Line` 和 `Column`，用于稳定定位编译与 ABI 诊断。缺少这些字段的 v1 cache 不做隐式迁移，loader 会以单个不支持版本错误拒绝，需由当前编译器重新生成。

Texture2D 使用方式：

```hlsl
#pragma property g_SurfaceMap "Surface Map" texture2d default(white) semantic(BaseColor) colorspace(SRGB) fallback(white)

const float4 texel = g_SurfaceMap.Sample(
    HE_MATERIAL_DEFAULT_SAMPLER,
    input.UV0);
```

`BaseColor` 和 `Emissive` 纹理通常使用 `SRGB`；normal、metallic-roughness、AO 和普通 data map 使用 `Linear`。可用 fallback token 为 `white`、`black`、`normal`。序列化纹理路径必须是安全的 project/engine-content-relative 路径。

## Surface ABI

`HEMaterialInput` 提供：

- `UV0`
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
- `Mask`：Forward、Depth、Shadow 都调用同一个 `EvaluateMaterial`，并使用同一 `Opacity - AlphaCutoff` 规则。
- `Transparent`：只生成 Forward permutations，使用排序透明路径且 depth read/no write。
- `CullMode=None`/`DoubleSided` 会关闭背面剔除。

`.hmat` V2 的顶层 `BlendMode`、`CullMode`、`DepthWrite` 和 `AlphaCutoff` 应与 Shader pragma 一致。`Parameters`/`Textures` 是 typed 实例值；`Properties` 是过渡期 legacy PBR dual-write，旧资源和固定 PBR 路径仍可读取。未知或已改型的参数进入 `OrphanedParameters`，不会在一次 Save 后静默丢失。

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

## 场景实例材质覆盖

`MeshComponent.MaterialOverrides` 可以按 `Mesh::mMaterial` slot 索引保存每个实例的 `.hmat` 路径。空字符串保留 `.hmesh` 作者材质；非空路径必须是安全的 content-relative `.hmat`。这适用于同一份几何的大量材质变体：几何只解码和上传一次，各实例仍保留独立材质参数。内建 5×5 材质展示场景就是用一份 `MaterialBall.hmesh` 加 25 个 slot 0 覆盖实现。

## 示例

- `Engine/Content/Assets/Materials/BuiltInShowcase/`：25 个内建材质，覆盖 Fresnel、程序图案、Mask、透明、全息与常见 PBR 表面。
- `Engine/Content/Shaders/Materials/BuiltInShowcase/`：上述材质对应的可复用 Surface Shader。
- `Project/Assets/Material/Plastic.hmat`、`Gold.hmat`、`RoughCeramic.hmat`：默认展示项目保留的 BasePBR V2 对照实例。
- `Project/Shaders/` 与同名 `.hmat`：Fresnel、程序棋盘和透明脉冲的项目级 Shader/材质示例；`PropertyShowcase.hlsl` 展示属性声明。

## 当前限制

- 仅支持 `Surface` domain；无 PostProcess Material。
- 无 vertex displacement/WPO、tessellation 或 Mesh Shader。
- 自定义 Surface Material 当前明确回退 CPU static/skinned/transparent 路径；默认 PBR 继续使用 GPU-driven 路径。
- 不允许每材质自定义 root signature 或 register space。
- 同一份 HLSL 由 DXC 生成 DX12/DXIL 与 Vulkan/SPIR-V；Surface manifest v3 按 backend 保存 ArtifactSets，逻辑材质 identity 不随 API 改变。
- Vulkan 当前使用每页 120 SRV 的 bounded descriptor paging；可见材质可跨任意数量页面，但单个材质不会被拆页。GPU-driven、CPU、Skinned、透明、Depth、Mask、Shadow 与 Outline pass 均绑定同一材质页。DX12 HeapDirect 是独立 capability，不会被错误映射到 Vulkan。
- 当前阶段不引入 SPIRV-Cross；SPIR-V 由 DXC 直接生成，并由离线 SPIRV-Tools contract 校验。
- 本系统保持 forward HDR，不引入 deferred/GBuffer。
