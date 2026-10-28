/// @file    TerrainGpuShadow.hlsl
/// @brief   Terrain shadows use the same GPU height evaluation as the visible surface.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "Common/Constants.hlsli"
#include "Terrain/TerrainGeometry.hlsli"
float4 VSMain(uint vertexId : SV_VertexID) : SV_POSITION
{
    const TerrainGeometryVertex vertex = EvaluateTerrainGeometry(vertexId);
    return mul(mul(float4(vertex.position, 1.0f), world), viewProjection);
}
void PSMain() {}
