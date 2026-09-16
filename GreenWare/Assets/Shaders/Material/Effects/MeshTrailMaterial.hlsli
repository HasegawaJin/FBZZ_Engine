/// @file    MeshTrailMaterial.hlsli
/// @brief   メッシュ残像シェーダーの共通契約。カスタムシェーダーは必ずこれを include する
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// WHY 契約をファイルにするか:
///   残像は «その時点のボーン姿勢でメッシュをもう一度描く» という定型が絵の内容と
///   無関係に必ず要る。Skinned は 4 本のボーン行列を重みで混ぜてから world を掛ける
///   ので、写経させると «出ない / 体が原点で潰れる» という、原因の見えない壊れ方を
///   する。定型は MeshTrailSkinnedVS() / MeshTrailStaticVS() に閉じ、材質側は
///   PSMain だけを書く。
///
/// WHY 材質パラメータを MaterialConstants (b2) に置くか:
///   BuildDescriptor() は cbuffer を **名前** で探す
///   (GetConstantBufferByName("MaterialConstants"))。この名前とレジスタに合わせて
///   おくだけで、既存の .mat / ShaderDescriptor / Inspector がそのまま残像にも効く。
///   エンジンが埋める «この 1 枚の色» は CB_MESHTRAIL (b6) へ逃がしてある。
///
/// WHY 色を材質だけで決めないか:
///   帯の «古いほど薄い» は 1 枚ごとに違う値で、材質は 1 つしかない。
///   MeshTrailComponent の colorStart → colorEnd を距離で配った結果が trailColor
///   として毎ドロー来るので、材質はそれを起点に «形» を作る。
///
/// 使い方 (最小):
/// @code
///   #define FBZZ_MESHTRAIL_SKINNED          // Skinned へ張るなら include より前に
///   #include "Material/Effects/MeshTrailMaterial.hlsli"
///
///   cbuffer MaterialConstants : register(CB_MATERIAL)
///   {
///       float4 rimColor;    // .mat の [params] と **名前** で結ばれる
///       float  rimSharpness;
///   };
///
///   // VSMain はこのヘッダーが供給する。書くのは PSMain だけ。
///   float4 PSMain(MeshTrailPSIn p) : SV_Target0
///   {
///       return gMeshTrailTex.Sample(gSampler, p.uv) * trailColor;
///   }
/// @endcode
///
/// オプション (include より前に定義する):
///   FBZZ_MESHTRAIL_SKINNED    … Skinned 用 VS を供給する (既定は静的メッシュ用)。
///                               .mat の mesh_type = 'skinned' と揃えること。
///   FBZZ_MESHTRAIL_CUSTOM_VS  … 既定の VSMain を供給しない。自分で書く場合に定義する。
///
/// 使えるテクスチャスロット:
///   t0 = .mat の [textures] albedo (このヘッダーが gMeshTrailTex として宣言済み)。
///   残像パスは他のスロットを 1 つも束縛しないので、t1 以降は空いている。
#ifndef FBZZ_MESH_TRAIL_MATERIAL_HLSLI
#define FBZZ_MESH_TRAIL_MATERIAL_HLSLI

// b2 は材質へ明け渡す。Constants.hlsli の既定 MaterialConstants を出させないための
// 宣言で、ParticleCommon.hlsli と同じ作法 (Binding.hlsli の CB_MESHTRAIL 参照)。
#ifndef FBZZ_MATERIAL_CONSTANTS
#define FBZZ_MATERIAL_CONSTANTS
#endif

#include "Common/Binding.hlsli"

cbuffer MeshTrailConstants : register(CB_MESHTRAIL)
{
    // この 1 枚の色。エンジンが colorStart → colorEnd を «古さ» で配った結果。
    float4 trailColor;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    gMeshTrailTex : register(TEX_ALBEDO);
SamplerState gSampler      : register(SAMPLER_DEFAULT);

// WHY 法線と world 座標まで渡すか:
//   残像を «輪郭» として扱う材質 (フレネル) は、板 1 枚では作れない。視線と面の
//   向きが要る。塗り潰すだけの材質は読まなければ補間器 2 本ぶん無駄になるだけで、
//   契約を 2 種類に割るほうが後で高く付く。
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

float4x4 MeshTrailBlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

MeshTrailPSIn MeshTrailSkinnedVS(SkinnedVSInput v)
{
    MeshTrailPSIn o;
    float4x4 skin = MeshTrailBlendSkinMatrix(v);
    float4 localPos = mul(float4(v.position, 1.0f), skin);
    float3 localN   = mul(v.normal, (float3x3)skin);
    float4 worldPos = mul(localPos, world);
    o.svPosition  = mul(worldPos, viewProjection);
    // 接線を «ほぼ 0» で足して参照を残す。旧 SkinnedMeshTrail.hlsl から引き継いだ
    // 予防で、TANGENT が落ちて入力レイアウトがずれると UV だけが別の場所を指す。
    o.uv          = v.uv + v.tangent.xy * 1e-8f;
    o.worldPos    = worldPos.xyz;
    // ボーンの姿勢は残像サンプルごとに固定されているので、法線もその時点の向き。
    // worldInvTranspose は «今» の姿勢なので使わない ─ 残像だけ光り方が現在の
    // ポーズに追従して、形と陰が食い違う。
    o.worldNormal = mul(localN, (float3x3)world);
    return o;
}

#ifndef FBZZ_MESHTRAIL_CUSTOM_VS
#ifdef FBZZ_MESHTRAIL_SKINNED
MeshTrailPSIn VSMain(SkinnedVSInput v) { return MeshTrailSkinnedVS(v); }
#else
MeshTrailPSIn VSMain(VSInput v) { return MeshTrailStaticVS(v); }
#endif
#endif // FBZZ_MESHTRAIL_CUSTOM_VS

#endif // FBZZ_MESH_TRAIL_MATERIAL_HLSLI
