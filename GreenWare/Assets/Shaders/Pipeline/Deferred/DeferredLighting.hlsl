// FBZZ Engine
// Pipeline/Deferred/DeferredLighting.hlsl | Pipeline
// ディファードライティングパス — GBuffer を読み取り PBR ライティングを適用する
//
// フルスクリーン三角形 (頂点バッファなし) で描画する。
// VS はスクリーン座標を生成し、PS が GBuffer + 深度から worldPos を復元する。

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

Texture2D      texGBuffer0  : register(TEX_GBUFFER0);   // albedo(RGB) + roughness(A)
Texture2D      texGBuffer1  : register(TEX_GBUFFER1);   // normal(RGB) + metallic(A)
Texture2D      texDepth     : register(TEX_DEPTH);
Texture2D<float>       texShadow    : register(TEX_SHADOW);
Texture2D              texSSAO      : register(TEX_SSAO);
Texture2D<float>       texContactShadow : register(TEX_CONTACT_SHADOW); // t24: 接触影マスク (1=非遮蔽, 0=遮蔽)
SamplerState           sampDefault  : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow   : register(SAMPLER_SHADOW);
SamplerState           sampGBuffer  : register(SAMPLER_POINT_CLAMP);

// IBL テクスチャ — AdvancedGraphicsConstants.iblIntensity > 0 のときにバインドされる
// irradiance : 拡散 IBL (半球積分済み)
// prefilter  : 鏡面 IBL (roughness → mip でフィルタリング済み)
// brdfLUT    : BRDF 積分テーブル (BRDFIntegration.cs.hlsl でスタートアップ時ベイク)
TextureCube       texIBLIrradiance : register(TEX_IBL_IRRADIANCE);  // t16
TextureCube       texIBLPrefilter  : register(TEX_IBL_PREFILTER);   // t17
Texture2D<float4> texBRDFLut       : register(TEX_IBL_BRDF_LUT);   // t18: RG=scale/bias
// IBL BRDF LUT は UV が [0,1] 範囲外に出ないよう Linear Clamp でサンプリングする
SamplerState      sampLinearClamp  : register(SAMPLER_LINEAR_CLAMP); // s2

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

// フルスクリーン三角形: 頂点 ID だけで UV / クリップ座標を生成する
FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv          = float2((id & 1u) ? 2.0f : 0.0f,
                           (id & 2u) ? 2.0f : 0.0f);
    o.svPosition  = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    float2 uv = p.uv;

    // 深度を先読みし、ジオメトリがないピクセル (空・背景) を除外する
    float  ndcDepth = texDepth.Sample(sampGBuffer, uv).r;
    if (ndcDepth >= 1.0f) discard;

    // GBuffer 展開
    float4 gb0    = texGBuffer0.Sample(sampGBuffer, uv);
    float4 gb1    = texGBuffer1.Sample(sampGBuffer, uv);
    float3 col    = gb0.rgb;
    float  rough  = gb0.a;
    // サンプラー補間や GBuffer 境界でも BRDF に単位法線を渡し、鏡面値の発散を防ぐ。
    float3 N      = SafeNormalize(gb1.rgb * 2.0f - 1.0f,
                                  float3(0.0f, 1.0f, 0.0f));  // デコード [0,1] → [-1,1]
    float  met    = gb1.a;

    // WHAT: 画面上で急変する法線の分散を roughness に畳み込み、サブピクセル鏡面を低域化する。
    // WHY: IBL の高輝度方向を細い法線ピークが拾うと、非金属では白、金属では base color の
    //      点状ハイライトになる。ライト非依存の specular aliasing を BRDF 評価前に抑える。
    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);
    float normalVariance = 0.5f * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
    float kernelRoughness2 = min(2.0f * normalVariance, 0.18f);
    rough = max(sqrt(saturate(saturate(rough) * saturate(rough) + kernelRoughness2)), 0.045f);
    met   = saturate(met);

    // 深度から worldPos を復元
    float3 worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);

    // SSAO が有効なときだけ遮蔽テクスチャを反映する。
    float ao = 1.0f;
    if (ssaoIntensity > 0.0f)
    {
        float ssao = texSSAO.Sample(sampLinearClamp, uv).r;
        ao = lerp(1.0f, saturate(ssao), saturate(ssaoIntensity));
    }

    float3 V      = SafeNormalize(cameraPos - worldPos, N);
    float3 L      = SafeNormalize(-lightDir, N);
    float  shadow = ComputeShadow(texShadow, sampShadow, worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);

    // コンタクトシャドウ: 有効時 (contactShadowStrength>0) のみスクリーンスペース接触影を乗算する。
    // WHY: contactShadowStrength は無効時に 0 が CB に入る (RenderSystem) ため、無効時は未バインドの
    //      t24 をサンプルせずに済む。マスクは ContactShadows.cs が strength を織り込み済み (1=非遮蔽)。
    //      半解像度で焼いても sampLinearClamp の正規化 UV サンプルでバイリニアにアップスケールされる。
    if (contactShadowStrength > 0.0f)
        shadow *= texContactShadow.Sample(sampLinearClamp, uv);

    // ---- ディレクショナルライト + アンビエント --------------------------------
    // IBL が有効 (iblIntensity > 0) のとき: Lighting_PBR_IBL で
    //   環境光を物理ベース IBL に差し替える。定数 ambientColor は使わない。
    // IBL が無効 (iblIntensity == 0) のとき: 従来の Lighting_PBR を使い
    //   定数 ambient (albedo * ambientColor * ao) を維持する。
    float3 result;
    if (iblIntensity > 0.0f)
    {
        // IBL アンビエント + ダイレクトライティングを一括評価
        // iblIntensity は Lighting_PBR_IBL 内で EvaluateIBL 戻り値に乗算される
        result = Lighting_PBR_IBL(N, V, L, col, met, rough,
                                   lightColor, lightIntensity, shadow, ao,
                                   texIBLIrradiance, texIBLPrefilter, texBRDFLut,
                                   iblMaxMipLevel, iblIntensity,
                                   iblDiffuseScale, iblSpecularScale,
                                   sampDefault, sampLinearClamp);
    }
    else
    {
        // IBL 無効: 従来の定数アンビエント PBR (後方互換)
        result = Lighting_PBR(N, V, L, col, met, rough,
                              lightColor, lightIntensity, shadow, ao);
    }

    // ---- ポイントライトループ ------------------------------------------------
    [loop] for (int pi = 0; pi < pointLightCount; ++pi)
    {
        float3 toLight = pointLights[pi].position - worldPos;
        float  dist    = length(toLight);
        float3 Lp      = SafeNormalize(toLight, N);
        float  atten   = LightAttenuation(dist, pointLights[pi].range);
        result += Lighting_PBR_Direct(N, V, Lp, col, met, rough,
                      pointLights[pi].color, pointLights[pi].intensity * atten);
    }

    // ---- スポットライトループ ------------------------------------------------
    [loop] for (int si = 0; si < spotLightCount; ++si)
    {
        float3 toLight = spotLights[si].position - worldPos;
        float  dist    = length(toLight);
        float3 Ls      = SafeNormalize(toLight, N);
        float  atten   = LightAttenuation(dist, spotLights[si].range);
        float  cone    = SpotConeWeight(Ls, spotLights[si].direction,
                             spotLights[si].innerCos, spotLights[si].outerCos);
        result += Lighting_PBR_Direct(N, V, Ls, col, met, rough,
                      spotLights[si].color, spotLights[si].intensity * atten * cone);
    }

    // カスケード可視化 (デバッグ)。無効時は白を返すので通常描画には影響しない。
    // WHY: 分割位置 (Split Lambda) と境界ブレンド幅は数値だけでは詰められない。
    //      不透明の主経路である Deferred へ入れておけば、地面や壁での切り替わりが
    //      そのまま見えて調整できる。
    result *= ShadowCascadeDebugTint(worldPos);

    return float4(result, 1.0f);
}
