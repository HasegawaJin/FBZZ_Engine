// FBZZ Engine
// Pipeline/Mask/ObjectMask.hlsl | fbzz::renderer
// オブジェクトマスク。申告された RGBA をそのままシルエットへ書く

// b2 は «申告 1 件ぶんのペイロードとフラグ» に使う。マテリアルのパラメーターは
// 要らない (マスクはシルエットしか描かない)。
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/BindlessIndices.hlsli"

cbuffer ObjectMaskConstants : register(CB_MATERIAL)
{
    float4 objectMaskPayload;  // そのまま出力する RGBA
    float4 objectMaskFlags;    // x = visibleOnly
};

// シーン深度。マスクの時点で «見えている面» に絞るために読む。
//
// WHY 読む側 (カスタムパス) で遮蔽を判定しないか:
//   後段が SceneHDR で走る場合、描き先は hdrRT でその深度は書き込み先として
//   束縛されている。同じリソースを SRV として同時には読めない。マスクを描く
//   ここは別の RT へ描いているので、シーン深度を自由に読める。
FBZZ_TEX2D_T(float, texSceneDepth, TEX_DEPTH_SLOT);

// 自分自身との比較は等しくなるので、しきい値は «明らかに手前に別の面がある» 分だけ。
static const float kObjectMaskDepthBias = 0.00002f;

struct VSInput
{
    float3 position : POSITION;
};

struct PSInput
{
    float4 position : SV_POSITION;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float3 worldPos = mul(float4(input.position, 1.0f), world).xyz;
    output.position = mul(float4(worldPos, 1.0f), viewProjection);
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    if (objectMaskFlags.x > 0.5f) {
        const float sceneDepth = texSceneDepth.Load(int3((int2)input.position.xy, 0));
        if (input.position.z > sceneDepth + kObjectMaskDepthBias) discard;
    }
    return objectMaskPayload;
}
