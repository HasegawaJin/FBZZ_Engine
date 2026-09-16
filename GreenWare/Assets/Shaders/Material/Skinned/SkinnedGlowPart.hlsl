/// @file SkinnedGlowPart.hlsl
/// @brief キャラクターの発光パーツ (コア / リング / スリット) を 1 枚の材質で描く
/// @author Hasegawa Jin
/// @date 2026-08-25
///
/// 用途:
///   Mite / Serpent / Roller / ボス / プレイヤーの «光る部位» すべてがこの材質を共有する。
///   色と明るさは GameObject 単位の override で決まるので、材質は 1 つで足りる。
///     - 敵   : 部位を持つスクリプトが色と明滅を書く
///     - 味方 : GlowPartComponent が固定色を 1 度だけ書く (12.2 の緑)
///
/// WHY モデルごとに焼かれた M_E_Core_Plus / M_E_Ring_Plus をやめたか:
///   あれは FBX から機体ごとに焼き直される複製で、Mite / Serpent / Roller で別実体だった。
///   «光り方» を直すたびに 6 枚を同じ値に揃える作業になり、しかも名前に _Plus と入っている
///   とおり albedo に赤が焼き込まれていた。－極 (青) を乗せても下地の赤が残るため、
///   12.2 の「色として乗るのは極性の赤 / 青だけ」を材質側から破っていた。
///   下地を無彩色に戻し、色は override だけが決める、という形にすればこの矛盾が消える。
///
/// WHY 正面より «輪郭» を明るくするか (この材質の肝):
///   SkinnedPBR の emissive は面全体を一律に持ち上げるだけで、強くするほど広い面積が
///   同時に飽和へ向かう。12.2 が注記している「発光を強くしすぎると白飛びして赤と青の
///   区別がつかなくなる」はこれが原因で、色を保ったまま «強く光って見せる» ことができない。
///   フレネルで輪郭へ寄せると、白飛びしうる画素の面積が細い縁だけに絞られる。ブルームは
///   その細い縁を拾って大きく散るので、面の彩度を落とさずに «強い» 印象だけが増える。
///   結果として、正面から見た面は色が読める明るさに保たれ、シルエットは光って見える。
///
/// WHY 発光に影を掛けないか:
///   自分で光っている面が影に入った瞬間に消えると、発光体には見えない。12.4 は残り時間の
///   可視化を «演出ではなく仕様» と書いていて、影の位置によって極が読めなくなるのは困る。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SurfaceCommon.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

// 変数名と型がそのまま Inspector / .mat の [params] キーになる。
// albedo / emissiveColor / emissiveScale / uvTiling / uvOffset はエンジン標準レイアウトと
// 同じ名前に揃えてある (InitDefaultMaterialParams が名前で既定値を書き込むため)。
// NOTE: textureMask は必ず最後の 16-byte チャンクの先頭に置くこと。
//       Material::Upload() が自動計算して書き込む (.mat での手動設定は不要)。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;         // offset  0: 消灯時の下地。12.2 の「帯電していない = 無彩色の金属」
    float3 emissiveColor;  // offset 16: 発光色。GameObject 単位の override が本体
    float  emissiveScale;  // offset 28: 発光量。0 で消灯
    float  rimPower;       // offset 32: 輪郭の鋭さ [1,16]。大きいほど縁だけに寄る
    float  rimBoost;       // offset 36: 輪郭での発光倍率。ブルームへ渡す «強さ» はここ
    float  coreLevel;      // offset 40: 正面を向いた面の発光比。色が読める下限を保つ
    float  shadeStrength;  // offset 44: 消灯時の陰影の効き [0,1]。0 で完全に平坦
    float2 uvTiling;       // offset 48
    float2 uvOffset;       // offset 56
    uint   textureMask;    // offset 64: テクスチャ存在フラグ (自動設定)
    float3 _pad0;          // offset 68

    // bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    // ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
};

FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);   // t0: 任意。下地の模様
FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);   // t8: シャドウマップ (自動バインド)
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

// 消灯時のスペキュラの締まり。レンズ / 磨いた金属として振る舞わせる。
//
// WHY 材質パラメーターにしないか: 発光パーツで調整したいのは «光り方» であって、
//     消えている間のハイライトの広がりではない。ノブを増やすほど「どれを触れば
//     光り方が変わるのか」が読めなくなるので、見た目の主題でない値は畳んでおく。
static const float kLensRoughness = 0.18f;

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

PSInput VSMain(SkinnedVSInput v)
{
    PSInput o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float3 localN    = SurfaceSafeNormalize(mul(v.normal,  (float3x3)skin),
                                            float3(0.0f, 1.0f, 0.0f));
    float3 localT    = SurfaceSafeNormalize(mul(v.tangent, (float3x3)skin),
                                            float3(1.0f, 0.0f, 0.0f));
    float4 worldPos4 = mul(localPos, world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = SurfaceSafeNormalize(mul(localN, (float3x3)worldInvTranspose),
                                        float3(0.0f, 1.0f, 0.0f));
    o.tangent    = SurfaceSafeNormalize(mul(localT, (float3x3)world),
                                        float3(1.0f, 0.0f, 0.0f));
    o.uv         = v.uv;
    return o;
}

float4 PSMain(PSInput p) : SV_Target0
{
    float2 uv = SurfaceTransformUv(p.uv, uvTiling, uvOffset);
    float4 surfaceAlbedo = SurfaceSampleAlbedo(texAlbedo, sampDefault, uv,
                                               albedo, textureMask);
    float3 base = surfaceAlbedo.rgb;

    float3 N = SurfaceSafeNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    float3 V = SurfaceSafeNormalize(cameraPos - p.worldPos, float3(0.0f, 0.0f, 1.0f));
    float3 L = SurfaceSafeNormalize(-lightDir, float3(0.0f, 1.0f, 0.0f));

    float shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                 lightViewProjection, shadowMapTexelSize,
                                 shadowBias, N, L);
    // NOTE: スキンドメッシュは GBuffer に描かれない (ExecuteGBufferPass が isSkinned を除外)。
    //       画面空間 AO / 接触影を引くと、キャラの画素で「背景の遮蔽」を読んでしまうので使わない。
    //       これは Deferred でも同じ (キャラは DeferredLighting を通らない) ため、差は生じない。

    float3 lit = Lighting_BlinnPhong(N, V, L, base, kLensRoughness,
                                     lightColor, lightIntensity, shadow);

    // 点光源 / スポットライト — 走査元は clusterLightMode が決める
    // (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        lit += Lighting_BlinnPhong_Direct(N, V, ps.L, base, saturate(kLensRoughness + ps.roughnessBias),
                                          ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END

    // 陰影の効き。0 なら完全に平坦な下地として出る。
    float3 result = lerp(base, lit, saturate(shadeStrength));

    // フレネル近似。法線と視線が直交する «縁» ほど 1 に近づく。
    float NdotV = saturate(dot(N, V));
    float rim   = pow(1.0f - NdotV, max(rimPower, 1.0f));

    // 正面は coreLevel 倍 (色が読める明るさ)、縁は rimBoost ぶん上へ抜ける。
    // 抜けた分だけがブルームのしきい値を越えるので、光っているのは細い縁だけになる。
    float profile = max(coreLevel, 0.0f) + rim * max(rimBoost, 0.0f);
    result += emissiveColor * (max(emissiveScale, 0.0f) * profile);

    return float4(result, surfaceAlbedo.a);
}