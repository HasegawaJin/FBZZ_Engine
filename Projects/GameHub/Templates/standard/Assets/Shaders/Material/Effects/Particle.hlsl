// FBZZ Engine
// Material/Effects/Particle.hlsl | Material
// CPU パーティクル用ビルボードシェーダー
// PSO: SOLID_NOCULL + ADDITIVE/ALPHA_BLEND + DEPTH_READ

#include "Common/Binding.hlsli"
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/ParticleCommon.hlsli"
#include "Rendering/ParticleNoise.hlsli"
#include "Rendering/ParticleSelfShadow.hlsli"
#include "Rendering/Shadow.hlsli"

Texture2D    gParticleTex : register(TEX_ALBEDO);
// 歪みベクトル専用ノーマルマップ。gEffectsFlags の FBZZ_PFX_DISTORTION_MAP で有効判定する。
Texture2D    gDistortionTex : register(TEX_NORMAL);
Texture2D    gSceneDepth  : register(TEX_DEPTH);
Texture2D    gSceneColor  : register(t5);
Texture2D    gMotionVectors : register(t6);
// 自己影の光源側密度 (R=Σα, G=Σα·深度)。ParticleSelfShadowDensity.hlsl が書く。
Texture2D    gParticleDensity : register(TEX_PARTICLE_DENSITY);
// 受け影用。Surface/Skinned マテリアルと同じスロット・同じ ComputeShadow を使う。
Texture2D<float>       gShadowMap  : register(TEX_SHADOW);
SamplerState           gSampler    : register(SAMPLER_DEFAULT);
SamplerComparisonState gSampShadow : register(SAMPLER_SHADOW);

cbuffer ParticleRenderConstants : register(CB_MATERIAL)
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
    // バインドされないため、歪みの画面UVはこちらを使う。
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
    // GPU 経路 (ParticleGPU.hlsl) 専用。CPU 経路では読まないが、同じ b2 を共有するため
    // ParticleRenderCB のレイアウトを 1 バイトもずらさないよう必ず宣言を揃える。
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

struct ParticleVSIn
{
    float3 center : POSITION;   // ワールド空間パーティクル中心
    float2 uv     : TEXCOORD0;  // クワッドコーナー UV [0,1]
    float4 color  : COLOR;      // RGBA (alpha = フェード乗数)
    float  size   : TEXCOORD1;  // ビルボードの一辺サイズ (ワールド単位)
    float  rotation : TEXCOORD2;
    float4 uvRect   : TEXCOORD3; // xy=min, zw=max
    float3 velocity : TEXCOORD4;
    float4 nextUvRect : TEXCOORD5;
    float spriteBlend : TEXCOORD6;
};

struct ParticlePSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float2 localUv    : TEXCOORD1;
    float2 nextUv     : TEXCOORD2;
    float spriteBlend : TEXCOORD3;
    // 受け影のシャドウマップ投影に使うワールド座標。
    float3 worldPos   : TEXCOORD4;
    // ボリュメトリック煙のレイマーチ用: 粒子中心と半径 (ワールド単位)。
    float3 center     : TEXCOORD5;
    float  radius     : TEXCOORD6;
    float4 color      : COLOR;
};

// ---------- ボリュメトリック煙 -------------------------------------------
// ビルボードの矩形内で、粒子中心の球状密度場をレイマーチする。
// WHY: 板ポリゴンにテクスチャを貼るだけでは、カメラが回り込んだときに
//      「紙が回った」ように見える。視線方向へ積分すると厚みが出て、
//      逆光での前方散乱 (縁が光る) も表現できる。
//      専用パスを増やさず PS 内で完結させることで、既存の VB/PSO/ブレンド・
//      ソート・受け影の仕組みをそのまま流用している。

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

ParticlePSIn VSMain(ParticleVSIn v)
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

    // UV [0,1] → corner オフセット [-1, +1]
    float2 corner   = v.uv * 2.0f - 1.0f;
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

float4 PSMain(ParticlePSIn p) : SV_Target0
{
    // 中心から外側にかけてソフトフェード (加算合成なので alpha で輝度調整)
    float2 d    = p.localUv * 2.0f - 1.0f;
    float  fade = RadialMask(p.localUv);

    // ── ボリュメトリック煙 ──
    // テクスチャではなく密度場の積分で色と不透明度を決めるため、
    // 以降のテクスチャ/フリップブック処理とは排他で早期リターンする。
    if ((gEffectsFlags & FBZZ_PFX_VOLUMETRIC) != 0u)
    {
        float discSq = dot(d, d);
        if (discSq >= 1.0f) discard;   // 球の外は描かない (矩形の角を消す)

        float3 viewDir = normalize(cameraPos - p.center);
        // ビュー行列の列 0,1 がカメラ空間 X/Y 軸のワールド向き (VS と同じ取り出し方)
        float3 right = float3(view[0][0], view[1][0], view[2][0]);
        float3 up    = float3(view[0][1], view[1][1], view[2][1]);

        // 視線が球を貫く長さの半分。ディスク中心ほど厚い。
        float halfChord = sqrt(saturate(1.0f - discSq)) * p.radius;
        float3 entry = p.center
                     + right * (d.x * p.radius)
                     + up    * (d.y * p.radius)
                     + viewDir * halfChord;

        uint  steps = max(gVolumetricSteps, 1u);
        float stepLength = (2.0f * halfChord) / (float)steps;
        float3 lightDirection = normalize(-lightDir);
        float  phase = HenyeyGreenstein(dot(-viewDir, lightDirection), gVolumetricAnisotropy);

        float3 scattered = 0.0f;
        float  transmittance = 1.0f;
        [loop] for (uint s = 0; s < steps; ++s)
        {
            // ステップ中央でサンプルすると、粗いステップ数でも縞が出にくい。
            float3 samplePos = entry - viewDir * (((float)s + 0.5f) * stepLength);
            float density = VolumetricDensityAt(samplePos, p.center, p.radius);
            if (density <= 0.0f) continue;

            // 自己遮蔽: ライト方向へ数ステップだけ密度を積んで内部の陰影を作る。
            float shadowDensity = 0.0f;
            [unroll] for (int ls = 1; ls <= 2; ++ls)
            {
                float3 lightSample = samplePos + lightDirection * (p.radius * 0.4f * (float)ls);
                shadowDensity += VolumetricDensityAt(lightSample, p.center, p.radius);
            }
            float lightTransmittance = exp(-shadowDensity * gVolumetricDensity);

            // シャドウマップの影も掛ける (周囲の遮蔽物が落とす影)。
            float mapShadow = 1.0f;
            if ((gEffectsFlags & FBZZ_PFX_RECEIVE_SHADOW) != 0u)
            {
                mapShadow = ComputeShadow(gShadowMap, gSampShadow, samplePos,
                                          lightViewProjection, shadowMapTexelSize, shadowBias,
                                          lightDirection, lightDirection);
                mapShadow = lerp(1.0f, mapShadow, saturate(gShadowStrength));
            }
            // 雲全体の自己影をステップごとに掛ける。
            // WHY: 上の自己遮蔽は「この粒子 1 個の球内部」しか見ないため、
            //      複数の粒子が作る大きな塊の中で光が減っていく様子が出ない。
            //      光源側密度をステップ位置で引くと、雲を貫く光の筋 (光の柱) が
            //      ボリューム内部に現れる。ここが「ライトとの相互作用」の本体。
            mapShadow *= ComputeParticleSelfShadowFromMap(
                gParticleDensity, gSampler, samplePos, lightViewProjection, gSelfShadowStrength);

            float extinction = density * gVolumetricDensity * stepLength;
            float stepTransmittance = exp(-extinction);
            float3 inScatter = (ambientColor
                + lightColor * (phase * lightTransmittance * mapShadow))
                * p.color.rgb * gTintColor.rgb;
            // エネルギー保存に沿った積分 (解析的な 1 ステップ積分)
            scattered += transmittance * (1.0f - stepTransmittance) * inScatter;
            transmittance *= stepTransmittance;
            if (transmittance < 0.01f) break;   // ほぼ不透明になったら打ち切る
        }

        float alpha = (1.0f - transmittance) * p.color.a;
        if (gSoftParticles != 0)
        {
            float sceneDepth = gSceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r;
            float sceneLinear = LinearizeDepth(sceneDepth, nearZ, farZ);
            float particleLinear = LinearizeDepth(p.svPosition.z, nearZ, farZ);
            alpha *= saturate((sceneLinear - particleLinear) / gSoftParticleFadeDistance);
        }
        // 散乱光は既に alpha で重み付けされているため、事前乗算アルファとして出す。
        return float4(scattered * gEmissiveScale, alpha);
    }
    float2 currentUv = p.uv;
    float2 nextUv = p.nextUv;
    if ((gEffectsFlags & FBZZ_PFX_MOTION_VECTOR) != 0u)
    {
        float2 motion = gMotionVectors.Sample(gSampler, p.uv).rg * 2.0f - 1.0f;
        currentUv += motion * (p.spriteBlend * gMotionVectorStrength);
        nextUv -= motion * ((1.0f - p.spriteBlend) * gMotionVectorStrength);
    }
    // アルファの取り出し方を素材に合わせて解決し、RGB をリニアへ揃えてから 2 コマを混ぜる。
    // WHY: 先に lerp してから輝度を取ると、コマ境界で「合成後の輝度」を
    //      マスクにすることになり、コマの重なった部分だけ濃く出てしまう。
    //      リニア化も混合前に済ませる (混合はリニア空間で行うのが正しい)。
    float4 tex = lerp(ResolveParticleAlbedo(gParticleTex.Sample(gSampler, currentUv), gEffectsFlags),
                      ResolveParticleAlbedo(gParticleTex.Sample(gSampler, nextUv), gEffectsFlags),
                      saturate(p.spriteBlend));
    if (gSoftParticles != 0)
    {
        float sceneDepth = gSceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r;
        float sceneLinear = LinearizeDepth(sceneDepth, nearZ, farZ);
        float particleLinear = LinearizeDepth(p.svPosition.z, nearZ, farZ);
        fade *= saturate((sceneLinear - particleLinear) / gSoftParticleFadeDistance);
    }
    // 頂点カラー (グラデーション) は既にリニア。tint は .mat 由来の共有色調整。
    float4 result = tex * float4(p.color.rgb * gTintColor.rgb, p.color.a * fade * gTintColor.a);

    // 受け影。ビルボードには本物の法線が無いので、法線依存のバイアス項には
    // ライト方向をそのまま渡して法線バイアスを実質無効化する
    // (板ポリゴンに法線バイアスを掛けると自己遮蔽で不自然に暗くなるため)。
    float shadow = 1.0f;
    if ((gEffectsFlags & FBZZ_PFX_RECEIVE_SHADOW) != 0u)
    {
        float3 lightDirection = normalize(-lightDir);
        shadow = ComputeShadow(gShadowMap, gSampShadow, p.worldPos,
                               lightViewProjection, shadowMapTexelSize, shadowBias,
                               lightDirection, lightDirection);
        shadow = lerp(1.0f, shadow, saturate(gShadowStrength));
    }

    // 自己影。粒子群が自分に落とす影で、受け影 (シャドウマップ) とは別物。
    // WHY: これが無いと、粒子をいくら重ねても光の当たり方が一様なままで
    //      厚みのある煙・雲が平坦な塊に見える。厚みはここでしか出せない。
    shadow *= ComputeParticleSelfShadowFromMap(gParticleDensity, gSampler, p.worldPos,
                                               lightViewProjection, gSelfShadowStrength);

    if ((gEffectsFlags & FBZZ_PFX_SIX_WAY) != 0u)
    {
        // ビルボードには本物の法線が無いため、スプライト面を球とみなした疑似法線を作る。
        float2 normalXY = p.localUv * 2.0f - 1.0f;
        float3 normal = normalize(float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY)))));
        float3 lightDirection = normalize(-lightDir);
        float3 viewDir = normalize(cameraPos - p.worldPos);
        // 巻き込み拡散 + 前方散乱。素の N·L だけでは煙が「不透明な球」に見え、
        // 背後の光を透かさないので炎の手前の煙が暗いまま残る。
        float diffuse = ParticleWrappedDiffuse(dot(normal, lightDirection), saturate(gSmokeWrap));
        float back = ParticleBackScatter(viewDir, lightDirection,
                                         gSmokeBackScatterPower, gSmokeTransmission);
        // 影は直接光成分だけに掛け、環境光は残す (影の中の煙が真っ黒に潰れない)。
        float3 lit = ambientColor + lightColor * ((diffuse + back) * shadow);
        result.rgb *= lerp(float3(1.0f, 1.0f, 1.0f), lit, saturate(gLightingStrength));
    }
    else
    {
        // 非ライティング時は色へ直接掛ける。発光体 (加算) では影が効きすぎないよう
        // 完全な 0 にはせず、strength の範囲で減衰させる。
        result.rgb *= shadow;
    }
    result.rgb *= gEmissiveScale;
    if ((gEffectsFlags & FBZZ_PFX_DISTORTION) != 0u)
    {
        float2 screenUv = p.svPosition.xy / max(float2(gScreenWidth, gScreenHeight), float2(1.0f, 1.0f));
        // 歪みベクトルは専用マップ優先。無い場合だけ従来どおり albedo の RG を流用する。
        // WHY: albedo の RG を向きとして使うと、素材を差し替えただけで曲がる向きが変わる。
        // NOTE: 専用マップはリニア化しない (色ではなく [-1,1] のベクトルなので、
        //       ガンマを掛けると向きが歪む)。
        float2 vector2 = (gEffectsFlags & FBZZ_PFX_DISTORTION_MAP) != 0u
            ? gDistortionTex.Sample(gSampler, currentUv).rg
            : tex.rg;
        float2 offset = (vector2 * 2.0f - 1.0f) * gDistortionStrength;
        // 色収差: 屈折率の波長依存を、RGB を歪み方向へずらして表す。
        // 衝撃波の縁が単色で滑るのを防ぎ、圧縮された空気の density 差が見えるようになる。
        float2 dispersion = offset * gDistortionChromatic;
        float3 refracted;
        refracted.r = gSceneColor.Sample(gSampler, saturate(screenUv + offset + dispersion)).r;
        refracted.g = gSceneColor.Sample(gSampler, saturate(screenUv + offset)).g;
        refracted.b = gSceneColor.Sample(gSampler, saturate(screenUv + offset - dispersion)).b;
        result = float4(refracted, result.a);
    }
    // 事前乗算アルファは SrcBlend=ONE なので RGB が「そのまま」出力される。
    // 他のブレンドはブレンド側が src.a を掛けてくれるため RGB は素のままでよいが、
    // 事前乗算では RGB 自身が alpha 込みの値になっていなければならない。
    // 掛け落としていた 2 つはどちらも致命的だった:
    //   RadialMask (fade の初期値) — 矩形の角を消すマスク。RGB に掛からないと角が
    //     残り、スプライトが「■」として見える。
    //   p.color.a — グラデーションのアルファ。RGB に掛からないと、寿命の終わりで
    //     alpha が 0 へ落ちても RGB が残り続け、消えるはずの粒子が四角い光として
    //     居座る (「途中から■が見え始める」の正体)。
    // 歪み (distortion) で RGB を差し替えた後にも効かせる必要があるため、
    // 個々の分岐ではなく PSMain 末尾の 1 か所へ置く。
    // NOTE: ボリュメトリック経路は scattered を alpha で重み付け済みのまま早期 return
    //       するので、ここは通らない (二重に掛からない)。
    if ((gEffectsFlags & FBZZ_PFX_PREMULTIPLIED) != 0u) result.rgb *= p.color.a * fade;
    return result;
}
