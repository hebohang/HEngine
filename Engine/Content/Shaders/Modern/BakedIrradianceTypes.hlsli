#ifndef HE_BAKED_IRRADIANCE_TYPES
#define HE_BAKED_IRRADIANCE_TYPES
struct HEBakedIrradianceVolume
{
    float4 BoundsMinBlend;
    float4 BoundsMaxIntensity;
    float4 SH[9];
};
#endif
