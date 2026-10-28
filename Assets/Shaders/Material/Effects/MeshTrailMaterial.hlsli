/// @file    MeshTrailMaterial.hlsli
/// @brief   メッシュ残像シェーダーの共通契約。カスタムシェーダーは必ずこれを include する
/// @author  Hasegawa Jin
/// @date    2026-09-08

/// @note 契約をファイルにするか:
/// @note 残像は «その時点のボーン姿勢でメッシュをもう一度描く» という定型が絵の内容と
/// @note 無関係に必ず要る。Skinned は 4 本のボーン行列を重みで混ぜてから world を掛ける
/// @note ので、写経させると «出ない / 体が原点で潰れる» という、原因の見えない壊れ方を
/// @note する。定型は MeshTrailSkinnedVS() / MeshTrailStaticVS() に閉じ、材質側は
/// @note PSMain だけを書く。

/// @note 材質パラメータを MaterialConstants (b2) に置くか:
/// @note BuildDescriptor() は cbuffer を **名前** で探す
/// @note (GetConstantBufferByName("MaterialConstants"))。この名前とレジスタに合わせて
/// @note おくだけで、既存の .mat / ShaderDescriptor / Inspector がそのまま残像にも効く。
/// @note エンジンが埋める «この 1 枚の色» は CB_MESHTRAIL (b6) へ逃がしてある。

/// @note 色を材質だけで決めないか:
/// @note 帯の «古いほど薄い» は 1 枚ごとに違う値で、材質は 1 つしかない。
/// @note MeshTrailComponent の colorStart → colorEnd を距離で配った結果が trailColor
/// @note として毎ドロー来るので、材質はそれを起点に «形» を作る。

/// @note 使い方 (最小):
/// @code
/// @note #define FBZZ_MESHTRAIL_SKINNED          // Skinned へ張るなら include より前に
/// @note #include "Material/Effects/MeshTrailMaterial.hlsli"

/// @note cbuffer MaterialConstants : register(CB_MATERIAL)
/// @note {
/// @note float4 rimColor;    // .mat の [params] と **名前** で結ばれる
/// @note float  rimSharpness;
/// @note };

/// @note // VSMain はこのヘッダーが供給する。書くのは PSMain だけ。
/// @note float4 PSMain(MeshTrailPSIn p) : SV_Target0
/// @note {
/// @note return gMeshTrailTex.Sample(gSampler, p.uv) * trailColor;
/// @note }
/// @endcode

/// @note オプション (include より前に定義する):
/// @note FBZZ_MESHTRAIL_SKINNED    … Skinned 用 VS を供給する (既定は静的メッシュ用)。
/// @note .mat の mesh_type = 'skinned' と揃えること。
/// @note FBZZ_MESHTRAIL_CUSTOM_VS  … 既定の VSMain を供給しない。自分で書く場合に定義する。

/// @note 使えるテクスチャスロット:
/// @note t0 = .mat の [textures] albedo (このヘッダーが gMeshTrailTex として宣言済み)。
/// @note 残像パスは他のスロットを 1 つも束縛しないので、t1 以降は空いている。
#ifndef FBZZ_MESH_TRAIL_MATERIAL_HLSLI
#define FBZZ_MESH_TRAIL_MATERIAL_HLSLI

/// @note b2 は材質へ明け渡す。Constants.hlsli の既定 MaterialConstants を出させないための
/// @note 宣言で、ParticleCommon.hlsli と同じ作法 (Binding.hlsli の CB_MESHTRAIL 参照)。
#ifndef FBZZ_MATERIAL_CONSTANTS
#define FBZZ_MATERIAL_CONSTANTS
#endif

#include "Common/Binding.hlsli"

cbuffer MeshTrailConstants : register(CB_MESHTRAIL)
{
    /// @note この 1 枚の色。エンジンが colorStart → colorEnd を «古さ» で配った結果。
    float4 trailColor;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Skinning.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

/// @note 添字は MaterialConstants ではなく描画ごとの添字ブロック (b14) の t0 枠から引く。
/// @note C++ 側は DrawCall::textures[0] で差す。既定の .mat は MaterialConstants を持たない。
FBZZ_TEX2D(gMeshTrailTex, TEX_ALBEDO_SLOT);
SamplerState gSampler      : register(SAMPLER_DEFAULT);

/// @note 法線と world 座標まで渡すか:
/// @note 残像を «輪郭» として扱う材質 (フレネル) は、板 1 枚では作れない。視線と面の
/// @note 向きが要る。塗り潰すだけの材質は読まなければ補間器 2 本ぶん無駄になるだけで、
/// @note 契約を 2 種類に割るほうが後で高く付く。
struct MeshTrailPSIn
{
    float4 svPosition  : SV_POSITION;
    float2 uv          : TEXCOORD0;
    float3 worldPos    : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
};

MeshTrailPSIn MeshTrailStaticVS(VSInput v)
{
    MeshTrailPSIn o;
    float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition  = mul(worldPos, viewProjection);
    o.uv          = v.uv;
    o.worldPos    = worldPos.xyz;
    o.worldNormal = mul(v.normal, (float3x3)worldInvTranspose);
    return o;
}



MeshTrailPSIn MeshTrailSkinnedVS(SkinnedVSInput v)
{
    MeshTrailPSIn o;
    float4x4 skin = BlendSkinMatrix(v);
    float4 localPos = mul(float4(v.position, 1.0f), skin);
    float3 localN   = mul(v.normal, (float3x3)skin);
    float4 worldPos = mul(localPos, world);
    o.svPosition  = mul(worldPos, viewProjection);
    /// @note 接線を «ほぼ 0» で足して参照を残す。旧 SkinnedMeshTrail.hlsl から引き継いだ
    /// @note 予防で、TANGENT が落ちて入力レイアウトがずれると UV だけが別の場所を指す。
    o.uv          = v.uv + v.tangent.xy * 1e-8f;
    o.worldPos    = worldPos.xyz;
    /// @note ボーンの姿勢は残像サンプルごとに固定されているので、法線もその時点の向き。
    /// @note worldInvTranspose は «今» の姿勢なので使わない ─ 残像だけ光り方が現在の
    /// @note ポーズに追従して、形と陰が食い違う。
    o.worldNormal = mul(localN, (float3x3)world);
    return o;
}

#ifndef FBZZ_MESHTRAIL_CUSTOM_VS
#ifdef FBZZ_MESHTRAIL_SKINNED
MeshTrailPSIn VSMain(SkinnedVSInput v) { return MeshTrailSkinnedVS(v); }
#else
MeshTrailPSIn VSMain(VSInput v) { return MeshTrailStaticVS(v); }
#endif
#endif /// @note FBZZ_MESHTRAIL_CUSTOM_VS

#endif /// @note FBZZ_MESH_TRAIL_MATERIAL_HLSLI