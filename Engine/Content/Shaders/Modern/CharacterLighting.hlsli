#ifndef HE_CHARACTER_LIGHTING_HLSLI
#define HE_CHARACTER_LIGHTING_HLSLI

// Direct-light character response shared by clustered forward and deferred.
// Skin is a local diffusion approximation, not screen-space/profile diffusion.
float HE_StrandLobe(float3 tangent, float3 halfVector, float exponent)
{
    const float tDotH = clamp(dot(tangent, halfVector), -1.0f, 1.0f);
    const float sinTheta = sqrt(saturate(1.0f - tDotH * tDotH));
    // A cylindrical sin^n band integrates to 2*pi*Beta(1/2, n/2+1),
    // unlike a cos^n surface lobe. Gamma-ratio expansion is accurate over
    // our n >= 0.9 range (including the wider secondary lobe).
    const float z = 0.5f * exponent + 1.0f;
    const float invZ = rcp(z);
    const float ratio = sqrt(z) * (1.0f + invZ * (-0.125f + invZ *
        (0.0078125f + invZ * (0.0048828125f - invZ * 0.000640869140625f))));
    const float normalization = ratio / (2.0f * 3.14159265f * 1.77245385f);
    return normalization * pow(sinTheta, exponent);
}

float3 HE_CharacterLight(uint model, float3 baseColor, float roughness,
    float3 n, float3 v, float3 l, float3 strand, float3 tint,
    float strength, float3 radiance, float3 standardLight)
{
    const float ndl = saturate(dot(n, l));
    if (model == 2u)
    {
        // Normalized wrapped diffuse broadens the terminator while preserving
        // a sharp surface reflection. RGB weights approximate wavelength spread.
        const float3 wrap = float3(0.45f, 0.22f, 0.12f);
        const float3 diffuse = saturate((dot(n, l) + wrap) / (1.0f + wrap));
        const float3 normalized = diffuse / (1.0f + wrap);
        return max(standardLight + radiance * baseColor * saturate(tint) *
            (normalized - ndl) * (0.96f / 3.14159265f), 0.0f);
    }
    if (model == 3u)
    {
        const float3 halfSum = v + l;
        const float halfLengthSquared = dot(halfSum, halfSum);
        const float3 h = halfLengthSquared > 1e-8f
            ? halfSum * rsqrt(halfLengthSquared) : n;
        const float exponent = clamp(2.0f / max(roughness * roughness, 0.025f) - 2.0f, 2.0f, 78.0f);
        const float3 t1 = normalize(strand + n * 0.12f);
        const float3 t2 = normalize(strand - n * 0.22f);
        const float primary = HE_StrandLobe(t1, h, exponent);
        const float secondary = HE_StrandLobe(t2, h, exponent * 0.45f);
        const float fresnel = 0.0465f + 0.9535f * pow(1.0f - saturate(dot(v, h)), 5.0f);
        const float visibility = sqrt(saturate(1.0f - dot(strand, l) * dot(strand, l)));
        const float3 specular = (halfLengthSquared > 1e-8f ? fresnel : 0.0f) *
            (primary * 0.65f + baseColor * secondary * 0.35f);
        const float transmission = pow(saturate(dot(-l, v)), 5.0f) * saturate(-dot(n, l)) * 0.15f;
        const float3 hairLight = radiance * (baseColor *
            ((1.0f - fresnel) * (0.85f * ndl / 3.14159265f + transmission)) + specular * visibility);
        return lerp(standardLight, hairLight, saturate(strength));
    }
    if (model == 4u)
    {
        const float3 h = normalize(v + l + 1e-7f);
        const float sheen = pow(1.0f - saturate(dot(n, h)), 5.0f);
        return standardLight * (1.0f - saturate(tint) * 0.15f) +
            radiance * saturate(tint) * sheen * ndl;
    }
    if (model == 7u)
    {
        // Thin masked leaves pass colored light toward the camera when lit
        // from behind. A small normalized wrap also broadens the front-side
        // terminator instead of leaving a hard plastic-looking leaf edge.
        // Light attenuation and shadowing remain in the shared scene path.
        const float backlit = saturate(-dot(n, l));
        const float facing = saturate(dot(n, v));
        const float forwardScatter = pow(saturate(dot(-l, v)), 4.0f);
        const float transmission = backlit * (0.55f + 0.45f * forwardScatter)
            * (0.35f + 0.65f * facing);
        const float wrapped = saturate((dot(n, l) + 0.35f) / 1.35f);
        const float diffuse = (wrapped - ndl) * 0.5f;
        return standardLight + radiance *
            (baseColor * diffuse + saturate(tint) * transmission) / 3.14159265f;
    }
    return standardLight;
}
#endif
