// FBZZ Engine
// Material/Effects/MeshTrail.hlsl | Material
// MeshTrailComponent 用 Static Mesh 残像シェーダー (組み込みの既定)
// PSO: SOLID_NOCULL or SOLID + ALPHA_BLEND + DEPTH_READ
//
// 見た目を差し替えたいときはこれを直さず、.mat に自分の .hlsl を宣言する
// (契約は Material/Effects/MeshTrailMaterial.hlsli)。ここは «何も足さない» 既定で
// あり続ける ─ 直すと、材質を持たない全ての残像の絵が黙って変わる。

#include "Material/Effects/MeshTrailMaterial.hlsli"

float4 PSMain(MeshTrailPSIn input) : SV_Target0
{
    return gMeshTrailTex.Sample(gSampler, input.uv) * trailColor;
}
