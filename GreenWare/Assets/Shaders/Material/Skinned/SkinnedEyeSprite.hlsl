// FBZZ Engine
// Material/Skinned/SkinnedEyeSprite.hlsl | Material
// GPU スキニング + 目 (バイザー / 瞳) のスプライトを自発光パネルとして描く
//
// 用途:
//   キャラクターの目だけを別 submesh に分けて、そこへ表情スプライトを貼る。
//   スプライトの差し替え (瞬き・表情) はスクリプト側が t0 の albedo を
//   GameObject 単位で上書きして行う (ゲーム側スクリプトの役目)。
//   atlas 内の 1 コマを指した Sprite 参照なら、uvTiling / uvOffset へその矩形が
//   自動合成される (GeometryPassHelpers::ApplyAlbedoSpriteUv)。
//
// WHY 既存の Skinned シェーダーを使わず専用にするか:
//   1) 目は肌と同じ陰影則で塗ると成立しない。逆光や影の中で真っ暗になると
//      「目を閉じている」ようにしか見えず、瞬きの演出そのものが読めなくなる。
//      ここでは陰影の効き (shadeStrength) と最低輝度 (ambientFloor) を材質の
//      設定として持ち、暗所でも目が必ず読める状態を保証する。
//   2) 目の絵は基本的に描き込み済みの 2D 素材で、法線マップも金属度も持たない。
//      PBR ローブを通す意味がないうえ、Inspector に無関係なパラメーターが並ぶと
//      「どれを触れば目の見た目が変わるのか」が分からなくなる。
//   3) スプライトを貼る以上 uvTiling / uvOffset が必須。これを持たない
//      SkinnedUnlit では atlas の 1 コマを選べない。
//
// WHY 2 枚のテクスチャを同時に持ってクロスフェードしないか:
//   瞬きは 2〜3 フレームで閉じ切る速い動きで、実写でも中間状態はほぼ見えない。
//   混ぜると開いた目と閉じた目が重なった半透明の絵が一瞬出て、目が二重に見える。
//   切り替えは 1 枚差し替えの即時カットが正しい。おかげでテクスチャスロットも
//   標準の t0 だけで足り、共有 .mat のテクスチャ規約から外れずに済む。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SurfaceCommon.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

// 変数名と型がそのまま Inspector / .mat の [params] キーになる。
// albedo / emissiveColor / emissiveScale / uvTiling / uvOffset / alphaCutoff は
// エンジン標準レイアウトと同じ名前・同じオフセットに揃えてある。
// WHY: InitDefaultMaterialParams が名前で既定値を書き込むため、独自名にすると
//      「.mat に書いていない項目が 0 で始まる」という材質ごとの差が生まれる。
// NOTE: textureMask は必ず最後の 16-byte チャンクの先頭に置くこと。
//       Material::Upload() が自動計算して書き込む (.mat での手動設定は不要)。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;         // offset  0: 目の色。テクスチャがあれば乗算 tint として働く
    float3 emissiveColor;  // offset 16: 自発光の色。バイザーの発光色をここで決める
    float  emissiveScale;  // offset 28: 自発光の強さ。0 で消灯
    float  shadeStrength;  // offset 32: 陰影の効き [0,1]。0 = 完全に平坦 / 1 = 陰影あり
    float  ambientFloor;   // offset 36: 影の中でも保つ最低輝度 [0,1]。暗所で目が消えるのを防ぐ
    float  alphaCutoff;    // offset 40: これ未満の α を捨てる。0 なら切り抜きなし
    float  _pad0;          // offset 44
    float2 uvTiling;       // offset 48: UV スケール (Sprite 矩形が自動合成される)
    float2 uvOffset;       // offset 56: UV オフセット (同上)
    uint   textureMask;    // offset 64: テクスチャ存在フラグ (自動設定)
    float3 _pad1;          // offset 68

    // bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    // ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
};

FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);   // t0: 目のスプライト
FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);   // t8: シャドウマップ (自動バインド)
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

// 4 本のボーンを線形ブレンドしてスキン行列を作る。
// WHY: 他の Skinned シェーダーと完全に同一のロジックにしておく。ここだけ独自の
//      ブレンドにすると、顔と目が別々に変形して目が顔から外れる。
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
    // Sprite 矩形が合成済みの UV 変換。atlas の 1 コマだけをここで切り出す。
    float2 uv = SurfaceTransformUv(p.uv, uvTiling, uvOffset);

    float4 surfaceAlbedo = SurfaceSampleAlbedo(texAlbedo, sampDefault, uv,
                                               albedo, textureMask);
    float3 col   = surfaceAlbedo.rgb;
    float  alpha = surfaceAlbedo.a;
    // WHY しきい値 0 を分けるか: alphaCutoff = 0 のまま clip(alpha) を通すと、
    //     α がちょうど 0 のピクセルだけがドライバ依存で落ちる。「切り抜かない」を
    //     選んだ材質では clip 自体を通さず、挙動をバックエンド間で一致させる。
    if (alphaCutoff > 0.0f)
        clip(alpha - alphaCutoff);

    float3 N = SurfaceSafeNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    float3 L = SurfaceSafeNormalize(-lightDir, float3(0.0f, 1.0f, 0.0f));

    float shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                 lightViewProjection, shadowMapTexelSize,
                                 shadowBias, N, L);
    // NOTE: スキンドメッシュは GBuffer に描かれない (ExecuteGBufferPass が isSkinned を除外)。
    //       画面空間 AO / 接触影を引くと、キャラの画素で「背景の遮蔽」を読んでしまうので使わない。
    //       これは Deferred でも同じ (キャラは DeferredLighting を通らない) ため、差は生じない。

    // ハーフランバート。目は球面に近い形状なので素の N·L だと縁が黒く落ち、
    // 白目と背景の境界が潰れて表情が読めなくなる。
    //
    // WHY 放射照度 (lightIntensity・LIGHT_UNIT_SCALE) を通さないか:
    //   ここで欲しいのは明るさそのものではなく「元の絵をどこまで暗くしていいか」
    //   という 0〜1 の減光係数である。物理単位の強度を掛けると屋外の太陽
    //   (intensity > 1) で係数が 1 を超えて描き込み済みの絵が白飛びし、
    //   ambientFloor で守っているはずの下限と一緒に上限まで壊れる。
    //   目の見た目をシーンの明るさから切り離すのは、この材質の狙いそのもの。
    float3 shaded = lightColor * (saturate(dot(N, L) * 0.5f + 0.5f) * shadow);

    // 陰影の効き。shadeStrength = 0 なら完全に平坦な 2D として出る。
    float3 lighting = lerp(float3(1.0f, 1.0f, 1.0f), shaded, saturate(shadeStrength));
    // 影・夜・光源色のどれで暗くなった場合でも、ここが最終的な下限を保証する。
    lighting = max(lighting, saturate(ambientFloor).xxx);

    float3 result = col * lighting;

    // 点光源 / スポットライト — 走査元は clusterLightMode が決める
    // (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    // 目は自発光が主体なので、寄与は shadeStrength で絞った分だけ足す。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        float punctual = saturate(dot(N, ps.L) * 0.5f + 0.5f);
        result += col * ps.color * ps.intensity * punctual * saturate(shadeStrength);
    FBZZ_PUNCTUAL_END

    // 自発光。影も光源も掛けない。
    // WHY: バイザーや瞳のハイライトは「自分で光っている面」なので、影に入った
    //      瞬間に消えると発光しているように見えなくなる。ここが shadow を
    //      無視することで、暗所でも目の位置と表情が必ず読める。
    result += col * emissiveColor * emissiveScale;

    return float4(result, alpha);
}