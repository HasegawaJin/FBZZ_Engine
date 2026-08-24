// FBZZ Engine
// Rendering/ParticleCommon.hlsli
// パーティクル描画シェーダーが共有する定数・頂点レイアウト・ビルボード展開
//
// WHY: effectsFlags のビット・b11 の cbuffer・ParticleVSIn・ビルボード展開は、
//      以前 Particle.hlsl / ParticleGPU.hlsl / ParticleGpuMesh.hlsl /
//      ParticleSelfShadowDensity.hlsl / ParticleOverdraw.hlsl /
//      SelectionMaskParticle.hlsl / SelectionMaskParticleGPU.hlsl の
//      7 ファイルへ別々に書き写されていた。29 個の cbuffer メンバーを 1 つ足すたびに
//      7 か所 × 4 アセットツリー = 28 か所を見比べることになり、片方だけ直すと
//      「CPU では効くが GPU では効かない」形で静かに壊れる。定義をここへ集約する。
//      C++ 側は GeometryPasses.hpp の ParticleRenderCB / ParticleVertex が正本。

#ifndef FBZZ_PARTICLE_COMMON_INCLUDED
#define FBZZ_PARTICLE_COMMON_INCLUDED

// パーティクルは b2 を材質へ明け渡すため、Constants.hlsli の汎用 PBR MaterialConstants は
// 使わない。材質パラメータを持つシェーダーは自分で
// `cbuffer MaterialConstants : register(CB_MATERIAL)` を宣言する。
//
// NOTE: そのため **このヘッダーは Common/Constants.hlsli より先に include すること**。
//       逆順にすると Constants.hlsli のインクルードガードが先に立ち、この定義が間に合わない。
#ifndef FBZZ_MATERIAL_CONSTANTS
#define FBZZ_MATERIAL_CONSTANTS
#endif

#include "Common/Binding.hlsli"
#include "Common/Color.hlsli"
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Mask.hlsli"
#include "Rendering/ParticleNoise.hlsli"

// ── effectsFlags のビット割り当て ──
#define FBZZ_PFX_DISTORTION     1u   // 背景を屈折させる (熱歪み)
#define FBZZ_PFX_SIX_WAY        2u   // 疑似法線による lit smoke
#define FBZZ_PFX_MOTION_VECTOR  4u   // フリップブックの motion vector ブレンド
#define FBZZ_PFX_RECEIVE_SHADOW 8u   // シャドウマップを受ける
#define FBZZ_PFX_VOLUMETRIC     16u  // ビルボード内レイマーチ
// 事前乗算アルファで描かれている。RGB が既に alpha 込みの値であることを示す。
// WHY: ソフトパーティクルの fade を alpha だけに掛けると RGB が減らず白い縁が残る。
//      PS 側で RGB にも同じ係数を掛けるために要る。
#define FBZZ_PFX_PREMULTIPLIED  32u
// albedo テクスチャが sRGB でエンコードされている (.meta の srgb)。
// WHY: このエンジンは _SRGB フォーマットの SRV を作らず、「シェーダーが自分で
//      SRGBToLinear する」規約で統一されている (PBR.hlsl / GBuffer.hlsl / Terrain 等)。
//      素材はすべて sRGB とは限らない (ProceduralVFXTextures はリニアで焼く) ので
//      フラグで切り替える。
#define FBZZ_PFX_SRGB_TEXTURE   64u
// 歪み専用ノーマルマップ (t1) がバインドされている。無い場合は albedo の RG を使う。
#define FBZZ_PFX_DISTORTION_MAP 128u

// アルファの取り出し方は effectsFlags の bit8-10 (3 ビット) に格納する。
// 値は Rendering/Mask.hlsli の FBZZ_MASK_* をそのまま使う。
// WHY: パーティクルだけ別の番号体系にすると、同じ「黒背景素材」の指定が
//      マテリアルとパーティクルで違う値になり、両方を触る人が必ず取り違える。
#define FBZZ_PALPHA_SHIFT 8u
#define FBZZ_PALPHA_MASK  7u

// ── エンジンが毎ドロー埋める定数 ──
//
// LAYOUT: Engine/Scene/Systems/RenderPasses/Geometry/GeometryPasses.hpp の
//         ParticleRenderCB と 1 バイトも違わずに一致させること
//         (static_assert(sizeof(ParticleRenderCB) == 128) がある)。
//
// WHY b2 (CB_MATERIAL) ではないか:
//   シェーダーリフレクションは cbuffer 名 "MaterialConstants" を b2 に探す
//   (DX11Shader/DX12Shader::BuildDescriptor)。ここを占有すると .mat の [params] を
//   1 つも束縛できず、パーティクルだけマテリアルを持てない例外になる。
//   b2 は材質へ明け渡し、エンジンの値は CB_PARTICLE (b11) へ逃がす。
//   Decal (CB_DECAL) / UI がすでに同じ判断をしている。
cbuffer ParticleRenderConstants : register(CB_PARTICLE)
{
    uint  gRenderMode;
    float gStretchedVelocityScale;
    float gStretchedLengthScale;
    float gSoftParticleFadeDistance;
    uint  gSoftParticles;
    uint  gMaxParticles;
    uint  gEffectsFlags;
    float gDistortionStrength;
    float gLightingStrength;
    float gEmissiveScale;
    float gMotionVectorStrength;
    // 描画先の解像度。PostProcConstants の screenSize はパーティクル描画では
    // バインドされないため、歪みの画面 UV はこちらを使う。
    float gScreenWidth;
    float gScreenHeight;
    // ビルボードの軸ごとサイズ倍率 (縦に伸びる炎・平たい衝撃波などの非等方形状用)
    float gSizeAxisScaleX;
    float gSizeAxisScaleY;
    // 受け影の強さ [0,1]。0 で無効 (影サンプリング自体をスキップ)。
    float gShadowStrength;
    // ボリュメトリック煙 (gEffectsFlags bit4)
    uint  gVolumetricSteps;
    float gVolumetricDensity;
    float gVolumetricAnisotropy;
    float gVolumetricNoiseScale;
    // GPU 経路 (ParticleGPU.hlsl) 専用。
    uint  gGpuSortEnabled;
    // 自己影の消衰係数。0 で無効。密度バッファ (t9) は Particle パスが用意する。
    float gSelfShadowStrength;
    // 煙の散乱 (FBZZ_PFX_SIX_WAY 有効時)。巻き込み拡散と逆光透過。
    float gSmokeWrap;
    float gSmokeTransmission;
    // float4 はレジスタを跨げないため、ここまでで 16 バイト境界 (offset 96) に揃えてある。
    // 順序を入れ替えると C++ の ParticleRenderCB と黙ってずれる。
    float4 gTintColor;             // .mat の [params] albedo (リニア済み)
    float gSmokeBackScatterPower;
    float gDistortionChromatic;    // 歪みの色収差量 [画面 UV]
    float gParticlePad1;
    float gParticlePad2;
};

// ── 頂点入力 ──
// LAYOUT: GeometryPasses.hpp の ParticleVertex が正本 (static_assert(sizeof == 92))。
// カスタムシェーダーは semantics / index / 順序をここと完全に一致させること。
struct ParticleVSIn
{
    float3 center     : POSITION;   // ワールド空間パーティクル中心
    float2 uv         : TEXCOORD0;  // クワッドコーナー UV [0,1]
    float4 color      : COLOR;      // RGBA (alpha = フェード乗数)
    float  size       : TEXCOORD1;  // ビルボードの一辺サイズ (ワールド単位)
    float  rotation   : TEXCOORD2;
    float4 uvRect     : TEXCOORD3;  // xy=min, zw=max
    float3 velocity   : TEXCOORD4;
    float4 nextUvRect : TEXCOORD5;
    float  spriteBlend : TEXCOORD6;
};

// ── VS → PS ──
struct ParticlePSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float2 localUv    : TEXCOORD1;
    float2 nextUv     : TEXCOORD2;
    float  spriteBlend : TEXCOORD3;
    // 受け影のシャドウマップ投影に使うワールド座標。
    float3 worldPos   : TEXCOORD4;
    // ボリュメトリック煙のレイマーチ用: 粒子中心と半径 (ワールド単位)。
    float3 center     : TEXCOORD5;
    float  radius     : TEXCOORD6;
    float4 color      : COLOR;
};

// ── ビルボード展開 ──
// CPU シミュレーションが積んだ頂点を、カメラ (自己影パスでは光源) へ正対する板へ広げる。
//
// WHY 1 本にまとめるか: 通常描画・自己影・オーバードロー計測・選択マスクは
//     «同じ形» を描かないと意味を持たない。マスクが 1px でもずれれば輪郭が本体から
//     浮き、計測がずれれば «実際とは違う形» のコストを測ることになる。
ParticlePSIn ParticleBillboardVS(ParticleVSIn v)
{
    // row-major view 行列の列 0, 1 = カメラ空間 X / Y 軸のワールド向き
    float3 right = float3(view[0][0], view[1][0], view[2][0]);
    float3 up    = float3(view[0][1], view[1][1], view[2][1]);
    float lengthScale = 1.0f;
    if (gRenderMode == 1)
    {
        float speed = length(v.velocity);
        if (speed > 1.0e-4f)
        {
            up = v.velocity / speed;
            float3 viewDir = normalize(cameraPos - v.center);
            right = normalize(cross(up, viewDir));
            lengthScale = max(gStretchedLengthScale + speed * gStretchedVelocityScale, 0.0f);
        }
    }
    else if (gRenderMode == 2)
    {
        right = float3(1.0f, 0.0f, 0.0f);
        up = float3(0.0f, 0.0f, 1.0f);
    }
    else if (gRenderMode == 3)
    {
        up = float3(0.0f, 1.0f, 0.0f);
        float3 viewDir = normalize(cameraPos - v.center);
        right = normalize(cross(up, viewDir));
    }

    // UV [0,1] → corner オフセット [-1, +1]。
    // WHY y を反転するか: uvRect.y はスプライト行の «上端» で、テクスチャの V は下向きに増える。
    //     つまり uv.y = 0 は絵の上端なので、画面でも up の正側 (上) へ置かなければならない。
    //     反転を入れないと CPU 経路だけスプライトが上下逆に貼られ、GPU 経路 (QUAD_CORNERS が
    //     +Y から始まる) と絵が食い違う。方向を持つ素材とフリップブックで実害が出る。
    float2 corner   = float2(v.uv.x * 2.0f - 1.0f, 1.0f - v.uv.y * 2.0f);
    float  s = sin(v.rotation);
    float  c = cos(v.rotation);
    corner = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    // 軸ごとの倍率は回転の後に掛ける。先に掛けると回転で縦横比が混ざり、
    // 「回しても細長いまま」という直感的な挙動にならない。
    float3 worldPos = v.center
                    + right * corner.x * v.size * 0.5f * gSizeAxisScaleX
                    + up    * corner.y * v.size * 0.5f * gSizeAxisScaleY * lengthScale;

    ParticlePSIn o;
    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv         = lerp(v.uvRect.xy, v.uvRect.zw, v.uv);
    o.localUv    = v.uv;
    o.nextUv     = lerp(v.nextUvRect.xy, v.nextUvRect.zw, v.uv);
    o.spriteBlend = v.spriteBlend;
    o.worldPos   = worldPos;
    o.center     = v.center;
    // 非等方スケールが掛かっていても球として扱うため、大きい方の半径を使う
    // (小さい方に合わせると縁が矩形からはみ出して切れて見える)。
    o.radius     = v.size * 0.5f * max(gSizeAxisScaleX, gSizeAxisScaleY);
    o.color      = v.color;
    return o;
}

// パーティクル素材からアルファを取り出す。RGB は色としてそのまま残す。
// (輝度をアルファにする素材でも、RGB は炎や煙の色として意味を持つため)
float4 ResolveParticleTexel(float4 texel, uint effectsFlags)
{
    const uint channel = (effectsFlags >> FBZZ_PALPHA_SHIFT) & FBZZ_PALPHA_MASK;
    // 通常のアルファ素材は無加工で返す (saturate も掛けず、既存の見た目を厳密に保つ)。
    if (channel == FBZZ_MASK_ALPHA) return texel;
    return ApplyMaskToAlpha(texel, channel);
}

// アルファ解決に加えて RGB をリニアへ揃える。描画に使う色は必ずこちらを通すこと。
// NOTE: アルファ抽出は「エンコードされたまま」の値で行う。輝度をアルファに使う素材は
//       見た目の明るさ (= sRGB 値) を基準に作られているため、先にリニア化すると
//       中間調のアルファだけが落ちて抜けが変わってしまう。
float4 ResolveParticleAlbedo(float4 texel, uint effectsFlags)
{
    float4 resolved = ResolveParticleTexel(texel, effectsFlags);
    if ((effectsFlags & FBZZ_PFX_SRGB_TEXTURE) != 0u) resolved.rgb = SRGBToLinear(resolved.rgb);
    return resolved;
}

// ── ボリュメトリック煙 ──
// ビルボードの矩形内で、粒子中心の球状密度場をレイマーチするための道具。
// WHY: 板ポリゴンにテクスチャを貼るだけでは、カメラが回り込んだときに
//      「紙が回った」ように見える。視線方向へ積分すると厚みが出て、
//      逆光での前方散乱 (縁が光る) も表現できる。
//      CPU 経路と GPU 経路で式が 1 文字でも違うと «モードを変えると煙が変わる» ので共有する。

// Henyey-Greenstein 位相関数。g>0 で前方散乱が強くなる。
float HenyeyGreenstein(float cosTheta, float g)
{
    float gg = g * g;
    float denom = 1.0f + gg - 2.0f * g * cosTheta;
    return (1.0f - gg) / (4.0f * 3.14159265f * max(pow(abs(denom), 1.5f), 1.0e-4f));
}

// 球内の密度。中心ほど濃く、外周でゼロへ落ちる。ノイズで塊感を与える。
float VolumetricDensityAt(float3 samplePos, float3 center, float radius)
{
    float3 offset = (samplePos - center) / max(radius, 1.0e-4f);
    float  r = length(offset);
    if (r >= 1.0f) return 0.0f;
    // 外周へ向かって滑らかに 0 へ。二乗で中心に密度を寄せる。
    float falloff = 1.0f - r;
    falloff *= falloff;
    // ノイズはワールド座標基準。粒子が動いても模様が張り付いて見えないよう
    // 中心からの相対位置ではなくワールド位置でサンプルする。
    float noise = FbmNoise3D(samplePos * gVolumetricNoiseScale, 3);
    return saturate(falloff * (0.6f + 0.8f * noise));
}

// ── 煙の散乱 ──
// 巻き込み拡散: 光を透かす媒質では明暗の境界が N·L=0 で切れない。
// WHY: 素の saturate(N·L) は不透明な球の陰影で、煙に使うと陰側が硬く真っ黒に落ちる。
float ParticleWrappedDiffuse(float ndotl, float wrap)
{
    return saturate((ndotl + wrap) / (1.0f + wrap));
}

// 前方散乱による逆光透過。視線と光の向きが揃うほど強い。
// WHY: Mie 散乱の位相関数は前方に鋭く尖る。煙が「向こう側の光で縁から光る」のはこれで、
//      この項が無いと炎が煙の背後にあっても煙は暗いままになる。
float ParticleBackScatter(float3 viewDir, float3 lightDirection, float power, float strength)
{
    if (strength <= 0.0f) return 0.0f;
    return pow(saturate(dot(-viewDir, lightDirection)), max(power, 0.1f)) * strength;
}

#endif // FBZZ_PARTICLE_COMMON_INCLUDED
