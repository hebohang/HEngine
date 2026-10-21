#ifndef HE_RASTER_SURFACE_LIGHTING_HLSLI
#define HE_RASTER_SURFACE_LIGHTING_HLSLI

#include "SceneLightGridCommon.hlsli"
#include "SceneLocalShadowSampling.hlsli"
#include "EnvironmentSpecular.hlsli"
#include "Shared/NormalMapDecode.hlsli"
#include "Shared/SurfaceLightingMath.hlsli"

Texture2D<float> g_ScreenSpaceAo : register(t2, space0);

float3 HE_ApplyDistanceHeightFog(float3 radiance, float3 positionWS)
{
    if (g_FogColorDensity.w <= 0.0f && g_SecondaryFogColorDensity.w <= 0.0f)
        return radiance;
    const float3 cameraWS = g_CameraPositionAndExposure.xyz;
    const float rayDistance = length(positionWS - cameraWS);
    float3 primaryFogColor = g_FogColorDensity.rgb;
    if (g_FogColorDensity.w > 0.0f && g_FogDirectionalColorIntensity.w > 0.0f &&
        g_DirectionalDirectionAndIntensity.w > 0.0f && rayDistance > 1e-5f)
    {
        const float3 viewRay = (positionWS - cameraWS) / rayDistance;
        const float3 sunDirection = -normalize(g_DirectionalDirectionAndIntensity.xyz);
        const float sunward = pow(saturate(dot(viewRay, sunDirection)),
            max(g_FogDirectionalParams.x, 1.0f));
        primaryFogColor += g_FogDirectionalColorIntensity.rgb *
            (g_FogDirectionalColorIntensity.w * sunward);
    }
    if (g_FogColorDensity.w > 0.0f)
    {
        const float transmittance = exp(-HE_HeightFogOpticalDepth(
            cameraWS.y, positionWS.y, rayDistance, g_FogDistanceHeight.x,
            g_FogDistanceHeight.y, g_FogDistanceHeight.z, g_FogColorDensity.w));
        radiance = lerp(primaryFogColor, radiance, transmittance);
    }
    if (g_SecondaryFogColorDensity.w > 0.0f)
    {
        const float transmittance = exp(-HE_HeightFogOpticalDepth(
            cameraWS.y, positionWS.y, rayDistance, g_SecondaryFogDistanceHeight.x,
            g_SecondaryFogDistanceHeight.y, g_SecondaryFogDistanceHeight.z,
            g_SecondaryFogColorDensity.w));
        radiance = lerp(g_SecondaryFogColorDensity.rgb, radiance, transmittance);
    }
    return radiance;
}

float HE_GetScreenSpaceAo(float3 positionWS)
{
    const float4 clipPosition = mul(g_ViewProjection, float4(positionWS, 1.0f));
    if (clipPosition.w <= 0.0f) return 1.0f;
    const float2 uv = clipPosition.xy / clipPosition.w * float2(0.5f, -0.5f) + 0.5f;
    uint width, height;
    g_ScreenSpaceAo.GetDimensions(width, height);
    const int2 pixel = clamp(int2(uv * float2(width, height)),
        int2(0, 0), int2(width, height) - 1);
    return g_ScreenSpaceAo.Load(int3(pixel, 0));
}

static const uint c_PbrDebugBaseColor = 1u;
static const uint c_PbrDebugWorldNormal = 2u;
static const uint c_PbrDebugMetallic = 3u;
static const uint c_PbrDebugRoughness = 4u;
static const uint c_PbrDebugAo = 5u;
static const uint c_PbrDebugDirect = 6u;
static const uint c_PbrDebugDiffuseIbl = 7u;
static const uint c_PbrDebugSpecularIbl = 8u;
static const uint c_PbrDebugShadow = 9u;
static const float c_Pi = 3.14159265358979323846f;
static const float c_MaxFiniteHalf = 65504.0f;
// A raster pixel cannot resolve an arbitrarily narrow microfacet lobe. Keep
// the renderer's effective lobe finite, then add measured normal variance.
static const float c_MinResolvedPerceptualRoughness = 0.08f;
static const float c_MaxNormalVariance = 0.35f;

float DistributionGGX(float3 normalWS, float3 halfwayWS, float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float nDotH = saturate(dot(normalWS, halfwayWS));
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / max(c_Pi * denominator * denominator, 1e-12f);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max((1.0f - roughness).xxx, f0) - f0) *
        pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 SafeNormalize(float3 value)
{
    if (!all(isfinite(value)))
        return float3(0.0f, 0.0f, 1.0f);
    const float lengthSquared = dot(value, value);
    return isfinite(lengthSquared) && lengthSquared > 1e-12f
        ? value * rsqrt(lengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
}

float3 SanitizeHalfRadiance(float3 value)
{
    if (!all(isfinite(value)))
        return 0.0f;
    return clamp(value, 0.0f, c_MaxFiniteHalf);
}

float SanitizeUnit(float value, float fallback)
{
    return isfinite(value) ? saturate(value) : fallback;
}

float3x3 BuildWorldTbn(
    float3 normalWS,
    float3 tangentWS,
    float3 bitangentWS,
    float faceSign)
{
    const float3 sourceNormal = SafeNormalize(normalWS);
    const float3 orthogonalTangent =
        tangentWS - sourceNormal * dot(sourceNormal, tangentWS);
    const float3 fallbackTangent = abs(sourceNormal.z) < 0.999f
        ? SafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), sourceNormal))
        : SafeNormalize(cross(float3(0.0f, 1.0f, 0.0f), sourceNormal));
    const float3 tangent = SafeNormalize(
        dot(orthogonalTangent, orthogonalTangent) > 1e-8f
            ? orthogonalTangent
            : fallbackTangent);
    const float handedness = dot(cross(sourceNormal, tangent), bitangentWS) < 0.0f
        ? -1.0f
        : 1.0f;
    const float3 normal = sourceNormal * faceSign;
    const float3 bitangent = SafeNormalize(cross(normal, tangent)) * handedness;
    return float3x3(tangent, bitangent, normal);
}

float3 SampleMaterialNormal(
    Texture2D<float4> normalTexture,
    SamplerState materialSampler,
    float2 texCoord,
    float3x3 tangentBasis,
    float normalScale,
    out float normalMipVariance,
    uint normalEncoding = 0u)
{
    const float3 normalTS = HE_DecodeNormalMap(
        normalTexture.Sample(materialSampler, texCoord), normalScale,
        normalMipVariance, normalEncoding);
    return SafeNormalize(
        normalTS.x * tangentBasis[0] +
        normalTS.y * tangentBasis[1] +
        normalTS.z * tangentBasis[2]);
}

float ApplySpecularAntiAliasing(
    float roughness,
    float3 normalWS,
    float normalMipVariance)
{
    const float3 dndx = ddx(normalWS);
    const float3 dndy = ddy(normalWS);
    // For a box-filtered pixel, the variance of a linear normal ramp is
    // derivative^2 / 12. Add that footprint variance to the mip-derived
    // variance in GGX alpha space (alpha = perceptualRoughness^2).
    const float screenNormalVariance =
        all(isfinite(dndx)) && all(isfinite(dndy))
        ? (dot(dndx, dndx) + dot(dndy, dndy)) / 12.0f
        : c_MaxNormalVariance;
    const float normalVariance = min(
        max(normalMipVariance, 0.0f) + max(screenNormalVariance, 0.0f),
        c_MaxNormalVariance);
    const float minimumAlpha = c_MinResolvedPerceptualRoughness *
        c_MinResolvedPerceptualRoughness;
    const float materialAlpha = max(roughness * roughness, minimumAlpha);
    return sqrt(saturate(materialAlpha + normalVariance));
}

float ComputeSpecularOcclusion(float nDotV, float ao, float roughness)
{
    return HE_ComputeSpecularOcclusion(nDotV, ao, roughness);
}

float ComputePointLightAttenuation(
    float distanceToLight,
    float radius)
{
    const float safeRadius = max(radius, 1e-3f);
    const float safeDistance = max(distanceToLight, 1e-2f);
    const float normalizedDistance = distanceToLight / safeRadius;
    const float falloff = saturate(
        1.0f - normalizedDistance * normalizedDistance *
        normalizedDistance * normalizedDistance);
    return (falloff * falloff) /
        (safeDistance * safeDistance);
}

#include "CharacterLighting.hlsli"

float3 EvaluateLight(
    float3 baseColor,
    float metallic,
    float roughness,
    float3 normalWS,
    float3 viewWS,
    float3 lightWS,
    float3 radiance,
    out float3 diffuseLighting,
    float specularLevel = 0.5f)
{
    diffuseLighting = 0.0f;
    const float nDotL = saturate(dot(normalWS, lightWS));
    if (nDotL <= 0.0f)
        return 0.0f;

    const float3 halfwayWS = SafeNormalize(viewWS + lightWS);
    const float nDotV = saturate(dot(normalWS, viewWS));
    const float3 f0 = lerp((0.08f * saturate(specularLevel)).xxx, baseColor, metallic);
    const float3 fresnel = FresnelSchlick(saturate(dot(halfwayWS, viewWS)), f0);
    const float visibility = HE_VisibilitySmithGGXCorrelated(nDotV, nDotL, roughness);
    const float distribution = DistributionGGX(normalWS, halfwayWS, roughness);
    const float3 specular = distribution * visibility * fresnel;
    const float3 diffuseWeight = (1.0f - fresnel) * (1.0f - metallic);
    diffuseLighting = diffuseWeight * baseColor / 3.14159265f * radiance * nDotL;
    return diffuseLighting + specular * radiance * nDotL;
}

#include "DirectionalShadowSampling.hlsli"

float4x4 GetDirectionalShadowMatrix(int cascadeIndex)
{
    if (cascadeIndex == 0)
        return g_LightViewProjection;
    return g_AdditionalLightViewProjections[min(cascadeIndex - 1, 2)];
}

float3 GetDirectionalShadowCascadeDebugColor(int cascadeIndex)
{
    if (cascadeIndex == 0)
        return float3(1.0f, 0.25f, 0.2f);
    if (cascadeIndex == 1)
        return float3(0.25f, 1.0f, 0.25f);
    if (cascadeIndex == 2)
        return float3(0.25f, 0.45f, 1.0f);
    return float3(1.0f, 0.85f, 0.2f);
}

int SelectDirectionalShadowCascade(float viewDepth)
{
    const int cascadeCount = clamp(
        (int)round(g_DirectionalShadowCascadeSettings.x),
        1,
        4);
    [unroll]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadeCount)
            break;
        if (viewDepth <= g_DirectionalShadowCascadeSplits[cascade])
            return cascade;
    }
    return -1;
}

float SampleDirectionalShadowCascade(
    float3 positionWS,
    float3 geometricNormalWS,
    float3 lightWS,
    int cascadeIndex)
{
    const float4x4 shadowMatrix = GetDirectionalShadowMatrix(cascadeIndex);
    const float4x4 referenceMatrix = GetDirectionalShadowMatrix(0);
    const float normalBiasScale =
        1.0f - saturate(dot(geometricNormalWS, lightWS));
    const float3 biasedPositionWS = positionWS +
        geometricNormalWS * g_ShadowSettings.z * normalBiasScale;
    const float4 shadowClip = mul(
        shadowMatrix,
        float4(biasedPositionWS, 1.0f));
    if (shadowClip.w <= 0.0f)
        return 1.0f;

    const float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    const float2 shadowUv = float2(
        shadowNdc.x * 0.5f + 0.5f,
        0.5f - shadowNdc.y * 0.5f);
    if (any(shadowUv < 0.0f) || any(shadowUv > 1.0f) ||
        shadowNdc.z < 0.0f || shadowNdc.z > 1.0f)
    {
        return 1.0f;
    }

    uint shadowWidth = 0;
    uint shadowHeight = 0;
    uint shadowLayers = 0;
    g_DirectionalShadow.GetDimensions(shadowWidth, shadowHeight, shadowLayers);
    if ((uint)cascadeIndex >= shadowLayers)
        return 1.0f;
    // Authored depth bias is measured in the first cascade's depth range.
    // Preserve that world distance in every cascade: a fixed normalized bias
    // otherwise detaches far shadows as their orthographic depth range grows.
    const float depthScale = length(shadowMatrix[2].xyz);
    const float referenceDepthScale = length(referenceMatrix[2].xyz);
    const float compareDepth = saturate(shadowNdc.z -
        HE_DirectionalShadowDepthBias(g_ShadowSettings.y, depthScale, referenceDepthScale));
    if (g_ShadowSettings.w < 0.0f)
    {
        const float radius = -g_ShadowSettings.w - 1.0f;
        // Use a constant world-space filter footprint until limited by one
        // far-cascade texel. This avoids magnifying the penumbra at each split.
        // Keep a positive radius for bilinear comparison; zero remains the
        // explicit hard-shadow contract used by bUsePcf=false.
        const float cascadeRadius = HE_DirectionalShadowFilterRadius(radius,
            length(shadowMatrix[0].xyz), length(referenceMatrix[0].xyz));
        return HE_ContinuousShadowPcf(g_DirectionalShadow, shadowUv,
            uint(cascadeIndex), uint2(shadowWidth, shadowHeight), compareDepth,
            cascadeRadius);
    }
    const int radius = clamp((int)round(g_ShadowSettings.w), 0, 4);
    const int2 maximumTexel = int2(
        max((int)shadowWidth - 1, 0),
        max((int)shadowHeight - 1, 0));
    const int2 centerTexel = clamp(
        int2(shadowUv * float2(shadowWidth, shadowHeight)),
        int2(0, 0),
        maximumTexel);
    float visibility = 0.0f;
    float sampleCount = 0.0f;
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            const int2 sampleTexel = clamp(
                centerTexel + int2(x, y),
                int2(0, 0),
                maximumTexel);
            const float shadowDepth = g_DirectionalShadow.Load(
                int4(sampleTexel, cascadeIndex, 0));
            visibility += compareDepth <= shadowDepth ? 1.0f : 0.0f;
            sampleCount += 1.0f;
        }
    }
    return visibility / max(sampleCount, 1.0f);
}

float EvaluateDirectionalShadow(
    float3 positionWS,
    float3 geometricNormalWS,
    float3 lightWS,
    out float3 cascadeDebugColor)
{
    cascadeDebugColor = 1.0f;
    if (g_ShadowSettings.x <= 0.0f)
        return 1.0f;

    const float viewDepth = abs(mul(
        g_ShadowCameraView,
        float4(positionWS, 1.0f)).z);
    const int cascadeIndex = SelectDirectionalShadowCascade(viewDepth);
    if (cascadeIndex < 0)
        return 1.0f;

    const int cascadeCount = clamp(
        (int)round(g_DirectionalShadowCascadeSettings.x),
        1,
        4);
    float visibility = SampleDirectionalShadowCascade(
        positionWS,
        geometricNormalWS,
        lightWS,
        cascadeIndex);
    cascadeDebugColor = GetDirectionalShadowCascadeDebugColor(cascadeIndex);

    const float previousSplit = cascadeIndex > 0
        ? g_DirectionalShadowCascadeSplits[cascadeIndex - 1]
        : g_DirectionalShadowDebugSettings.w;
    const float cascadeSpan = max(
        g_DirectionalShadowCascadeSplits[cascadeIndex] - previousSplit,
        1e-3f);
    if (cascadeIndex + 1 < cascadeCount)
    {
        const float blendRange = cascadeSpan *
            saturate(g_DirectionalShadowCascadeSettings.z);
        const float blendStart =
            g_DirectionalShadowCascadeSplits[cascadeIndex] - blendRange;
        if (blendRange > 0.0f && viewDepth > blendStart)
        {
            const float blend = smoothstep(
                blendStart,
                g_DirectionalShadowCascadeSplits[cascadeIndex],
                viewDepth);
            visibility = lerp(
                visibility,
                SampleDirectionalShadowCascade(
                    positionWS,
                    geometricNormalWS,
                    lightWS,
                    cascadeIndex + 1),
                blend);
            cascadeDebugColor = lerp(
                cascadeDebugColor,
                GetDirectionalShadowCascadeDebugColor(cascadeIndex + 1),
                blend);
        }
    }
    else
    {
        const float fadeRange = cascadeSpan *
            saturate(g_DirectionalShadowCascadeSettings.w);
        const float fadeStart =
            g_DirectionalShadowCascadeSplits[cascadeIndex] - fadeRange;
        if (fadeRange > 0.0f && viewDepth > fadeStart)
        {
            const float fade = smoothstep(
                fadeStart,
                g_DirectionalShadowCascadeSplits[cascadeIndex],
                viewDepth);
            visibility = lerp(visibility, 1.0f, fade);
            cascadeDebugColor = lerp(cascadeDebugColor, 1.0f, fade);
        }
    }
    return lerp(1.0f, visibility, saturate(g_ShadowSettings.x));
}

#include "BakedIrradiance.hlsli"

float3 ShadeSceneSurface(
#if HE_DEFERRED_SKIN_SSS
    out float3 skinDiffuse,
#endif
    float3 positionWS, float3 baseColor, float metallic, float roughness,
    float ao, float3 normalWS, float3 geometricNormal, float3 emissive,
    bool bApplyScreenAo = true, uint characterModel = 0u,
    float3 characterTint = 0.0f, float3 strandWS = float3(0, 1, 0),
    float characterStrength = 0.0f, float specularLevel = 0.5f,
    float staticLightmapBlend = 0.0f)
{
    const float3 viewWS = SafeNormalize(
        g_CameraPositionAndExposure.xyz - positionWS);

    const float nDotV = saturate(dot(normalWS, viewWS));
    const float3 f0 = lerp((0.08f * saturate(specularLevel)).xxx, baseColor, metallic);
    const float3 environmentFresnel = FresnelSchlickRoughness(nDotV, f0, roughness);
    const float3 environmentDiffuseWeight =
        (1.0f - environmentFresnel) * (1.0f - metallic);
    const float3 irradiance = HE_BakedIrradiance(positionWS, normalWS,
        g_IrradianceMap.SampleLevel(g_ShadowSampler, normalWS, 0.0f).rgb * g_IblSettings.x);
    const float3 reflectionWS = reflect(-viewWS, normalWS);
    const float3 prefiltered = g_PrefilterMap.SampleLevel(
        g_ShadowSampler,
        reflectionWS,
        roughness * g_IblSettings.y).rgb;
    const float2 brdf = g_BrdfLut.SampleLevel(
        g_ShadowSampler,
        float2(nDotV, roughness),
        0.0f).rg;
    const float indirectAo = ao * (bApplyScreenAo ? HE_GetScreenSpaceAo(positionWS) : 1.0f);
    const float3 diffuseIbl = (1.0f - saturate(staticLightmapBlend)) *
        environmentDiffuseWeight * irradiance *
        (baseColor / c_Pi) * indirectAo *
        (bApplyScreenAo ? 1.0f - saturate(g_IblSettings.w) : 1.0f);
    const float3 specularIbl = prefiltered *
        HE_EnvironmentSpecularFactor(baseColor, metallic, roughness, nDotV,
            indirectAo, brdf, specularLevel) * g_IblSettings.x;
    float3 directLighting = 0.0f;
#if HE_DEFERRED_SKIN_SSS
    skinDiffuse = characterModel == 2u ? diffuseIbl : 0.0f;
#endif
    float directionalShadow = 1.0f;
    float3 shadowCascadeDebugColor = 1.0f;
    const uint cluster = HE_GetSceneLightCluster(positionWS);
    [loop]
    for (uint word = 0; word < g_LightGridDimensions.w; ++word)
    {
        uint mask = g_ClusterLightMasks[cluster * g_LightGridDimensions.w + word];
        [loop]
        while (mask != 0u)
        {
            const uint bit = firstbitlow(mask);
            mask &= mask - 1u;
            const uint lightIndex = word * 32u + bit;
            if (lightIndex >= g_LightGridFrameDimensions.z)
                continue;
            const HESceneLight light = g_SceneLights[lightIndex];
            float3 lightWS;
            float attenuation = 1.0f;
            float shadow = 1.0f;
            if (light.ConeReservedTypeShadow.z > 1.5f)
            {
                lightWS = -light.DirectionOuterCosine.xyz;
                if (light.ConeReservedTypeShadow.w > 0.5f)
                {
                    shadow = EvaluateDirectionalShadow(positionWS, geometricNormal,
                        lightWS, shadowCascadeDebugColor);
                    directionalShadow = shadow;
                }
            }
            else
            {
                const float3 toLight = light.PositionRadius.xyz - positionWS;
                const float distanceToLight = length(toLight);
                lightWS = SafeNormalize(toLight);
                attenuation = ComputePointLightAttenuation(distanceToLight,
                    light.PositionRadius.w);
                if (light.ConeReservedTypeShadow.z > 0.5f)
                {
                    const float coneCosine = dot(light.DirectionOuterCosine.xyz, -lightWS);
                    const float cone = saturate((coneCosine - light.DirectionOuterCosine.w) /
                        max(light.ConeReservedTypeShadow.x - light.DirectionOuterCosine.w, 1e-5f));
                    attenuation *= cone * cone * (3.0f - 2.0f * cone);
                }
                if (light.ConeReservedTypeShadow.w > 0.5f && attenuation > 0.0f)
                {
                    shadow = HE_EvaluateLocalShadow(positionWS, geometricNormal,
                        light.PositionRadius.xyz, light.ConeReservedTypeShadow.z < 0.5f,
                        (uint)round(light.ConeReservedTypeShadow.w) - 1u);
                }
            }
            const float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.w * attenuation;
            float3 diffuseLighting;
            const float3 standardLight = EvaluateLight(baseColor, metallic, roughness,
                normalWS, viewWS, lightWS, radiance, diffuseLighting, specularLevel);
#if HE_DEFERRED_SKIN_SSS
            if (characterModel == 2u)
            {
                // Profile diffusion replaces the local wrapped approximation;
                // only diffuse transport is filtered, never the surface lobe.
                directLighting += shadow * standardLight;
                skinDiffuse += shadow * diffuseLighting;
                continue;
            }
#endif
            directLighting += shadow * HE_CharacterLight(characterModel, baseColor,
                roughness, normalWS, viewWS, lightWS, strandWS, characterTint,
                characterStrength, radiance, standardLight);
        }
    }
    float3 color = directLighting + diffuseIbl + specularIbl + emissive;

    const uint pbrDebugMode = (uint)round(max(
        g_DirectionalShadowDebugSettings.y,
        0.0f));
    if (pbrDebugMode == c_PbrDebugBaseColor)
        color = baseColor;
    else if (pbrDebugMode == c_PbrDebugWorldNormal)
        color = normalWS * 0.5f + 0.5f;
    else if (pbrDebugMode == c_PbrDebugMetallic)
        color = metallic.xxx;
    else if (pbrDebugMode == c_PbrDebugRoughness)
        color = roughness.xxx;
    else if (pbrDebugMode == c_PbrDebugAo)
        color = ao.xxx;
    else if (pbrDebugMode == 12u)
        color = (bApplyScreenAo ? HE_GetScreenSpaceAo(positionWS) : 1.0f).xxx;
    else if (pbrDebugMode == 13u)
    {
        // View-space depth: black at the camera, white at 1000 world units.
        const float depth = abs(mul(g_ShadowCameraView, float4(positionWS, 1.0f)).z);
        color = saturate(log2(1.0f + depth) / log2(1001.0f)).xxx;
    }
    else if (pbrDebugMode == c_PbrDebugDirect)
        color = directLighting;
    else if (pbrDebugMode == c_PbrDebugDiffuseIbl)
        color = diffuseIbl;
    else if (pbrDebugMode == c_PbrDebugSpecularIbl)
        color = specularIbl;
    else if (pbrDebugMode == c_PbrDebugShadow)
        color = directionalShadow.xxx;

    // Cascade tint is an overlay, so it remains visible when combined with a
    // PBR component view (notably Shadow + cascade debug during capture).
    if (g_DirectionalShadowDebugSettings.x > 0.5f && g_ShadowSettings.x > 0.0f)
        color = lerp(color, shadowCascadeDebugColor, 0.65f);

    // Exposure, tone mapping and transfer-function encoding intentionally happen
    // in SceneToneMap.hlsl, once per output pixel.
    return SanitizeHalfRadiance(pbrDebugMode == 0u
        ? HE_ApplyDistanceHeightFog(color, positionWS) : color);
}

#endif
