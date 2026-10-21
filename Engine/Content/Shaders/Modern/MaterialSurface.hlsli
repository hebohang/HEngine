#ifndef HE_MATERIAL_SURFACE_HLSLI
#define HE_MATERIAL_SURFACE_HLSLI

#include "MaterialSurfaceCommon.hlsli"
#include "RasterSurfaceData.hlsli"
#include "StaticLightmapDirectionalAtlas.hlsli"

// The user material definition supplies this function after including this file.
void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface);
#if HE_MATERIAL_HAS_VERTEX_OFFSET
// Optional world-space offset, evaluated at both current and previous time.
float3 EvaluateMaterialVertexOffset(
    float3 positionWS, float2 uv0, float4 color, float timeSeconds);
#endif

struct HEMaterialVertexInput
{
    float3 Position : POSITION;
    float3 PreviousPosition : PREVIOUS_POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Bitangent : BINORMAL;
#if HE_MATERIAL_SKINNED
    int4 BoneIds[4] : BONEIDS;
    float4 BoneWeights[4] : BONEWEIGHTS;
#endif
    float2 TexCoord1 : TEXCOORDONE;
    float4 Color : COLOR;
#if HE_MATERIAL_GPU_DRIVEN
    uint ObjectIndex : INPUT_OBJECT_ID;
#endif
};

struct HEMaterialVaryings
{
    float4 PositionCS : SV_Position;
    float3 PositionWS : POSITION0;
    float3 NormalWS : NORMAL0;
    float3 TangentWS : TANGENT0;
    float3 BitangentWS : BINORMAL0;
    float2 UV0 : TEXCOORD0;
    float2 UV1 : TEXCOORD6;
    float4 Color : COLOR0;
#if HE_MATERIAL_GPU_DRIVEN
    nointerpolation uint ObjectIndex : TEXCOORD4;
#endif
#if HE_MATERIAL_PASS_GBUFFER
    float4 CurrentClip : TEXCOORD1;
    float4 PreviousClip : TEXCOORD2;
    float PreviousViewDepth : TEXCOORD3;
#endif
};

// HLSL clip(0) keeps the pixel. Imported 8-bit alpha masks can also contain
// near-zero coverage in empty areas, which filtered mips spread across cards.
// Treat <1% coverage as transparent for every masked material pass.
void HE_ClipMaskedOpacity(float opacity, float cutoff)
{
    clip(opacity - max(cutoff, 0.01f) - 1e-6f);
}

HEMaterialVaryings HEMaterialVS(
    HEMaterialVertexInput input,
    uint instanceId : SV_InstanceID)
{
#if HE_MATERIAL_GPU_DRIVEN
    HE_LoadGpuObject(input.ObjectIndex);
#endif
    float4 positionLS = float4(input.Position, 1.0f);
#if HE_MATERIAL_PASS_GBUFFER
    float4 previousPositionLS = float4(input.PreviousPosition, 1.0f);
#endif
    float3 normalLS = input.Normal;
    float3 tangentLS = input.Tangent;
    float3 bitangentLS = input.Bitangent;
#if HE_MATERIAL_SKINNED
    float4 skinnedPosition = 0.0f;
#if HE_MATERIAL_PASS_GBUFFER
    float4 previousSkinnedPosition = 0.0f;
#endif
    float3 skinnedNormal = 0.0f;
    float3 skinnedTangent = 0.0f;
    float3 skinnedBitangent = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for (uint influence = 0; influence < 16; ++influence)
    {
        const int boneId = input.BoneIds[influence / 4][influence % 4];
        const float weight = input.BoneWeights[influence / 4][influence % 4];
        if (boneId < 0 || weight <= 0.0f)
            continue;
        const float4x4 bone =
            g_HEBoneMatrices[g_HEMaterialObjectMeshIndices.w + boneId];
        skinnedPosition += mul(bone, positionLS) * weight;
#if HE_MATERIAL_PASS_GBUFFER
        previousSkinnedPosition += mul(
            g_HEPreviousBoneMatrices[g_HEMaterialObjectMeshIndices.w + boneId],
            previousPositionLS) * weight;
#endif
        skinnedNormal += mul((float3x3)bone, normalLS) * weight;
        skinnedTangent += mul((float3x3)bone, tangentLS) * weight;
        skinnedBitangent += mul((float3x3)bone, bitangentLS) * weight;
        totalWeight += weight;
    }
    if (totalWeight > 0.0f)
    {
        positionLS = skinnedPosition / totalWeight;
#if HE_MATERIAL_PASS_GBUFFER
        previousPositionLS = previousSkinnedPosition / totalWeight;
#endif
        normalLS = skinnedNormal / totalWeight;
        tangentLS = skinnedTangent / totalWeight;
        bitangentLS = skinnedBitangent / totalWeight;
    }
#endif

    float4 positionWS = mul(g_HEWorld, positionLS);
#if HE_MATERIAL_HAS_VERTEX_OFFSET
    positionWS.xyz += EvaluateMaterialVertexOffset(
        positionWS.xyz, input.TexCoord, input.Color,
        g_HEMaterialTimeAndPadding.x);
#endif
    float4 rasterPositionWS = positionWS;
#if HE_MATERIAL_PASS_OUTLINE
    rasterPositionWS.xyz += HE_SafeNormalize(
        mul((float3x3)g_HEWorldInverseTranspose, normalLS)) * 0.025f;
#endif
    HEMaterialVaryings output;
#if HE_MATERIAL_GPU_DRIVEN
    output.ObjectIndex = input.ObjectIndex;
#endif
#if HE_MATERIAL_PASS_SHADOW
    output.PositionCS = mul(
        HE_GetShadowPassMatrix(g_HEShadowCascadeIndex), rasterPositionWS);
#else
    output.PositionCS = mul(g_HEViewProjection, rasterPositionWS);
#endif
#if HE_MATERIAL_PASS_GBUFFER
    output.CurrentClip = output.PositionCS;
    float4 previousPositionWS = mul(g_HEPreviousWorld, previousPositionLS);
#if HE_MATERIAL_HAS_VERTEX_OFFSET
    previousPositionWS.xyz += EvaluateMaterialVertexOffset(
        previousPositionWS.xyz, input.TexCoord, input.Color,
        g_HEMaterialTimeAndPadding.y);
#endif
    output.PreviousClip = mul(g_HEPreviousViewProjection, previousPositionWS);
    // Preserve signed depth through interpolation for triangles crossing the
    // camera plane. Taking abs at vertices corrupts the visible portion.
    output.PreviousViewDepth = -mul(g_HEPreviousView, previousPositionWS).z;
#endif
    output.PositionWS = positionWS.xyz;
    output.NormalWS = HE_SafeNormalize(
        mul((float3x3)g_HEWorldInverseTranspose, normalLS));
    output.TangentWS = HE_SafeNormalize(mul((float3x3)g_HEWorld, tangentLS));
    output.BitangentWS = HE_SafeNormalize(mul((float3x3)g_HEWorld, bitangentLS));
    output.UV0 = input.TexCoord;
    output.UV1 = input.TexCoord1;
    output.Color = input.Color;
    return output;
}

HEMaterialInput HE_BuildMaterialInput(
    HEMaterialVaryings input,
    bool bIsFrontFace)
{
#if HE_MATERIAL_GPU_DRIVEN
    HE_LoadGpuObject(input.ObjectIndex);
#endif
    HEMaterialInput materialInput;
    materialInput.UV0 = input.UV0;
    materialInput.UV1 = input.UV1;
    materialInput.Color = input.Color;
    materialInput.PositionWS = input.PositionWS;
    materialInput.NormalWS = input.NormalWS;
    materialInput.TangentWS = input.TangentWS;
    materialInput.BitangentWS = input.BitangentWS;
    materialInput.ViewWS = HE_SafeNormalize(
        g_HECameraPositionAndExposure.xyz - input.PositionWS);
    materialInput.bFrontFace = bIsFrontFace;
    materialInput.TimeSeconds = g_HEMaterialTimeAndPadding.x;
    return materialInput;
}

#if HE_MATERIAL_PASS_GBUFFER
HERasterSurfaceOutputs HEMaterialPS(
    HEMaterialVaryings input,
    bool bIsFrontFace : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID)
{
    const HEMaterialInput materialInput = HE_BuildMaterialInput(input, bIsFrontFace);
    HEMaterialSurface surface = HE_DefaultMaterialSurface();
    EvaluateMaterial(materialInput, surface);
    surface.Opacity = HE_SanitizeUnit(surface.Opacity, 1.0f);
    surface.AlphaCutoff = HE_SanitizeUnit(surface.AlphaCutoff, 0.5f);
#if HE_MATERIAL_BLEND_MASK
    HE_ClipMaskedOpacity(surface.Opacity, surface.AlphaCutoff);
#endif
    const float3 normalWS = HE_ResolveMaterialNormal(materialInput, surface.NormalTS,
        surface.NormalUvSet);
    const float roughness = isfinite(surface.PerceptualRoughness)
        ? clamp(surface.PerceptualRoughness, 0.045f, 1.0f) : 1.0f;
    float filteredRoughness = HE_ApplySpecularAntiAliasing(roughness, normalWS, surface.NormalMipVariance);
    if (surface.CharacterModel == 3u)
        filteredRoughness = HE_ApplySpecularAntiAliasing(filteredRoughness,
            HE_SafeNormalize(surface.StrandDirectionWS));
    HERasterSurfaceOutputs output = HE_EncodeRasterSurface(surface.BaseColor,
        HE_SanitizeUnit(surface.Metallic, 0.0f), normalWS,
        filteredRoughness,
        HE_SanitizeUnit(surface.AO, 1.0f), HE_SanitizeHdr(surface.Emissive),
        HE_MATERIAL_SHADING_UNLIT != 0, input.CurrentClip, input.PreviousClip,
        input.PreviousViewDepth, g_HEMaterialObjectMeshIndices.y,
        g_HEMaterialObjectMeshIndices.z + primitiveId);
    // Reserve the high primitive-ID bit for hair-card history matching. TAA
    // may reuse another card of this same skinned object only after its depth
    // and normal checks pass. Other consumers still receive exact primitive IDs.
    if (surface.CharacterModel == 3u)
        output.SurfaceId.y |= 0x80000000u;
#if !HE_RASTER_GUIDES_ONLY
    // Negative model codes never alias the 0=Lit / 1=Unlit classification.
    // Existing MRT formats remain unchanged.
    if ((surface.CharacterModel >= 2u && surface.CharacterModel <= 4u)
        || surface.CharacterModel == 7u)
    {
        const float3 payload = surface.CharacterModel == 3u
            ? float3(HE_EncodeOctahedralNormal(HE_SafeNormalize(surface.StrandDirectionWS)),
                saturate(surface.CharacterStrength))
            : saturate(surface.CharacterTint) * saturate(surface.CharacterStrength);
        output.EmissiveModel = float4(-payload, -(float)surface.CharacterModel);
        if (surface.CharacterModel == 2u)
            output.EmissiveModel.w -= 0.5f * HE_SanitizeUnit(surface.SpecularLevel, 0.5f);
    }
    else if (surface.CharacterModel == 6u)
    {
        // The emissive-model alpha is floating point. Its negative fractional
        // part carries dielectric specular for this eye model without changing
        // the GBuffer layout or discarding the eye's RGB emission.
        output.EmissiveModel.w = -6.0f - 0.5f * HE_SanitizeUnit(surface.SpecularLevel, 0.5f);
    }
#if HE_HEAP_DIRECT
    if (g_HEStaticLightmapDescriptor.x != 0xffffffffu &&
        (surface.CharacterModel == 0u || surface.CharacterModel == 7u) &&
        HE_MATERIAL_SHADING_UNLIT == 0)
    {
        Texture2D<float4> lightmapTexture =
            HE_MATERIAL_TEXTURE(g_HEStaticLightmapDescriptor.x);
        const HEGpuMaterialData material =
            g_HEGpuMaterials[g_HEMaterialObjectMeshIndices.x];
        SamplerState lightmapSampler = HE_MATERIAL_SAMPLER(
            material.EmissiveSamplerFlags.y);
        const float3 irradiance = HE_DecodeDirectionalAtlasLightmap(
            lightmapTexture, lightmapSampler, input.UV1, normalWS,
            g_HEStaticLightmapUvScaleBias, g_HEStaticLightmapScale0,
            g_HEStaticLightmapAdd0, g_HEStaticLightmapScale1,
            g_HEStaticLightmapAdd1, g_HEStaticLightmapDescriptor.z);
        const float staticLightmapBlend =
            g_HEStaticLightmapDescriptor.y / 65535.0f;
        const float3 bakedDiffuse = irradiance * surface.BaseColor *
            (1.0f - HE_SanitizeUnit(surface.Metallic, 0.0f)) / 3.14159265f *
            staticLightmapBlend;
        if (surface.CharacterModel == 7u)
            output.StaticDiffuse = float4(HE_SanitizeHdr(bakedDiffuse), staticLightmapBlend);
        else
            output.EmissiveModel = float4(HE_SanitizeHdr(surface.Emissive) +
                bakedDiffuse, 2.0f + staticLightmapBlend * 0.25f);
    }
#endif
#endif
    return output;
}
#elif HE_MATERIAL_PASS_FORWARD
float4 HEMaterialPS(
    HEMaterialVaryings input,
    bool bIsFrontFace : SV_IsFrontFace) : SV_Target0
{
    const HEMaterialInput materialInput =
        HE_BuildMaterialInput(input, bIsFrontFace);
    HEMaterialSurface surface = HE_DefaultMaterialSurface();
    EvaluateMaterial(materialInput, surface);
    surface.Opacity = HE_SanitizeUnit(surface.Opacity, 1.0f);
    surface.AlphaCutoff = HE_SanitizeUnit(surface.AlphaCutoff, 0.5f);
#if HE_MATERIAL_BLEND_MASK
    HE_ClipMaskedOpacity(surface.Opacity, surface.AlphaCutoff);
#endif
    const float outputAlpha = HE_MATERIAL_BLEND_TRANSPARENT
        ? surface.Opacity
        : 1.0f;
    // SceneRenderer's tone-map pass owns exposure and display encoding.
    float3 radiance = HE_ShadeMaterial(materialInput, surface);
#if HE_MATERIAL_BLEND_TRANSPARENT
    // A thin cornea already carries its reflected energy through GGX Fresnel.
    // Straight-alpha blending must not attenuate that reflection a second time.
    if (surface.CharacterModel == 5u)
        radiance /= max(outputAlpha, 1e-6f);
#endif
    return float4(radiance, outputAlpha);
}
#elif HE_MATERIAL_PASS_OUTLINE
float4 HEMaterialPS(
    HEMaterialVaryings input,
    bool bIsFrontFace : SV_IsFrontFace) : SV_Target0
{
    const HEMaterialInput materialInput =
        HE_BuildMaterialInput(input, bIsFrontFace);
    HEMaterialSurface surface = HE_DefaultMaterialSurface();
    EvaluateMaterial(materialInput, surface);
    surface.Opacity = HE_SanitizeUnit(surface.Opacity, 1.0f);
    surface.AlphaCutoff = HE_SanitizeUnit(surface.AlphaCutoff, 0.5f);
    HE_ClipMaskedOpacity(surface.Opacity, surface.AlphaCutoff);
    return float4(1.0f, 0.22f, 0.02f, 1.0f);
}
#else
void HEMaterialPS(
    HEMaterialVaryings input,
    bool bIsFrontFace : SV_IsFrontFace)
{
    const HEMaterialInput materialInput =
        HE_BuildMaterialInput(input, bIsFrontFace);
    HEMaterialSurface surface = HE_DefaultMaterialSurface();
    EvaluateMaterial(materialInput, surface);
    surface.Opacity = HE_SanitizeUnit(surface.Opacity, 1.0f);
    surface.AlphaCutoff = HE_SanitizeUnit(surface.AlphaCutoff, 0.5f);
    // Mask Forward, Depth, and Shadow all execute the same material opacity path.
    HE_ClipMaskedOpacity(surface.Opacity, surface.AlphaCutoff);
}
#endif

#endif
