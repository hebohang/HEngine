#include "Include/Common.hlsli"
#include "Include/PBR/BRDF.hlsli"
#include "Include/PBR/IBL.hlsli"

#pragma htype vs VS
#pragma htype ps PS

namespace Properties
{
#pragma property g_AlbedoMap         "Albedo"             texture  default(white)   group("Surface")
#pragma property g_NormalMap         "Normal Map"         texture  default(normal)  group("Normals")
#pragma property g_MetallicRoughness "Metallic Roughness" texture  default(white)   group("Surface")
#pragma property g_AoMap             "Ambient Occlusion"  texture  default(white)   group("Surface")

#pragma property metallic            "Metallic"           float    default(1.0)     range(0.0, 1.0) group("Surface")
#pragma property roughness           "Roughness"          float    default(1.0)     range(0.04, 1.0) group("Surface")
#pragma property normalStrength      "Normal Strength"    float    default(1.0)     range(0.0, 2.0) group("Normals")
#pragma property baseColorTint       "Base Color"         color    default(1,1,1,1) group("Surface")

    Texture2D g_AlbedoMap         : register(t0, space1);
    Texture2D g_NormalMap         : register(t1, space1);
    Texture2D g_MetallicRoughness : register(t2, space1);
    Texture2D g_AoMap             : register(t3, space1);

    cbuffer MaterialCBuffer : register(b3, space1)
    {
        float4 baseColorTint;
        float metallic;
        float roughness;
        float normalStrength;
        float _MaterialPad;
    }
}

SurfaceVaryings VS(VertexIn input, uint instanceId : SV_InstanceID)
{
    return BuildSurfaceVaryings(input.pos, input.normal, input.tangent, input.bitangent, input.texcoord, instanceId);
}

float4 PS(SurfaceVaryings input) : SV_Target
{
    PbrSurfaceData surface;
    const float4 albedoSample = Properties::g_AlbedoMap.Sample(g_LinearWrapSampler, input.TexCoord);
    surface.BaseColor = SRGBToLinear(albedoSample.rgb) * baseColorTint.rgb;
    surface.Alpha = albedoSample.a * baseColorTint.a;

    const float4 metallicRoughnessSample = Properties::g_MetallicRoughness.Sample(g_LinearWrapSampler, input.TexCoord);
    surface.Metallic = saturate(metallicRoughnessSample.b * metallic);
    surface.Roughness = clamp(metallicRoughnessSample.g * roughness, 0.04f, 1.0f);

    const float ao = Properties::g_AoMap.Sample(g_LinearWrapSampler, input.TexCoord).r;
    surface.AmbientOcclusion = ao;
    surface.Emissive = float3(0.0f, 0.0f, 0.0f);

    const float3 normalWS = SampleNormalTexture(
        Properties::g_NormalMap,
        input.TexCoord,
        input.NormalWS,
        input.TangentWS,
        input.BitangentWS,
        normalStrength);
    surface.Roughness = ApplySpecularAntiAliasing(surface.Roughness, normalWS);

    const float3 viewWS = SafeNormalize(g_CameraPos - input.PositionWS);
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), surface.BaseColor, surface.Metallic);

    float3 lighting = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        if (g_DirLight[i].intensity > 0.0f)
        {
            const float3 lightWS = SafeNormalize(-g_DirLight[i].direction);
            const float3 radiance = g_DirLight[i].color * g_DirLight[i].intensity;
            lighting += EvaluateDirectBRDF(surface, normalWS, viewWS, lightWS, radiance, F0);
        }
    }

    lighting += EvaluateImageBasedLighting(surface, normalWS, SafeNormalize(input.NormalWS), viewWS, F0);

    // Keep material output linear; the graph-owned tone-mapping pass owns
    // exposure and display encoding.
    return float4(lighting, surface.Alpha);
}
