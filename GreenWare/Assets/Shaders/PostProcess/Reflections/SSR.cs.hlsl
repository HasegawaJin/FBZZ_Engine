// FBZZ Engine
// SSR.cs.hlsl | PostProcess/Reflections
// Screen Space Reflections — Compute Shader (DX11 SM5.0)
//
// 概要:
//   GBuffer からベースカラー・粗さ・法線・金属度・深度を取得し、スクリーン空間で
//   レイマーチを行って、マテリアルの Fresnel 反射率を反映した反射色を書き出す。
//   誘電体も斜入射では反射するため、金属度だけで除外しない。
//
// バインディング:
//   t0  = texGBuffer0  (ベースカラー RGB + 粗さ A)
//   t5  = texHDR       (GBuffer0 スロット再利用、HDR カラー)
//   t6  = texGBuffer1  (法線 RGB + 金属度 A)
//   t7  = texDepth      (Deferred GBuffer 深度)
//   t25 = texSceneDepth (Forward 不透明物を含む HDR 深度)
//   u3  = OutputSSR
//   b0  = CameraConstants
//   b8  = AdvancedGraphicsConstants
//
// スレッドグループ: [8, 8, 1]

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/BindlessIndices.hlsli"

// ---- リソース ---------------------------------------------------------------

// HDR カラー (GBuffer0 スロット t5 を流用)
FBZZ_TEX2D(texHDR, 5);
// GBuffer0: albedo(RGB) + roughness(A)
FBZZ_TEX2D(texGBuffer0, 0);
// GBuffer1: normal(RGB) + metallic(A)
FBZZ_TEX2D(texGBuffer1, 6);
// Deferred GBuffer 深度
FBZZ_TEX2D_T(float, texDepth, 7);
// Deferred 深度の転写後に Forward 不透明物を書き込んだ最終シーン深度
FBZZ_TEX2D_T(float, texSceneDepth, TEX_SCENE_DEPTH_SLOT);

/// @note 全画面フェッチなので clamp (s0 は DX12 では WRAP で、画面端が反対側の色を拾う)。
SamplerState       sampDefault : register(SAMPLER_LINEAR_CLAMP);

// SSR 出力 (UAV_SSR = u3)
FBZZ_RWTEX2D_T(float4, OutputSSR, 3);

// ---- メインカーネル ---------------------------------------------------------

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // スクリーンサイズを texelSize から算出
    // WHY: screenSize は PostProcConstants(b5) にあるが、ここでは b0/b8 のみ宣言する設計のため
    //      texelSize の逆数で代替する。
    float2 outputSize;
    OutputSSR.GetDimensions(outputSize.x, outputSize.y);

    // テクスチャ境界チェック
    if ((float)id.x >= outputSize.x || (float)id.y >= outputSize.y)
        return;

    if (ssrIntensity <= 0.0f || ssrMaxDistance <= 0.0f || ssrSteps <= 0)
    {
        OutputSSR[id.xy] = 0.0f;
        return;
    }

    float2 uv = (float2(id.xy) + 0.5f) / outputSize;

    // ---- GBuffer サンプル ---------------------------------------------------

    // GBuffer0/1 から反射元サーフェスの PBR パラメータを復元する。
    // GBuffer と深度は対象ピクセルを直接読む。
    // WHY: 境界で隣接ジオメトリを線形補間すると、存在しない法線・深度からレイが生成される。
    float4 gb0  = texGBuffer0.Load(int3(id.xy, 0));
    float4 gb1  = texGBuffer1.Load(int3(id.xy, 0));
    float3 N    = normalize(gb1.xyz * 2.0f - 1.0f);
    float  met  = gb1.w;
    float3 baseColor = max(gb0.rgb, 0.0f);
    float  rough = saturate(gb0.a);

    // 非常に粗い面では単一レイの SSR が不正確なため IBL に委ねる。
    if (rough >= 0.95f)
    {
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // 深度の取得
    float ndcDepth = texDepth.Load(int3(id.xy, 0)).r;

    // Forward 描画された不透明物が反射面より手前にあるピクセルでは SSR を合成しない。
    // WHY: SkinnedMeshRenderer は GBuffer 後に HDR へ描かれるため、GBuffer 深度だけでは Player の遮蔽を検出できない。
    /// @note カメラ深度は Reversed-Z なので «手前» は値が大きい側。
    float sceneNdcDepth = texSceneDepth.Load(int3(id.xy, 0)).r;
    if (sceneNdcDepth - 1e-5f > ndcDepth)
    {
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    /// @note 最遠 (Reversed-Z で 0) はスカイボックス。反射不要。
    if (IsFarDepth(ndcDepth))
    {
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // ---- 座標復元 -----------------------------------------------------------

    // ワールド座標
    float3 worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);

    // カメラ → サーフェスの視線ベクトル (正規化)
    /// @note 平行投影では視線が全画素で同じ (カメラ前方)。視空間 +Z をワールドへ戻す。
    float3 V = isOrthographic > 0.5f
        ? normalize(mul((float3x3)view, float3(0.0f, 0.0f, 1.0f)))
        : normalize(worldPos - cameraPos);

    // ---- 反射レイの生成 -----------------------------------------------------

    // ワールド空間での反射ベクトル R = reflect(入射方向, 法線)
    float3 R = reflect(V, N);

    // R をビュー空間に変換してレイマーチする
    // WHY: ビュー空間でのレイマーチは NDC への投影が単純な除算で済む
    float3 rayOriginVS = mul(float4(worldPos, 1.0f), view).xyz;
    float3 rayDirVS    = normalize(mul(float4(R, 0.0f), view).xyz);

    // ---- スクリーン空間レイマーチ -------------------------------------------

    int stepCount = max(ssrSteps, 1);

    // レイ終端をカメラの near/far 範囲内へ制限する。
    // WHY: 投影不能なカメラ背面や far 面外にステップを消費せず、画面内の探索密度を維持する。
    float traceDistance = ssrMaxDistance;
    if (rayDirVS.z < -1e-5f && rayOriginVS.z + rayDirVS.z * traceDistance < nearZ)
        traceDistance = max((nearZ * 1.001f - rayOriginVS.z) / rayDirVS.z, 0.0f);
    else if (rayDirVS.z > 1e-5f && rayOriginVS.z + rayDirVS.z * traceDistance > farZ)
        traceDistance = max((farZ - rayOriginVS.z) / rayDirVS.z, 0.0f);

    float3 rayEndVS = rayOriginVS + rayDirVS * traceDistance;
    float4 originClip = mul(float4(rayOriginVS, 1.0f), projection);
    float4 endClip    = mul(float4(rayEndVS, 1.0f), projection);
    if (traceDistance <= 0.0f || abs(originClip.w) < 1e-5f || abs(endClip.w) < 1e-5f)
    {
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float2 originUV = NdcToUv(originClip.xy / originClip.w);
    float2 endUV    = NdcToUv(endClip.xy / endClip.w);
    float2 uvDelta  = endUV - originUV;

    // 画面外へ出る位置でレイを切り、利用可能なステップを画面内へ集中させる。
    float maxTraceRatio = 1.0f;
    if (uvDelta.x > 1e-6f)
        maxTraceRatio = min(maxTraceRatio, (1.0f - originUV.x) / uvDelta.x);
    else if (uvDelta.x < -1e-6f)
        maxTraceRatio = min(maxTraceRatio, -originUV.x / uvDelta.x);
    if (uvDelta.y > 1e-6f)
        maxTraceRatio = min(maxTraceRatio, (1.0f - originUV.y) / uvDelta.y);
    else if (uvDelta.y < -1e-6f)
        maxTraceRatio = min(maxTraceRatio, -originUV.y / uvDelta.y);
    maxTraceRatio = saturate(maxTraceRatio);

    // ワールド距離ではなくスクリーン上で均等に進める。
    // WHY: 真上・真横などの斜角では固定ワールドステップの投影間隔が広がり、縞状の欠落が発生する。
    float pixelLength = length(uvDelta * maxTraceRatio * outputSize);
    int marchStepCount = min(stepCount, max((int)ceil(pixelLength), 1));
    float previousRayDepth = rayOriginVS.z;
    float previousDepthDiff = 0.0f;
    float4 hitColor  = float4(0.0f, 0.0f, 0.0f, 0.0f);
    bool   bHit      = false;

    [loop]
    for (int step = 1; step <= marchStepCount; ++step)
    {
        float traceRatio = maxTraceRatio * (float(step) / float(marchStepCount));
        float2 rayUV = originUV + uvDelta * traceRatio;

        // 非線形深度は補間せず、レイ位置に対応するピクセルを直接取得する。
        // WHY: raw depth の線形補間は実際には存在しないビュー深度を生成し、輪郭を貫通させる。
        int2 depthPixel = clamp(int2(rayUV * outputSize), int2(0, 0), int2(outputSize) - 1);
        // HDR 深度には Deferred 深度の転写後に描かれた Player 等も含まれる。
        float sampleNdcDepth = texSceneDepth.Load(int3(depthPixel, 0)).r;

        // ビュー空間での深度に変換して比較
        // WHY: ビュー空間深度は線形なので厚みの比較が直感的
        float sampleDepthLinear = LinearizeDepth(sampleNdcDepth, nearZ, farZ, isOrthographic);
        // 透視投影後の補間率からビュー深度を逆数補間する。
        // WHY: UV を線形補間してもビュー深度は線形でないため、遠近補正なしでは交差位置がずれる。
        /// @note 平行投影では画面上の補間がそのまま視空間 Z の補間になる。
        float rayDepthLinear = isOrthographic > 0.5f
            ? lerp(rayOriginVS.z, rayEndVS.z, traceRatio)
            : rcp(lerp(rcp(rayOriginVS.z), rcp(rayEndVS.z), traceRatio));

        // 1 ステップ分の深度移動を交差幅に含め、深度帯の飛び越しを防ぐ。
        float hitThickness = max(ssrThickness, abs(rayDepthLinear - previousRayDepth));

        // レイがシーン深度の手前から奥へ横切った場合だけ交差候補にする。
        // WHY: 単に depthDiff > 0 の点を拾うと、既に遮蔽物の背後にあるレイまで反射として採用される。
        float depthDiff = rayDepthLinear - sampleDepthLinear;
        bool crossedDepthSurface = previousDepthDiff <= 0.0f && depthDiff > 0.0f;
        if (crossedDepthSurface && depthDiff < hitThickness)
        {
            // 交差 UV の HDR カラーをサンプル
            hitColor = texHDR.SampleLevel(sampDefault, rayUV, 0);
            bHit     = true;
            break;
        }

        previousRayDepth = rayDepthLinear;
        previousDepthDiff = depthDiff;
    }

    // ---- 出力 ---------------------------------------------------------------

    if (bHit)
    {
        // F0 と Schlick Fresnel により、金属は base color、誘電体は 4% を基準に反射を着色する。
        // RGB Fresnel を色と最大反射率に分け、alpha 1本の合成でも色付き金属を保持する。
        float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, saturate(met));
        float  NdotV = saturate(dot(N, -V));
        float3 fresnel = F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f);
        float  reflectance = max(fresnel.r, max(fresnel.g, fresnel.b));
        float  roughnessConfidence = 1.0f - smoothstep(0.20f, 0.95f, rough);
        float  confidence = saturate(reflectance * roughnessConfidence);
        float3 reflectionTint = fresnel / max(reflectance, 1e-4f);

        // ssrIntensity は Composite で一度だけ適用し、二重乗算を避ける。
        OutputSSR[id.xy] = float4(hitColor.rgb * reflectionTint, confidence);
    }
    else
    {
        // 未交差: 透明 (後段で IBL フォールバックを使用)
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
}
