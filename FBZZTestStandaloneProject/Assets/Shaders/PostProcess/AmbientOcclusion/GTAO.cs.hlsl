// FBZZ Engine
// PostProcess/AmbientOcclusion/GTAO.cs.hlsl | PostProcess
// Ground Truth Ambient Occlusion (Horizon-Based AO / HBAO)
// — スライスごとにホライゾン角を探索し、より正確な遮蔽率を計算する
//
// アルゴリズム概要 (HBAO 近似):
//   1. 深度からビュー空間座標を復元
//   2. GBuffer1 から法線を復元
//   3. gtaoSlices 方向にスライスを分割し、各スライスで
//      gtaoStepsPerSlice ステップ前後にサンプルしてホライゾン角を探索
//   4. AO = 1 - (H1 + H2) / PI の近似で遮蔽率を算出
//   5. 全スライスの平均を出力
//
// 入力バインディング:
//   t6  = GBuffer1 (法線 RGB + metallic A)  (TEX_GBUFFER1)
//   t7  = 深度バッファ                       (TEX_DEPTH)
//   u6  = 出力 UAV                           (UAV_GTAO_RAW)
//   b0  = CameraConstants
//   b5  = PostProcConstants
//   b8  = AdvancedGraphicsConstants
//
// Dispatch サイズ: ceil(width/8) x ceil(height/8) x 1

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"

Texture2D        texGBuffer1 : register(TEX_GBUFFER1); // 法線(RGB) + metallic(A)
Texture2D<float> texDepth    : register(TEX_DEPTH);    // 深度バッファ

SamplerState sampDefault : register(SAMPLER_DEFAULT);

RWTexture2D<float> OutputGTAO : register(UAV_GTAO_RAW); // GTAO RAW 出力

static const float PI = 3.14159265f;

// ─── ビュー空間座標の復元 ─────────────────────────────────────────────────────
// WHY: HBAO はビュー空間でホライゾン角を計算するため、ワールドではなくビュー空間を使う
float3 ReconstructViewSpacePos(float2 uv, float ndcZ, float4x4 ndcToView)
{
    // NDC → ビュー空間を 1 回の行列積で復元する。ndcToView = invViewProjection * view を
    // 呼び出し側で 1 度だけ計算して渡す。元の「invVP でワールド → view でビュー空間」の 2 段と
    // 数学的に等価 (結合則。view は剛体変換なので w が保存される) だが、サンプル毎の行列積が半分になる。
    float4 ndc = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, ndcZ, 1.0f);
    float4 vs  = mul(ndc, ndcToView);
    return vs.xyz / vs.w;
}

// ─── ホライゾン角の計算 ───────────────────────────────────────────────────────
// sampleVS: サンプル点のビュー空間座標
// originVS: 現ピクセルのビュー空間座標
// dir2D   : スライス方向（ビュー空間 XY 平面内）
float ComputeHorizonAngle(float3 originVS, float3 sampleVS, float3 dir2D)
{
    float3 delta = sampleVS - originVS;
    float  proj  = dot(delta, dir2D); // スライス方向への投影
    float  dz    = delta.z;           // 深度差

    // ホライゾン角 = atan2(深度差, スライス方向投影距離)
    // 遮蔽があると角度が大きくなる
    return atan2(-dz, max(abs(proj), 1e-5f));
}

// ─── メインカーネル ────────────────────────────────────────────────────────────

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;

    // 出力 AO バッファの実サイズを基準にする (半解像度対応)。
    // WHY: AO は低周波なので半解像度で焼き、消費側 (DeferredLighting) が正規化 UV の linear サンプルで
    //      アップスケールする。screenSize/texelSize(b5) はフル解像度のままだが、下のホライゾン UV
    //      オフセットは texelSize*screenSize が打ち消し合い解像度非依存なので流用できる。ここでは
    //      pixel→uv と範囲判定のみ出力サイズ基準にする。
    float2 outSize;
    OutputGTAO.GetDimensions(outSize.x, outSize.y);
    if (pixel.x >= (uint)outSize.x || pixel.y >= (uint)outSize.y)
    {
        OutputGTAO[pixel] = 1.0f;
        return;
    }

    float2 uv = (float2(pixel) + 0.5f) / outSize;

    // ── 深度チェック: スカイドームは AO 不要 ────────────────────────────────────
    float ndcZ = texDepth.SampleLevel(sampDefault, uv, 0).r;
    if (ndcZ >= 0.9999f)
    {
        OutputGTAO[pixel] = 1.0f;
        return;
    }

    // ── GBuffer1 から法線を復元（ワールド空間）──────────────────────────────────
    float3 N = texGBuffer1.SampleLevel(sampDefault, uv, 0).rgb * 2.0f - 1.0f;
    N = normalize(N);

    // ── ビュー空間座標を復元 ──────────────────────────────────────────────────
    // NDC → ビュー空間の合成行列を 1 度だけ用意し、全ホライゾンサンプルで使い回す (ALU 最適化)。
    float4x4 ndcToView = mul(invViewProjection, view);
    float3 originVS = ReconstructViewSpacePos(uv, ndcZ, ndcToView);

    // ── ノイズで各ピクセルのスライス開始角度をランダム化（時間的フリッカ低減）──────
    // WHY: 固定パターンのバンディングを避けるため、Hash2D でスライス位相をずらす
    float angleOffset = Hash2D(uv + float2(time * 0.1f, time * 0.07f)) * PI;

    // ── 全スライスの AO を積算 ────────────────────────────────────────────────
    float totalAO = 0.0f;
    int   slices  = max(gtaoSlices, 1);
    int   steps   = max(gtaoStepsPerSlice, 1);

    for (int slice = 0; slice < slices; ++slice)
    {
        // スライス方向をビュー空間 XY 平面内に均等配置
        float sliceAngle = angleOffset + (float(slice) / float(slices)) * PI;
        float2 dir2D     = float2(cos(sliceAngle), sin(sliceAngle));

        // ホライゾン角の最大値を探索（前方・後方それぞれ）
        float maxH1 = 0.0f; // +方向
        float maxH2 = 0.0f; // -方向

        for (int step = 1; step <= steps; ++step)
        {
            // ビュー空間でのサンプリング半径をステップで均等分割
            float  stepFrac  = float(step) / float(steps);
            float  radius    = stepFrac * gtaoRadius;

            // UV オフセット（ビュー空間半径 → スクリーン空間オフセット）
            // 近似: ビュー空間 X = NDC X * (farZ / projection[0][0]) → texelSize スケールで近似
            float2 uvOffset = dir2D * radius * texelSize * (screenSize.x * 0.5f);

            // ── 正方向サンプル ─────────────────────────────────────────────────
            float2 sUVPos   = saturate(uv + uvOffset);
            float  sDepthP  = texDepth.SampleLevel(sampDefault, sUVPos, 0).r;
            float3 sVSPos   = ReconstructViewSpacePos(sUVPos, sDepthP, ndcToView);
            float  hP       = ComputeHorizonAngle(originVS, sVSPos, float3(dir2D, 0.0f));
            maxH1           = max(maxH1, hP);

            // ── 負方向サンプル ─────────────────────────────────────────────────
            float2 sUVNeg   = saturate(uv - uvOffset);
            float  sDepthN  = texDepth.SampleLevel(sampDefault, sUVNeg, 0).r;
            float3 sVSNeg   = ReconstructViewSpacePos(sUVNeg, sDepthN, ndcToView);
            float  hN       = ComputeHorizonAngle(originVS, sVSNeg, float3(-dir2D, 0.0f));
            maxH2           = max(maxH2, hN);
        }

        // このスライスの AO: 1 - (H1 + H2) / PI
        // WHY: 半球全体が遮蔽されると H1 = H2 = PI/2 → AO = 0
        //      遮蔽なしは H1 = H2 = 0 → AO = 1
        float sliceAO = 1.0f - saturate((maxH1 + maxH2) / PI);
        totalAO += sliceAO;
    }

    // 全スライスを平均し、gtaoIntensity で強度スケール
    float ao = lerp(1.0f, totalAO / float(slices), saturate(gtaoIntensity));
    OutputGTAO[pixel] = ao;
}
