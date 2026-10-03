/// @file    RayEnvironment.hlsli
/// @brief   Raw linear HDR cube lookup and shared importance sampling PDF.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#ifndef FBZZ_RAY_ENVIRONMENT_HLSLI
#define FBZZ_RAY_ENVIRONMENT_HLSLI
#ifndef FBZZ_RAY_ENV_TEXTURE_SLOT
#define FBZZ_RAY_ENV_TEXTURE_SLOT 4
#endif
#ifndef FBZZ_RAY_ENV_TABLE_SLOT
#define FBZZ_RAY_ENV_TABLE_SLOT 7
#endif
struct RayEnvironmentRecord { float selectionCdf; float selectionPdf; float solidAngle; uint reserved; };
static TextureCube<float4> rayEnvironmentTexture = ResourceDescriptorHeap[FbzzPixelSlot(FBZZ_RAY_ENV_TEXTURE_SLOT)];
static StructuredBuffer<RayEnvironmentRecord> rayEnvironmentTable = ResourceDescriptorHeap[FbzzPixelSlot(FBZZ_RAY_ENV_TABLE_SLOT)];
#ifndef FBZZ_RAY_LINEAR_SAMPLER_DEFINED
#define FBZZ_RAY_LINEAR_SAMPLER_DEFINED
SamplerState rayLinearSampler : register(s0);
#endif
static const float RAY_ENV_PI = 3.14159265358979323846f;
float3 RayEnvironmentLocal(float3 d, float rotation)
{
    float s, c; sincos(rotation, s, c);
    return float3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
}
float3 RayEnvironmentWorld(float3 d, float rotation)
{
    float s, c; sincos(rotation, s, c);
    return float3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}
float3 RayEnvironmentCubeDirection(uint face, float2 uv)
{
    float3 d = face == 0u ? float3(1, -uv.y, -uv.x)
        : face == 1u ? float3(-1, -uv.y, uv.x)
        : face == 2u ? float3(uv.x, 1, uv.y)
        : face == 3u ? float3(uv.x, -1, -uv.y)
        : face == 4u ? float3(uv.x, -uv.y, 1) : float3(-uv.x, -uv.y, -1);
    return normalize(d);
}
void RayEnvironmentCubeCoordinates(float3 d, out uint face, out float2 uv)
{
    float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) { face = d.x >= 0 ? 0u : 1u; uv = float2(d.x >= 0 ? -d.z : d.z, -d.y) / a.x; }
    else if (a.y >= a.z) { face = d.y >= 0 ? 2u : 3u; uv = float2(d.x, d.y >= 0 ? d.z : -d.z) / a.y; }
    else { face = d.z >= 0 ? 4u : 5u; uv = float2(d.z >= 0 ? d.x : -d.x, -d.y) / a.z; }
}
/// @note mode 0 is absent, 1 is constant radiance, 2 is the explicit raw HDR cube; filtered IBL is never used here.
float3 RayEnvironmentRadiance(float3 direction, uint mode, float3 constantRadiance, float rotation, float intensity)
{
    if (mode == 1u) return constantRadiance;
    if (mode != 2u) return 0;
    return rayEnvironmentTexture.SampleLevel(rayLinearSampler, RayEnvironmentLocal(direction, rotation), 0).rgb * intensity;
}
/// @note The discrete PMF is the actual rounded CDF interval; conditional UV sampling uses the same cube Jacobian here and in NEE.
/// @see https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights Directional importance densities.
float RayEnvironmentPdf(float3 direction, uint mode, uint tableCount, uint faceSize, float rotation)
{
    if (mode == 1u) return 1 / (4 * RAY_ENV_PI);
    if (mode != 2u || faceSize == 0u || tableCount != 6u * faceSize * faceSize
        || !all(isfinite(direction)) || !any(direction != 0)) return 0;
    uint face; float2 uv;
    RayEnvironmentCubeCoordinates(RayEnvironmentLocal(direction, rotation), face, uv);
    uint2 cell = min((uint2)(saturate(uv * 0.5f + 0.5f) * faceSize), faceSize - 1u);
    uint index = (face * faceSize + cell.y) * faceSize + cell.x;
    return rayEnvironmentTable[index].selectionPdf * (faceSize * faceSize * 0.25f) * pow(1 + dot(uv, uv), 1.5f);
}
bool SampleRayEnvironment(float3 u, uint mode, float3 constantRadiance, uint tableCount, uint faceSize,
    float rotation, float intensity, out float3 direction, out float3 radiance, out float pdf)
{
    direction = 0; radiance = 0; pdf = 0;
    if (mode == 1u) {
        float z = 1 - 2 * u.x, phi = 2 * RAY_ENV_PI * u.y;
        float r = sqrt(max(0, 1 - z * z));
        direction = float3(r * cos(phi), z, r * sin(phi));
        radiance = constantRadiance; pdf = 1 / (4 * RAY_ENV_PI);
    } else if (mode == 2u && faceSize > 0u && tableCount == 6u * faceSize * faceSize) {
        uint low = 0u, high = tableCount;
        while (low < high) {
            uint middle = low + (high - low) / 2u;
            if (u.x < rayEnvironmentTable[middle].selectionCdf) high = middle;
            else low = middle + 1u;
        }
        if (low >= tableCount) return false;
        uint face = low / (faceSize * faceSize), cell = low % (faceSize * faceSize);
        float2 uv = 2 * (float2(cell % faceSize, cell / faceSize) + u.yz) / faceSize - 1;
        direction = RayEnvironmentWorld(RayEnvironmentCubeDirection(face, uv), rotation);
        radiance = RayEnvironmentRadiance(direction, mode, constantRadiance, rotation, intensity);
        pdf = rayEnvironmentTable[low].selectionPdf * (faceSize * faceSize * 0.25f) * pow(1 + dot(uv, uv), 1.5f);
    } else return false;
    return all(isfinite(direction)) && all(isfinite(radiance)) && all(radiance >= 0) && isfinite(pdf) && pdf > 0;
}
#endif
