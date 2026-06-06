// FBZZ Engine
// Material/Effects/MeshTrail.hlsl | Material
// MeshTrailComponent 用 Static Mesh 残像シェーダー
// PSO: SOLID_NOCULL or SOLID + ALPHA_BLEND + DEPTH_READ

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MeshTrailConstants : register(CB_MATERIAL)
{
    float4 trailColor;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/DX11.hlsli"

struct MeshTrailPSIn
{
    float4 svPosition : SV_POSITION;
};

MeshTrailPSIn VSMain(VSInput v)
{
    MeshTrailPSIn o;
    float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    return o;
}

float4 PSMain(MeshTrailPSIn input) : SV_Target0
{
    return trailColor;
}
