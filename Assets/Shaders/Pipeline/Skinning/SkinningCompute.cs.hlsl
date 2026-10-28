/// @file    SkinningCompute.cs.hlsl
/// @brief   Compute skinning with either standalone or offset shared GPU palettes.
/// @author  Hasegawa Jin
/// @date    2026-08-14
/// @note WHY: これまでスキニングは各マテリアルの VS 内で行っていた。つまり同じキャラクターを
/// @note シャドウマップと画面の両方へ描くたびに、まったく同じボーン変形を計算し直していた。
/// @note さらにボーンパレットは 128 要素の cbuffer で、頂点あたり 4 回の動的インデックス
/// @note アクセスが発生する (定数キャッシュの高速経路から外れやすい既知の重い形)。
///
/// @note ここで一度だけ変形して「ただの静的メッシュ」に落としておけば:
/// @note - シャドウは SkinnedShadowMap ではなく通常の ShadowMap で描ける
/// @note - マテリアルも Material/Skinned/* ではなく Material/Surface/* が使える
/// @note - 結果として不透明スキンドが GBuffer 経路へそのまま乗る
/// @note パスが増えるほど得をする構造になる。
///
/// @note 出力レイアウトは静的メッシュの renderer::Vertex と完全に一致させること。
/// @note 一致していれば既存の Surface 系シェーダーの入力レイアウトがそのまま噛み合う。

#include "Common/Binding.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note 入力頂点。C++ の renderer::SkinnedVertex と完全に一致させること (76 bytes)。
struct SkinSrcVertex
{
    float3 position;     /// @note  0
    float3 normal;       /// @note 12
    float3 tangent;      /// @note 24
    float2 uv;           /// @note 36
    uint4  boneIndices;  /// @note 44
    float4 boneWeights;  /// @note 60
};                       /// @note = 76

/// @note 出力頂点。C++ の renderer::Vertex と完全に一致させること (60 bytes)。
struct SkinnedOutVertex
{
    float3 position;     /// @note  0
    float3 normal;       /// @note 12
    float3 tangent;      /// @note 24
    float2 uv;           /// @note 36
    float4 color;        /// @note 44
};                       /// @note = 60

FBZZ_SBUFFER_T(SkinSrcVertex, gSrcVertices, SB_SKIN_SRC_VERTICES_SLOT);
/// @note ボーンパレットは cbuffer ではなく StructuredBuffer で受ける。
/// @note WHY: 動的インデックスは SRV のほうが素直に走る。cbuffer 配列への動的アクセスは
/// @note ハードウェアによって定数キャッシュを外れ、頂点あたり 16 回のスカラーロードが
/// @note ベクターロードへ落ちる。ここは全スキンド頂点が必ず通る場所なので効きが大きい。
FBZZ_SBUFFER_T(float4x4, gBoneMatrices, SB_SKIN_BONES_SLOT);
FBZZ_RWSBUFFER_T(SkinnedOutVertex, gOutVertices, UAV_SKINNED_VERTICES_SLOT);

cbuffer SkinningConstants : register(b0)
{
    uint gVertexCount;
    uint gBoneOffset;
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

    /// @note 4 影響ボーンの線形ブレンド。VS 版 (BlendSkinMatrix) と同じ式にすること。
    float4x4 skin =
          gBoneMatrices[gBoneOffset + v.boneIndices.x] * v.boneWeights.x
        + gBoneMatrices[gBoneOffset + v.boneIndices.y] * v.boneWeights.y
        + gBoneMatrices[gBoneOffset + v.boneIndices.z] * v.boneWeights.z
        + gBoneMatrices[gBoneOffset + v.boneIndices.w] * v.boneWeights.w;

    SkinnedOutVertex o;
    o.position = mul(float4(v.position, 1.0f), skin).xyz;
    /// @note 法線・接線は平行移動を含めない (w=0)。
    /// @note NOTE: 非等方スケールのボーンでは厳密には逆転置が要るが、スキンメッシュの
    /// @note ボーン行列は回転+平行移動が支配的なので VS 版と同じ近似に揃える。
    o.normal   = mul(float4(v.normal,  0.0f), skin).xyz;
    o.tangent  = mul(float4(v.tangent, 0.0f), skin).xyz;
    o.uv       = v.uv;
    /// @note SkinnedVertex は頂点カラーを持たない。静的メッシュの「色なし」と同じ白を書く。
    o.color    = float4(1.0f, 1.0f, 1.0f, 1.0f);

    gOutVertices[index] = o;
}
