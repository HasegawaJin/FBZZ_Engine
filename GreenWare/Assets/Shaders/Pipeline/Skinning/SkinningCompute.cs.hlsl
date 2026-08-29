// FBZZ Engine
// Pipeline/Skinning/SkinningCompute.cs.hlsl | Pipeline
// コンピュートスキニング — ボーン変形を 1 フレームに 1 回だけ計算する
//
// WHY: これまでスキニングは各マテリアルの VS 内で行っていた。つまり同じキャラクターを
//      シャドウマップと画面の両方へ描くたびに、まったく同じボーン変形を計算し直していた。
//      さらにボーンパレットは 128 要素の cbuffer で、頂点あたり 4 回の動的インデックス
//      アクセスが発生する (定数キャッシュの高速経路から外れやすい既知の重い形)。
//
//      ここで一度だけ変形して「ただの静的メッシュ」に落としておけば:
//        - シャドウは SkinnedShadowMap ではなく通常の ShadowMap で描ける
//        - マテリアルも Material/Skinned/* ではなく Material/Surface/* が使える
//        - 結果として不透明スキンドが GBuffer 経路へそのまま乗る
//      パスが増えるほど得をする構造になる。
//
// 出力レイアウトは静的メッシュの renderer::Vertex と完全に一致させること。
// 一致していれば既存の Surface 系シェーダーの入力レイアウトがそのまま噛み合う。

#include "Common/Binding.hlsli"
#include "Platform/Backend.hlsli"

// 入力頂点。C++ の renderer::SkinnedVertex と完全に一致させること (76 bytes)。
struct SkinSrcVertex
{
    float3 position;     //  0
    float3 normal;       // 12
    float3 tangent;      // 24
    float2 uv;           // 36
    uint4  boneIndices;  // 44
    float4 boneWeights;  // 60
};                       // = 76

// 出力頂点。C++ の renderer::Vertex と完全に一致させること (60 bytes)。
struct SkinnedOutVertex
{
    float3 position;     //  0
    float3 normal;       // 12
    float3 tangent;      // 24
    float2 uv;           // 36
    float4 color;        // 44
};                       // = 60

StructuredBuffer<SkinSrcVertex>     gSrcVertices : register(SB_SKIN_SRC_VERTICES);
// ボーンパレットは cbuffer ではなく StructuredBuffer で受ける。
// WHY: 動的インデックスは SRV のほうが素直に走る。cbuffer 配列への動的アクセスは
//      ハードウェアによって定数キャッシュを外れ、頂点あたり 16 回のスカラーロードが
//      ベクターロードへ落ちる。ここは全スキンド頂点が必ず通る場所なので効きが大きい。
StructuredBuffer<float4x4>          gBoneMatrices : register(SB_SKIN_BONES);
RWStructuredBuffer<SkinnedOutVertex> gOutVertices : register(UAV_SKINNED_VERTICES);

cbuffer SkinningConstants : register(b0)
{
    uint gVertexCount;
    uint _skinPad0;
    uint _skinPad1;
    uint _skinPad2;
};

#define SKINNING_GROUP_SIZE 64

[numthreads(SKINNING_GROUP_SIZE, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint index = dtid.x;
    if (index >= gVertexCount) return;

    SkinSrcVertex v = gSrcVertices[index];

    // 4 影響ボーンの線形ブレンド。VS 版 (BlendSkinMatrix) と同じ式にすること。
    float4x4 skin =
          gBoneMatrices[v.boneIndices.x] * v.boneWeights.x
        + gBoneMatrices[v.boneIndices.y] * v.boneWeights.y
        + gBoneMatrices[v.boneIndices.z] * v.boneWeights.z
        + gBoneMatrices[v.boneIndices.w] * v.boneWeights.w;

    SkinnedOutVertex o;
    o.position = mul(float4(v.position, 1.0f), skin).xyz;
    // 法線・接線は平行移動を含めない (w=0)。
    // NOTE: 非等方スケールのボーンでは厳密には逆転置が要るが、スキンメッシュの
    //       ボーン行列は回転+平行移動が支配的なので VS 版と同じ近似に揃える。
    o.normal   = mul(float4(v.normal,  0.0f), skin).xyz;
    o.tangent  = mul(float4(v.tangent, 0.0f), skin).xyz;
    o.uv       = v.uv;
    // SkinnedVertex は頂点カラーを持たない。静的メッシュの「色なし」と同じ白を書く。
    o.color    = float4(1.0f, 1.0f, 1.0f, 1.0f);

    gOutVertices[index] = o;
}
