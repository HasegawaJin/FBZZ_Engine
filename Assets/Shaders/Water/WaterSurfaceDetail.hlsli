/// @file    WaterSurfaceDetail.hlsli
/// @brief   移流するさざ波、頂点で解像できない波の勾配、法線合成。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_SURFACE_DETAIL_HLSLI
#define FBZZ_WATER_SURFACE_DETAIL_HLSLI

/// @note WaterSurface.hlsli から読み込む。
/// @brief さざ波タイルから «1 ノイズセル単位の» 勾配と高さを引く。
/// @param q       ノイズセル単位の座標。
/// @param dqdx    q の画面 x 微分。異方フィルタの LOD 選択に使う。
/// @param dqdy    q の画面 y 微分。
/// @return xy = ∂h/∂q, z = 高さ [0,1]。
/// @note 呼び出し元は帯域外の tap を分岐で省く。非一様制御流れでも mip を選べるよう画面微分を明示する。
/// @note タイルの縮小は «勾配の平均» なので、遠景では自動的に平坦へ収束し、水面が鏡へ近づく。
float3 WaterNoiseTile(float2 q, float2 dqdx, float2 dqdy)
{
    float invCells = g_timeParams.z;
    float4 t = g_detailNoise.SampleGrad(g_samplerTile, q * invCells,
                                        dqdx * invCells, dqdy * invCells);
    return float3((t.rg * 2.0f - 1.0f) * g_normalParams.z, t.b);
}

struct WaterCurrentAdvection
{
    float2 firstOffset;
    float2 secondOffset;
    float firstWeight;
    float directionWeight;
};

/// @brief Trace the two noise samples upstream along the local world XZ velocity [m/s].
/// @note Each sample fades out as its phase wraps, bounding the distance traced through a curved velocity field.
WaterCurrentAdvection WaterCurrentAt(float2 velocity, float time)
{
    const float period = 1.6f;
    const float firstPhase = frac(time / period);
    const float secondPhase = frac(firstPhase + 0.5f);
    WaterCurrentAdvection current;
    current.firstOffset = -velocity * (period * (firstPhase - 0.5f));
    current.secondOffset = -velocity * (period * (secondPhase - 0.5f));
    current.firstWeight = smoothstep(0.0f, 1.0f, 1.0f - abs(2.0f * firstPhase - 1.0f));
    current.directionWeight = smoothstep(0.0f, 0.25f, length(velocity));
    return current;
}

/// @note オクターブごとの固有ドリフト。同じ向きに揃うと縞が流れて見えるため方向をばらす。
#define WATER_DETAIL_OCTAVES 5
static const float2 kWaterDrift[WATER_DETAIL_OCTAVES] = {
    float2( 0.31f,  0.17f), float2(-0.23f,  0.41f),
    float2( 0.47f, -0.29f), float2(-0.37f, -0.13f),
    float2( 0.11f,  0.53f)
};
/// @note 振幅 0.55^i (i = 0..4) の総和。途中で打ち切っても正規化がぶれないよう定数で持つ。
static const float kWaterDetailNorm = 2.110381f;
/// @note WaterNoiseBake.hpp の kGradientRms=0.5 の二乗。落とした勾配の分散を粗さへ渡す。
static const float kWaterDetailGradientVariance = 0.25f;
/// @note Nyquist cutoff 後の振幅分散比。0.55 amplitude × 2.07 frequency の二乗。
static const float kWaterDetailVarianceRatio = (0.55f * 2.07f) * (0.55f * 2.07f);
/// @note 完全に濾過された現在 octave から末尾までの幾何級数和。
static const float kWaterDetailVarianceTail[WATER_DETAIL_OCTAVES] = {
    1.0f + kWaterDetailVarianceRatio + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio
        + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio * kWaterDetailVarianceRatio
        + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio * kWaterDetailVarianceRatio * kWaterDetailVarianceRatio,
    1.0f + kWaterDetailVarianceRatio + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio
        + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio * kWaterDetailVarianceRatio,
    1.0f + kWaterDetailVarianceRatio + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio,
    1.0f + kWaterDetailVarianceRatio,
    1.0f
};

/// @brief さざ波の高さ勾配 (∂h/∂x, ∂h/∂z) をワールド XZ で積む。
/// @param worldXZ   ワールド XZ [m]。UV で評価すると extent の違う水面どうしで粒度が揃わない。
/// @param flowDir   この点の流れの向き (正規化)。渦の中では接線へ曲がる。
/// @param footprint このピクセルが覆うワールド距離 [m]。遠いほど・浅い角度ほど大きい。
/// @param dWorldDx  worldXZ の画面 x 微分。タイルの異方フィルタへ渡す。
/// @param dWorldDy  worldXZ の画面 y 微分。
/// @return xy = 勾配, z = Nyquist で落とした傾きの 1 軸平均分散 (強度を掛ける前)。
/// @note 風向に直交して座標を縮め、さざ波の «うね» を進行方向と直交させる。等方ノイズのままだと
/// @note 粒の集まりにしか見えず、水というより «ブツブツした膜» になる。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/simulating-waves-using-the-water-waves-asset-in-unreal-engine Unreal Engine Water Waves, macro mesh waves and material normal-map micro detail.
float3 WaterDetailGradient(float2 worldXZ, float2 flowDir, float2 flowTravel, float time, float footprint,
                           float2 dWorldDx, float2 dWorldDy, bool followCurrent, WaterCurrentAdvection current)
{
    float scale = max(g_detailParams.x, 1.0e-4f);
    float speed = g_detailParams.y;
    float2 flow = flowTravel;

    float  stretch = max(g_flowParams.z, 1.0f);
    float2 alongFlow = flowDir;
    float2 crossFlow = float2(-alongFlow.y, alongFlow.x);
    float2x2 aniso = float2x2(alongFlow.x, alongFlow.y, crossFlow.x / stretch, crossFlow.y / stretch);

    /// @note オクターブごとに座標を回し、格子が縞として残らないようにする。異方スケールを
    /// @note 初期値に畳んでおけば、以降は回転を掛け足すだけで両方が同時に効く。
    /// @note Rotating absolute world coordinates by a varying flow direction shears the noise at bends.
    float2x2 m = followCurrent ? float2x2(1.0f, 0.0f, 0.0f, 1.0f) : aniso;
    const float2x2 step = float2x2(0.80f, -0.60f, 0.60f, 0.80f);

    float2 grad = float2(0.0f, 0.0f);
    float amp = 1.0f, freq = scale, lostVariance = 0.0f;
    const float crossGain = lerp(1.0f, rcp(stretch), current.directionWeight);
    const float varianceAxisScale = followCurrent ? crossGain : rcp(stretch);
    const float meanAxisScaleSq = 0.5f * (1.0f + varianceAxisScale * varianceAxisScale);
    const float2 firstDx = dWorldDx + ddx(current.firstOffset);
    const float2 firstDy = dWorldDy + ddy(current.firstOffset);
    const float2 secondDx = dWorldDx + ddx(current.secondOffset);
    const float2 secondDy = dWorldDy + ddy(current.secondOffset);
    const float driftSpeed = followCurrent ? 0.0f : speed;

    [loop]
    for (int i = 0; i < WATER_DETAIL_OCTAVES; ++i)
    {
        /// @note 1 ピクセルに 1 周期以上入るオクターブは、平均すれば «ざらつき» しか残らない。
        /// @note 残すとカメラが動くたびに遠景の水面が総毛立って明滅する。周期がピクセルの 2 倍を
        /// @note 切ったところから滑らかに寝かせ、«細部が消えて鏡に近づく» 見え方へ収束させる。
        /// @note 消えた後も分散だけは積む。振幅・周波数・異方スケールを含め、弱いさざ波を過剰にぼかさない。
        float fade = 1.0f - smoothstep(0.0f, 0.5f, footprint * freq);
        float slopeScale = amp * freq / kWaterDetailNorm;
        if (fade <= 0.0f)
        {
            lostVariance += kWaterDetailGradientVariance * meanAxisScaleSq
                          * slopeScale * slopeScale * kWaterDetailVarianceTail[i];
            break;
        }
        lostVariance += kWaterDetailGradientVariance * meanAxisScaleSq
                      * slopeScale * slopeScale * (1.0f - fade * fade);
        if (fade > 0.0f)
        {
            const float2 q = (mul(m, worldXZ) + flow + kWaterDrift[i] * (time * driftSpeed)) * freq;
            float3 n;
            if (followCurrent && current.directionWeight > 0.0f) {
                const float3 first = WaterNoiseTile(q + mul(m, current.firstOffset) * freq,
                    mul(m, firstDx) * freq, mul(m, firstDy) * freq);
                const float3 second = WaterNoiseTile(q + mul(m, current.secondOffset) * freq,
                    mul(m, secondDx) * freq, mul(m, secondDy) * freq);
                n = lerp(second, first, current.firstWeight);
            } else {
                n = WaterNoiseTile(q, mul(m, dWorldDx) * freq, mul(m, dWorldDy) * freq);
            }
            /// @note 勾配は変換後の座標系で出るので、mul(g, m) = m^T g で元の軸へ戻す (連鎖律)。
            grad += mul(n.xy, m) * (amp * freq * fade);
        }
        m     = mul(step, m);
        amp  *= 0.55f;
        freq *= 2.07f;
    }

    if (followCurrent)
        grad -= crossFlow * (dot(grad, crossFlow) * (1.0f - crossGain));
    const float invNorm = 1.0f / kWaterDetailNorm;
    return float3(grad * invNorm, lostVariance);
}

float3 WaterDetailGradient(float2 worldXZ, float2 flowDir, float2 flowTravel, float time, float footprint,
                           float2 dWorldDx, float2 dWorldDy)
{
    return WaterDetailGradient(worldXZ, flowDir, flowTravel, time, footprint, dWorldDx, dWorldDy,
        false, (WaterCurrentAdvection)0);
}

/// @brief 頂点グリッドで刻めなかった Gerstner 波を «法線だけ» 戻す。
/// @param dWorldDx,dWorldDy 波変位前のワールド XZ の画面微分 [m/pixel]。
/// @param[out] lostVariance ピクセルより細かくて捨てた傾きの 1 軸平均分散。粗さへ回す。
/// @param crestDetail 残差波の基本波に対する二次高調波の振幅比。
/// @return ワールド XZ の高さ勾配 (∂h/∂x, ∂h/∂z)。
/// @note WaveMeshFade は海サイズの水面で 4 本すべてを寝かせる。変位を諦めるのは正しいが、法線まで
/// @note 消すと «空を映すだけの板» になる。波長がピクセルより十分大きい間は傾きとして返せる。
/// @note 傾きの上限 1.2 (約 50 度) は安全弁。ak > 1 の波は本来砕けており、変位を伴わないここでは
/// @note そのままの傾きを出すと «壁» に見える。
/// @note 位相の画面周波数は dot(D, dWorldDxy)/波長。直交した伸びは位相を変えず、両画面軸が 0.5 周期/pixel 未満なら Nyquist を満たす。
/// @note 群の斜めの包絡も位相幅へ含め、基本波に直交した長い footprint で群だけが alias することを防ぐ。
/// @note セル幅の補間後に非線形fadeを再評価すると、頂点法線の保持率と残差の和が1を超える。各頂点のfadeを法線と同じ重みで補間する。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems Chapter 1, directional wave phase and surface derivatives
float2 WaterResidualWaveSlope(float2 worldXZ, float time, float2 dWorldDx, float2 dWorldDy,
                              float4 meshWaveFade[4], out float lostVariance, float crestDetail = 0.0f)
{
    float2 slope = float2(0.0f, 0.0f);
    lostVariance = 0.0f;
    float2 rotation, scales;
    ResolveWaveSpread(rotation, scales);
    float grouping = saturate(g_reflectParams.y);
    float3 depthState = WaterDepthState(worldXZ).vertical;

    [loop]
    for (int i = 0; i < WaterWaveCount(); ++i)
    {
        if (g_waveDir[i].w <= 0.0f) continue;
        float wavelength = max(g_waveParams[i].y, 1.0e-4f);
        float meshFade = saturate(meshWaveFade[i >> 2][i & 3]);
        float residual = 1.0f - meshFade;
        if (residual <= 0.001f) continue;
        [unroll]
        for (int part = 0; part < 2; ++part)
        {
            float2 D = normalize(g_waveDir[i].xy);
            float amplitudeScale;
            ApplyWaveSpread(i, part, rotation, scales, D, amplitudeScale);
            if (amplitudeScale <= 0.001f) continue;
            float phaseFootprint = max(abs(dot(D, dWorldDx)), abs(dot(D, dWorldDy)));
            const float groupRatio = WaterWaveGroupRatio(i);
            if (grouping > 0.0001f) {
                const float2 groupDir = WaterWaveGroupDirection(i, D);
                phaseFootprint += groupRatio * max(abs(dot(groupDir, dWorldDx)), abs(dot(groupDir, dWorldDy)));
            }
            float pixelFade = saturate(1.0f - phaseFootprint * 2.0f / wavelength);
            const float k = g_waveParams[i].w;
            const float baseAmplitude = g_waveParams[i].x * amplitudeScale;
            const float amplitudeK = baseAmplitude * depthState.x * k;
            const float harmonic = crestDetail * residual;
            const float harmonicFade = saturate(2.0f * pixelFade - 1.0f);
            [branch] if (pixelFade > 0.0f) {
                const float omega = g_waveParams[i].z;
                const float3 group = WaveGroupEnvelope(i, D, k, omega, worldXZ, time);
                const float phase = k * dot(D, worldXZ) - omega * time + WaterWavePhase(i);
                float sine, cosine;
                sincos(phase, sine, cosine);
                const float visibleWave = residual * pixelFade;
                const float visibleHarmonic = harmonic * harmonicFade;
                /// @note h = sin(phase) - harmonic * cos(2 * phase) で山を絞り、谷を広げる。
                /// @see https://www.fhwa.dot.gov/engineering/hydraulics/pubs/hif19059.pdf HEC-25, §5.1, Fig. 5.4.
                const float height = visibleWave * sine + visibleHarmonic * (2.0f * sine * sine - 1.0f);
                const float phaseSlope = cosine * (visibleWave + 4.0f * visibleHarmonic * sine);
                const float2 amplitudeGradient = group.x * depthState.yz + depthState.x * group.yz;
                slope += (amplitudeK * group.x * phaseSlope) * D + (baseAmplitude * height) * amplitudeGradient;
            }
            /// @note cos の平均二乗は 1/2。方向の trace を 2 軸へ均し、群の平均二乗と保持した振幅の二乗を含める。
            const float retained = meshFade + residual * pixelFade;
            const float groupEnergy = 1.0f + 0.5f * grouping * grouping * (1.0f + groupRatio * groupRatio);
            const float harmonicLoss = 4.0f * harmonic * harmonic * (1.0f - harmonicFade * harmonicFade);
            lostVariance += 0.25f * amplitudeK * amplitudeK * groupEnergy * (1.0f - retained * retained + harmonicLoss);
        }
    }

    float len = length(slope);
    if (len > 1.2f) slope *= 1.2f / len;
    return slope;
}

/// @brief 接空間法線。tangent = +X(world), binormal = +Z(world) なので xy がそのまま world XZ に対応する。
/// @param geoNormal 頂点法線 (ワールド)。うねりの斜面を読み取り、さざ波の分布へ反映する。
/// @param detailGain 流れの場が持ち上げるさざ波の強度倍率 (1 起点)。
/// @return xyz = 接空間法線, w = 強度込みの落とした傾きの分散 (specular AA で粗さへ回す)。
/// @note うねりの勾配でさざ波の座標をずらす (領域ワープ)。波が «さざ波を運ぶ» ので、ワープが無いと
/// @note うねりとさざ波が別々の層として滑って見える。ワープ量は画面上でゆっくり変わるため、
/// @note 異方フィルタへ渡す微分には含めていない。
float4 SampleWaterNormal(float2 worldXZ, float2 uv, float2 flowDir, float2 flowTravel, float time, float footprint,
                          float2 dWorldDx, float2 dWorldDy, float3 geoNormal, float detailGain,
                          float4 meshWaveFade[4], float2 baseWorldXZ, float2 dBaseDx, float2 dBaseDy, bool splineSurface,
                          float normalStrength, float detailStrength, bool followCurrent, WaterCurrentAdvection current,
                          float crestDetail = 0.0f)
{
    float2 swellSlope = -geoNormal.xz / max(geoNormal.y, 0.2f);
    float2 warpOffset = swellSlope * g_flowParams.w;
    if (splineSurface) {
        /// @note 正のworld流速へ模様を運ぶため、回転・異方変換の前に逆向きへ座標を遡る。
        warpOffset -= flowTravel;
        flowTravel = 0.0f;
    }

    /// @note Use the warped coordinate's footprint for mip selection and keep the unwarped footprint as a lower bound.
    dWorldDx += ddx(warpOffset);
    dWorldDy += ddy(warpOffset);
    float detailFootprint = max(footprint, max(length(dWorldDx), length(dWorldDy)));
    float3 detail = WaterDetailGradient(worldXZ + warpOffset, flowDir, flowTravel, time, detailFootprint,
        dWorldDx, dWorldDy, followCurrent, current);
    /// @note 風上を向いた斜面ほどさざ波が立ち、風下は凪ぐ。一様に散らすとうねりが «ただの起伏» に見える。
    float windward = saturate(0.5f + dot(swellSlope, flowDir));
    float detailScale = max(detailStrength, 0.0f) * lerp(0.55f, 1.35f, windward) * max(detailGain, 0.0f);
    float2 grad = detail.xy * detailScale;

    float residualVariance = 0.0f;
    const float seam = WaterfallWaveWeight(baseWorldXZ);
    grad += WaterResidualWaveSlope(baseWorldXZ, time, dBaseDx, dBaseDy, meshWaveFade, residualVariance, crestDetail) * seam;
    residualVariance *= seam * seam;

    float2 ripple = DecodeWaterRipple(g_rippleTex.Sample(g_samplerClamp, uv).rgb).rg
                 * WaterLocalDataWeight(uv);
    /// @note A flat interaction normal contributes zero slope. Bound the conversion at grazing slopes.
    /// @see https://jcgt.org/published/0009/03/04/ Mikkelsen (2020), surface gradient composition.
    float rippleNormalZ = sqrt(saturate(1.0f - dot(ripple, ripple)));
    grad -= 0.5f * ripple / max(rippleNormalZ, 0.25f);
    float3 waveNormal = normalize(float3(-grad.x, -grad.y, 1.0f));
    return float4(normalize(lerp(float3(0.0f, 0.0f, 1.0f), waveNormal, normalStrength)),
                  (detail.z * detailScale * detailScale + residualVariance)
                      * normalStrength * normalStrength);
}

float4 SampleWaterNormal(float2 worldXZ, float2 uv, float2 flowDir, float2 flowTravel, float time, float footprint,
                          float2 dWorldDx, float2 dWorldDy, float3 geoNormal, float detailGain,
                          float4 meshWaveFade[4], float2 baseWorldXZ, float2 dBaseDx, float2 dBaseDy, bool splineSurface,
                          float normalStrength, float detailStrength)
{
    return SampleWaterNormal(worldXZ, uv, flowDir, flowTravel, time, footprint, dWorldDx, dWorldDy,
        geoNormal, detailGain, meshWaveFade, baseWorldXZ, dBaseDx, dBaseDy, splineSurface,
        normalStrength, detailStrength, false, (WaterCurrentAdvection)0);
}

float4 SampleWaterNormal(float2 worldXZ, float2 uv, float2 flowDir, float2 flowTravel, float time, float footprint,
                          float2 dWorldDx, float2 dWorldDy, float3 geoNormal, float detailGain,
                          float4 meshWaveFade[4], float2 baseWorldXZ, float2 dBaseDx, float2 dBaseDy, bool splineSurface)
{
    return SampleWaterNormal(worldXZ, uv, flowDir, flowTravel, time, footprint, dWorldDx, dWorldDy,
        geoNormal, detailGain, meshWaveFade, baseWorldXZ, dBaseDx, dBaseDy, splineSurface,
        g_normalParams.w, g_detailParams.z);
}

#endif
