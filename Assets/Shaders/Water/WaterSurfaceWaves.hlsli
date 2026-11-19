/// @file    WaterSurfaceWaves.hlsli
/// @brief   Gerstner 波、波群、交差する波頭と白波の幾何信号。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_SURFACE_WAVES_HLSLI
#define FBZZ_WATER_SURFACE_WAVES_HLSLI

/// @note WaterSurface.hlsli から読み込む。
/// @note 群の向きを波から傾ける角 (0.55 rad) の cos / sin。WaterComponent と同じ値。
static const float kWaveGroupCos = 0.852525f;
static const float kWaveGroupSin = 0.522687f;

float WaterWaveGroupRatio(int index)
{
    return 0.09f + 0.035f * (float)(index & 3);
}

float2 WaterWaveGroupDirection(int index, float2 D)
{
    const float sign = (index & 1) ? -1.0f : 1.0f;
    return float2(D.x * kWaveGroupCos - D.y * sign * kWaveGroupSin,
                  D.x * sign * kWaveGroupSin + D.y * kWaveGroupCos);
}

/// @return x=振幅の包絡、yz=変位前の world XZ に対する勾配。
/// @note 群の角周波数は r*omega/2。深水波の群速度は位相速度の半分。
/// @warning 包絡の値は WaterComponent::WaveGroupEnvelope と同期する。
/// @see Docs/design/water-waves.md
float3 WaveGroupEnvelope(int index, float2 D, float k, float omega, float2 worldXZ, float time)
{
    const float depth = saturate(g_reflectParams.y);
    if (depth <= 0.0001f) return float3(1.0f, 0.0f, 0.0f);

    const float ratio = WaterWaveGroupRatio(index);
    const float2 groupDir = WaterWaveGroupDirection(index, D);
    const float groupK = ratio * k;
    const float phase = groupK * dot(groupDir, worldXZ) - ratio * omega * 0.5f * time
                      + WaterWavePhase(index);
    return float3(1.0f + depth * sin(phase), depth * cos(phase) * groupK * groupDir);
}

/// @note Keep the crest gain and the height bound in sync with WaterComponent.
static const float kWaterCrossingCrestGain = 0.85f;

struct WaterCrossingCrest {
    float3 moment;
    float3 derivativeX;
    float3 derivativeZ;
};

float WaterMaximumWaveRise()
{
    return g_reflectParams.w * (1.0f + (g_farOceanParams.x > 0.5f ? kWaterCrossingCrestGain : 0.0f));
}

/// @return x is additional height in meters and yz are its undisplaced world XZ derivatives.
/// @note S^2 - dot(V,V) is the sum of positive crest products weighted by 2 * (1 - dot(D_i,D_j)).
/// @see https://doi.org/10.1017/jfm.2018.886 McAllister et al. (2019), crest focusing in crossing seas.
float3 WaterCrossingCrestDisplacement(WaterCrossingCrest crest, float3 attenuation)
{
    const float3 signedMoment = float3(crest.moment.x, -crest.moment.yz);
    const float coincidence = dot(crest.moment, signedMoment);
    if (coincidence <= 0.0f) return 0.0f;

    const float scale = kWaterCrossingCrestGain / max(g_reflectParams.w, 0.01f);
    const float factor = scale * attenuation.x;
    const float2 halfDerivative = float2(dot(signedMoment, crest.derivativeX), dot(signedMoment, crest.derivativeZ));
    const float2 slope = 2.0f * factor * (attenuation.x * halfDerivative + coincidence * attenuation.yz);
    return float3(factor * attenuation.x * coincidence, slope);
}

/// @brief deep-water Gerstner 波を 1 本評価し、同時に解析微分で TBN を更新する。
/// @param fade  頂点グリッドで刻めない波を寝かせる係数。振幅は更に群の包絡が掛かる。
/// @note 振幅の包絡と浅瀬の減衰が変化するので、両方の勾配を含める。
/// @warning 無効な波は normalize より先に除外する。ゼロ方向を正規化すると NaN が伝播する。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2.3, Gerstner Waves.
float3 GerstnerDisplace(int index, float4 dirData, float4 params, float3 pos, float time, float fade,
                        WaterWaveDepthState depthState, inout float3 tangent, inout float3 binormal,
                        bool crossingEnabled, inout WaterCrossingCrest crossing)
{
    if (dirData.w <= 0.0f || fade <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    float2 D = normalize(dirData.xy);
    float  Q = saturate(dirData.z);
    const float amplitude = params.x * fade;
    const float3 group = WaveGroupEnvelope(index, D, params.w, params.z, pos.xz, time);
    const float baseA = amplitude * group.x;
    float  k = params.w;
    float  omega = params.z;
    float  phi = k * dot(D, pos.xz) - omega * time + WaterWavePhase(index);
    float  s = sin(phi);
    float  c = cos(phi);

    [branch]
    if (crossingEnabled) {
        const float positiveCrest = max(s, 0.0f);
        const float crestWeight = amplitude * positiveCrest * positiveCrest;
        const float2 crestGradient = crestWeight * group.yz + 2.0f * baseA * positiveCrest * k * c * D;
        const float3 directionMoment = float3(1.0f, D);
        crossing.moment += directionMoment * (crestWeight * group.x);
        crossing.derivativeX += directionMoment * crestGradient.x;
        crossing.derivativeZ += directionMoment * crestGradient.y;
    }

    const float2 push = Q * baseA * D;
    const float2 horizontalAmplitudeGradient = group.x * depthState.horizontal.yz
        + depthState.horizontal.x * group.yz;
    const float2 verticalAmplitudeGradient = group.x * depthState.vertical.yz
        + depthState.vertical.x * group.yz;
    const float2 horizontalSlope = c * horizontalAmplitudeGradient
        - k * s * depthState.horizontal.x * group.x * D;
    const float2 verticalSlope = s * verticalAmplitudeGradient
        + k * c * depthState.vertical.x * group.x * D;
    /// @note 振幅の空間減衰には積の微分が必要。浅瀬の勾配を落とすとCPUの法線と異なる。
    const float2 horizontalAmplitude = Q * amplitude * D;
    tangent += float3(horizontalAmplitude.x * horizontalSlope.x, amplitude * verticalSlope.x,
        horizontalAmplitude.y * horizontalSlope.x);
    binormal += float3(horizontalAmplitude.x * horizontalSlope.y, amplitude * verticalSlope.y,
        horizontalAmplitude.y * horizontalSlope.y);

    const float2 horizontal = push * (depthState.horizontal.x * c);
    return float3(horizontal.x, baseA * depthState.vertical.x * s, horizontal.y);
}

/// @brief 方向広がりの成分ごとの «向きと振幅倍率»。
///
/// @param component 0 = 主成分, 1 = 伴走成分。
/// @param rotation  伴走成分を回す量。x=cos, y=sin。呼び出し側でループの外に出しておく。
/// @note 実海面の波は 1 方向へ揃わず狭い方向スペクトルを持つ。同じ波数で向きだけ違う波を重ねると、
/// @note 波頭が有限の長さに切れる (short-crested sea)。«無限に長い直線の波頭» が消える。
/// @note 振幅は main^2 + comp^2 = 1 で分ける。分けないと spread を上げるだけで海が高くなる。
/// @warning WaterComponent::ApplyWaveSpread と同じ式であること。片方だけ変えると、
/// @note 見えている波と浮力が別の水面になる。
void ApplyWaveSpread(int index, int component, float2 rotation, float2 scales,
                     inout float2 direction, out float amplitudeScale)
{
    if (component == 0) { amplitudeScale = scales.x; return; }
    amplitudeScale = scales.y;
    float sign = (index & 1) ? -1.0f : 1.0f;
    direction = float2(direction.x * rotation.x - direction.y * sign * rotation.y,
                       direction.x * sign * rotation.y + direction.y * rotation.x);
}

/// @brief 方向広がりの «回す量 (cos, sin)» と «主成分 / 伴走成分の振幅倍率» を一度だけ求める。
/// @note 波ごとに三角関数を回さないためにループの外へ括り出す。回す向きは波の番号で交互に
/// @note 入れ替わるので、回転そのものは 2 種類しかない。
void ResolveWaveSpread(out float2 rotation, out float2 scales)
{
    float spread = saturate(g_waveShapeParams.x);
    float energy = saturate(0.40f * spread);
    scales = float2(sqrt(1.0f - energy), sqrt(energy));
    float angle = spread * 0.9f;
    rotation = float2(cos(angle), sin(angle));
}

struct WaterCrestSample
{
    float3 displacement; ///< @brief ワールド XYZ の Gerstner 変位 [m]。
    float compression; ///< @brief 水平ヤコビアンから得た圧縮 [0,1]。
    float crossingRise; ///< @brief 交差波による追加の高さ [m]。
};

/// @brief 指定時刻の変位 [m] と水平圧縮を同じ波評価から求める。
/// @param basePos 変位«前»のワールド位置。Gerstner の位相はここで取る。
/// @note 位相の sin を高さとヤコビアンで共有し、履歴の追加の三角関数を避ける。
WaterCrestSample WaterWaveCrestAt(float3 basePos, float time, float2 meshCellSize,
                                 WaterWaveDepthState depthState, bool crossingEnabled)
{
    float3 tangent = float3(1.0f, 0.0f, 0.0f);
    float3 binormal = float3(0.0f, 0.0f, 1.0f);
    float3 displacement = 0.0f;
    WaterCrossingCrest crossing = (WaterCrossingCrest)0;
    float2 rotation, scales;
    ResolveWaveSpread(rotation, scales);

    [loop]
    for (int i = 0; i < WaterWaveCount(); ++i)
    {
        if (g_waveDir[i].w <= 0.0f) continue;
        float fade = WaveMeshFade(g_waveParams[i].y, meshCellSize);
        if (fade <= 0.0f) continue;

        [unroll]
        for (int part = 0; part < 2; ++part)
        {
            float2 D = normalize(g_waveDir[i].xy);
            float  scale;
            ApplyWaveSpread(i, part, rotation, scales, D, scale);
            if (scale <= 0.001f) continue;

            const float4 direction = float4(D, g_waveDir[i].z, g_waveDir[i].w);
            float4 params = g_waveParams[i];
            params.x *= scale;
            displacement += GerstnerDisplace(i, direction, params, basePos, time, fade,
                depthState, tangent, binormal, crossingEnabled, crossing);
        }
    }
    const float crossingRise = crossingEnabled ? WaterCrossingCrestDisplacement(crossing, depthState.vertical).x : 0.0f;
    displacement.y += crossingRise;
    const float jacobian = tangent.x * binormal.z - tangent.z * binormal.x;
    WaterCrestSample result = {displacement, saturate(1.0f - jacobian), crossingRise};
    return result;
}

/// @brief 波高による泡の帯を作り、水平圧縮が強い波頭と折り畳みを増幅する。
/// @note 高さは波高上限で正規化するため、波の倍率を下げても泡の開始位置は変わらない。
/// @note 高さ項は見た目の制御で、実際の砕波の判定ではない。圧縮項は Gerstner の水平ヤコビアンから取る。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-18-using-vertex-texture-displacement GPU Gems 2, 18.2 Foam Generation: height threshold and foam coverage.
float WaterCrestFoamAmount(float2 state)
{
    float threshold = saturate(g_crestFoamParams.x);
    float heightFade = max(g_crestFoamParams.y, 0.001f);
    float highWater = smoothstep(threshold, threshold + heightFade, state.x);
    float compression = smoothstep(0.02f, 0.25f, state.y);
    float breaking = smoothstep(0.55f, 1.0f, state.y);
    return max(highWater * lerp(0.85f, 1.0f, compression), breaking * 0.85f);
}

/// @note 有限水域は同じ粒、Horizon は上流を過去へ三点評価し、泡の残留を近似する。
/// @note lifetime=0 では履歴を省く。履歴バッファやビュー別の更新は不要。
#define WATER_FOAM_TRAIL_TAPS 3

#endif
