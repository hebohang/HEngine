#ifndef HE_BAKED_IRRADIANCE
#define HE_BAKED_IRRADIANCE

// Real orthonormal SH, ordering: 1, y, z, x, xy, yz, 3z²-1, xz, x²-y².
// Coefficients already include cosine convolution. Return E in linear units.
float3 HE_BakedIrradiance(float3 positionWS, float3 normalWS, float3 fallback)
{
    float x = normalWS.x, y = normalWS.y, z = normalWS.z;
    float basis[9] = {0.2820947918f, 0.4886025119f*y, 0.4886025119f*z,
        0.4886025119f*x, 1.0925484306f*x*y, 1.0925484306f*y*z,
        0.3153915653f*(3*z*z-1), 1.0925484306f*x*z, 0.5462742153f*(x*x-y*y)};
    float3 sum = 0;
    float total = 0;
    [loop] for (uint i = 0; i < min(g_BakedIrradianceCount.x, 16u); ++i)
    {
        HEBakedIrradianceVolume volume = g_BakedIrradianceVolumes[i];
        float3 edge = min(positionWS-volume.BoundsMinBlend.xyz,
            volume.BoundsMaxIntensity.xyz-positionWS);
        float weight = saturate(min(edge.x,min(edge.y,edge.z)) / volume.BoundsMinBlend.w);
        if (weight <= 0) continue;
        float3 irradiance = 0;
        [unroll] for (uint j = 0; j < 9; ++j) irradiance += volume.SH[j].rgb*basis[j];
        sum += max(irradiance,0)*volume.BoundsMaxIntensity.w*weight;
        total += weight;
    }
    // Replace the fallback, never add a second ambient term. Volumes are world
    // aligned and should end at separating walls to avoid cross-room leakage.
    return total > 0 ? lerp(fallback, sum/max(total,1e-6f), saturate(total)) : fallback;
}
#endif
