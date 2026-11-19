/// @file    WaterSurfaceFlow.hlsli
/// @brief   流れ場の標本化と水面の変位、局所流速、泡の逆追跡。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_SURFACE_FLOW_HLSLI
#define FBZZ_WATER_SURFACE_FLOW_HLSLI

/// @note WaterSurface.hlsli から読み込む。
/// @note 形の Gaussian の実効半径 / 影響半径。C++ の kWaterSurfaceFlowShapeRatio と同じ値。
static const float kWaterFlowShapeRatio = 0.4f;
/// @note 渦と吸い込みの «目» とみなす半径の割合。ここに泡を足す。
static const float kWaterFlowEyeRatio = 0.3f;
/// @note この流速 [m/s] でさざ波の向きと質感が «一様な流れ» と 1:1 で釣り合う。
/// @note 流速そのものではなく «重み» なので、単位を持つ .mat のつまみにはしない。
/// @warning C++ の kWaterSurfaceFlowReference と同じ値であること。
static const float kWaterFlowReference = 4.0f;
/// @note Uniform の «風の足跡» と Curl の «ざわつき» がさざ波の強度を持ち上げる上限。
static const float kWaterWindDetailGain = 0.6f;
static const float kWaterCurlDetailGain = 0.5f;

/// @brief クォータニオン q でベクトル v を回す。CPU の math::Quaternion::operator* と同じ式。
float3 WaterQuatRotate(float4 q, float3 v)
{
    const float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

/// @brief 速度場アトラスからタイル 1 枚ぶんを引く。
/// @param local 場のローカル正規化座標 [0,1]³。
/// @note Z を手で補間するのは、タイルが Z 方向に積んであり、ハードウェアのトリリニアだと
/// @note 境界で隣の場が混ざるため。
/// @warning ParticleGpuSim.cs.hlsl の SampleVelocityField と同じ式であること。片方だけ変えると、
/// @note 同じ 速度場 PNG で粒子と水面が別の流れを見る。
float3 WaterSampleVelocityField(uint tile, float3 local, float maxMagnitude)
{
    uint atlasW, atlasH, atlasD;
    g_velocityAtlas.GetDimensions(atlasW, atlasH, atlasD);
    const float tileTexels = (float)WATER_VELOCITY_FIELD_TILE;

    const float z  = local.z * tileTexels - 0.5f;
    const float z0 = clamp(floor(z), 0.0f, tileTexels - 1.0f);
    const float z1 = min(z0 + 1.0f, tileTexels - 1.0f);
    const float t  = saturate(z - z0);

    const float base = (float)tile * tileTexels;
    const float3 a = g_velocityAtlas.SampleLevel(
        g_samplerClamp, float3(local.xy, (base + z0 + 0.5f) / (float)atlasD), 0.0f).rgb;
    const float3 b = g_velocityAtlas.SampleLevel(
        g_samplerClamp, float3(local.xy, (base + z1 + 0.5f) / (float)atlasD), 0.0f).rgb;
    /// @note 復元は CPU の QuantizeVectorFieldValue と同じ式。
    return (lerp(a, b, t) * 2.0f - 1.0f) * maxMagnitude;
}

/// @brief 焼いた場 1 枚のワールド流速 [m/s]。範囲外はゼロ。
/// @param baseXZ **変位前の** ワールド XZ。高さは g_surfaceFlowE[i].w で基準面へ固定する。
/// @note 高さを «波を乗せた後の Y» で引くと «高さを高さで引く» 循環になる。
/// @warning WaterComponent::WaterBakedDisplacement と同じ標本化であること。
float3 WaterBakedVelocity(int i, float2 baseXZ)
{
    if (g_surfaceFlowC[i].z < 0.0f) return float3(0.0f, 0.0f, 0.0f);
    float3 toPoint = float3(baseXZ.x - g_surfaceFlowA[i].x,
                            g_surfaceFlowE[i].w,
                            baseXZ.y - g_surfaceFlowA[i].y);
    float4 invRot = g_surfaceFlowD[i];
    float3 uvw = WaterQuatRotate(invRot, toPoint) / g_surfaceFlowE[i].xyz * 0.5f + 0.5f;
    if (any(uvw < 0.0f) || any(uvw > 1.0f)) return float3(0.0f, 0.0f, 0.0f);

    float3 value = WaterSampleVelocityField((uint)g_surfaceFlowC[i].z, uvw, g_surfaceFlowC[i].w);
    /// @note 場は «場のローカル» で焼かれているのでワールドへ戻す。strength は無次元の倍率。
    return WaterQuatRotate(float4(-invRot.xyz, invRot.w), value) * g_surfaceFlowB[i].x;
}

/// @brief 焼いた場 1 枚が作るへこみ [m] (0 以下)。Bernoulli を XZ 成分だけに掛ける。
/// @warning WaterComponent::WaterBakedDisplacement と同じ式であること。
float WaterBakedDisplacement(int i, float2 baseXZ)
{
    float cap = abs(g_surfaceFlowA[i].w);
    if (cap <= 0.0001f) return 0.0f;
    float3 v = WaterBakedVelocity(i, baseXZ);
    return -min(dot(v.xz, v.xz) / 19.6f, cap);
}

/// @brief 中心を持つ流れ 1 本が作る変位 [m] と、その水平勾配。
/// @param worldXZ **変位後の** ワールド XZ。形は水平変位を持たないので逆写像に入れない。
/// @return x = 変位 [m] (穴は負・山は正), yz = (∂h/∂x, ∂h/∂z)。
/// @note 形を Gaussian にするのは C¹ 連続だから。(1 - r/R)^p は r = R で折れ、頂点法線が
/// @note 1 セルだけ跳ねて縁に輪が出る。
/// @note 勾配は Gerstner 変位を通す連鎖律を無視している (群の包絡と同じ扱い)。形の波数は
/// @note うねりよりずっと小さく、法線が数 % ずれるだけで絵には出ない。
/// @warning WaterComponent::WaterRadialDisplacement と同じ式であること。
float3 WaterRadialDisplacement(int i, float2 worldXZ)
{
    float4 a = g_surfaceFlowA[i];
    if (abs(a.w) <= 0.0001f || a.z <= 0.0001f) return float3(0.0f, 0.0f, 0.0f);
    float  s = a.z * kWaterFlowShapeRatio;
    float2 d = worldXZ - a.xy;
    float  h = a.w * exp(-dot(d, d) / (s * s));
    return float3(h, -2.0f * h * d / (s * s));
}

/// @brief World-anchored swell height and XZ gradient inside a Uniform field.
/// @note The squared compact envelope keeps the displacement and slope continuous at the boundary.
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1, directional waves and deep-water phase speed.
/// @warning Keep the phase and envelope in sync with WaterComponent::WaterSwellDisplacement.
float3 WaterSwellDisplacement(int i, float2 worldXZ, float time, float2 meshCellSize)
{
    const float4 field = g_surfaceFlowA[i];
    const float2 band = g_surfaceFlowE[i].xy;
    if (field.w <= 0.0f || field.z <= 0.0f || band.x <= 0.0f || band.y <= 0.0f)
        return 0.0f;
    const float invRadius = 1.0f / field.z;
    const float2 offset = worldXZ - field.xy;
    const float2 normalized = offset * invRadius;
    const float envelope = 1.0f - dot(normalized, normalized);
    if (envelope <= 0.0f) return 0.0f;
    const float lodFade = min(1.0f, WaveMeshFade(min(band.x, 2.0f * field.z), meshCellSize) / band.y);
    if (lodFade <= 0.0f) return 0.0f;
    const float2 direction = g_surfaceFlowC[i].xy;
    const float2 side = float2(-direction.y, direction.x);
    const float across = dot(offset, side);
    const float waveNumber = 6.28318530718f / band.x;
    const float phase = waveNumber * (dot(offset, direction) + 0.18f * across * (across * invRadius))
                      - sqrt(9.8f * waveNumber) * time;
    float sine, cosine;
    sincos(phase, sine, cosine);
    const float amplitude = field.w * lodFade;
    const float shape = envelope * envelope;
    const float2 shapeGradient = -4.0f * envelope * normalized * invRadius;
    const float2 phaseGradient = waveNumber * (direction + side * (0.36f * across * invRadius));
    return float3(amplitude * shape * cosine,
        amplitude * (shapeGradient * cosine - shape * sine * phaseGradient));
}

/// @brief Compact vortex bowl, analytic slope and spiral crest foam.
/// @note Angular frequency is an integer so crossing the atan2 branch cut does not introduce a seam.
/// @warning Keep the bowl and spiral in sync with WaterComponent::WaterSwellDisplacement.
float4 WaterVortexSwellDisplacement(int i, float2 worldXZ, float time, float2 meshCellSize)
{
    const float4 field = g_surfaceFlowA[i];
    const float2 band = g_surfaceFlowE[i].xy;
    if (field.w >= 0.0f || field.z <= 0.0f || band.y <= 0.0f) return 0.0f;
    const float diameter = 2.0f * field.z;
    /// @note The broad depression can remain resolved after the finer spiral falls below the mesh bandwidth.
    const float bowlFade = min(1.0f, WaveMeshFade(diameter, meshCellSize) / band.y);
    if (bowlFade <= 0.0f) return 0.0f;
    const float invRadius = 1.0f / field.z;
    const float2 offset = worldXZ - field.xy;
    const float2 normalized = offset * invRadius;
    const float radiusSquared = dot(normalized, normalized);
    const float envelope = 1.0f - radiusSquared;
    if (envelope <= 0.0f) return 0.0f;
    const float bowl = envelope * envelope * envelope;
    const float2 radiusGradient = 2.0f * normalized * invRadius;
    const float2 bowlGradient = -3.0f * envelope * envelope * radiusGradient;
    const float3 bowlDisplacement = (field.w * bowlFade) * float3(bowl, bowlGradient);
    const float spiralFade = min(1.0f, WaveMeshFade(min(band.x, diameter), meshCellSize) / band.y);
    if (radiusSquared <= 1.0e-10f || spiralFade <= 0.0f) return float4(bowlDisplacement, 0.0f);
    const float ring = 6.75f * radiusSquared * envelope * envelope;
    const float spiral = ring * (1.0f - bowl);
    const float2 ringGradient = 6.75f * envelope * (envelope - 2.0f * radiusSquared) * radiusGradient;
    const float2 spiralGradient = ringGradient * (1.0f - bowl) - ring * bowlGradient;
    const float spin = (asuint(g_surfaceFlowB[i].x) & 0x80000000u) != 0u ? -1.0f : 1.0f;
    const float waveNumber = 6.28318530718f / band.x;
    const float unitRadius = sqrt(radiusSquared);
    const float phase = waveNumber * unitRadius * field.z + 2.0f * spin * atan2(offset.y, offset.x)
                      + sqrt(9.8f * waveNumber) * time;
    const float2 phaseGradient = waveNumber * normalized / unitRadius
        + (2.0f * spin * invRadius / radiusSquared) * float2(-normalized.y, normalized.x);
    float sine, cosine;
    sincos(phase, sine, cosine);
    const float crest = saturate(cosine);
    const float3 spiralDisplacement = (-0.22f * field.w * spiralFade)
        * float3(spiral * cosine, spiralGradient * cosine - spiral * sine * phaseGradient);
    return float4(bowlDisplacement + spiralDisplacement, spiral * crest * crest * crest * spiralFade);
}

/// @brief PS が使う «その点の流れと質感»。
struct WaterFlowSample
{
    float2 direction;  ///< @brief さざ波・きらめき・泡が流れる向き (正規化)
    float2 localVelocity; ///< @brief 基流を除いた場のワールド XZ 流速 [m/s]。
    float  eye;        ///< @brief 渦と吸い込みの目への近さ [0,1]。泡へ足す
    float  ring;       ///< @brief 湧き出しの縁への近さ [0,1]。泡の輪
    float  detailGain; ///< @brief さざ波の強度倍率 (1 起点)
    float  chop;       ///< @brief 泡のムラ [0,1]
};

/// @brief その点の流れの向きと、型ごとの質感を集める。
/// @param baseXZ 変位前のワールド XZ (Baked の標本位置)。
/// @note 減衰 pow(saturate(1 - r/radius), falloffPower) は FlowFieldEval の ResolveInfluence と
/// @note 同じ式。中心と同じ高さの点なら CPU の流速と一致する。
/// @note 矩形は向きのみに使用する。スプラインは localVelocity で短時間だけ模様を逆追跡する。
/// @note Curl は向きを曲げない。ピクセルごとにカールノイズを回すことになるうえ、乱流が出したい
/// @note のは «向き» ではなく «ざわつき» なので質感だけに落とす。
WaterFlowSample SampleWaterSurfaceFlow(float2 worldXZ, float2 baseXZ)
{
    WaterFlowSample s;
    s.direction  = g_flowParams.xy;
    s.eye        = 0.0f;
    s.ring       = 0.0f;
    s.detailGain = 1.0f;
    s.chop       = 0.0f;

    float2 dir = g_flowParams.xy;
    int count = (int)g_waveShapeParams.y;

    [loop]
    for (int i = 0; i < WATER_SURFACE_FLOW_COUNT; ++i)
    {
        if (i >= count) continue;
        int kind = (int)g_surfaceFlowB[i].z;

        if (kind == WFF_BAKED)
        {
            float3 v = WaterBakedVelocity(i, baseXZ);
            dir += v.xz / kWaterFlowReference;
            s.detailGain += kWaterWindDetailGain
                          * saturate(length(v.xz) / kWaterFlowReference);
            continue;
        }

        float radius = g_surfaceFlowA[i].z;
        if (radius <= 0.0001f) continue;
        float2 d = worldXZ - g_surfaceFlowA[i].xy;
        float  r = length(d);
        if (r >= radius) continue;

        float influence = pow(saturate(1.0f - r / radius), g_surfaceFlowB[i].y);
        float speed = g_surfaceFlowB[i].x;
        float chop  = g_surfaceFlowB[i].w * influence;

        if (kind == WFF_UNIFORM)
        {
            dir += g_surfaceFlowC[i].xy * (speed * influence / kWaterFlowReference);
            /// @note «風の足跡»。局所の風はうねりを育てないが、さざ波はその場で立つ。
            s.detailGain += kWaterWindDetailGain * chop;
            continue;
        }
        if (kind == WFF_CURL)
        {
            s.detailGain += kWaterCurlDetailGain * chop;
            s.chop = max(s.chop, chop);
            continue;
        }
        if (r <= 1.0e-4f) continue;

        if (kind == WFF_VORTEX)
        {
            /// @note cross(up, d) の XZ 成分。符号は speed が持つ (回転軸の y の符号)。
            dir += float2(d.y, -d.x) / r * (speed * influence / kWaterFlowReference);
            s.eye = max(s.eye, saturate(1.0f - r / (radius * kWaterFlowEyeRatio)));
            continue;
        }
        /// @note Sink は内向き、Source は外向き。
        float sign = (kind == WFF_SOURCE) ? 1.0f : -1.0f;
        dir += d / r * (sign * speed * influence / kWaterFlowReference);
        if (kind == WFF_SINK)
        {
            /// @note 排水口の中心も泡立つ。回る水ほどではないので 0.6 倍。
            s.eye = max(s.eye, saturate(1.0f - r / (radius * kWaterFlowEyeRatio)) * 0.6f);
        }
        else
        {
            /// @note 湧き上がった水は縁で広がってぶつかる。influence は中心がいちばん強いので
            /// @note 輪には使えない。半径の 0.55〜1.0 に帯を立て、強さは流速から取る。
            float t = r / radius;
            s.ring = max(s.ring, smoothstep(0.55f, 0.80f, t) * (1.0f - smoothstep(0.80f, 1.0f, t))
                                 * saturate(abs(speed) / kWaterFlowReference));
        }
    }

    s.localVelocity = (dir - g_flowParams.xy) * kWaterFlowReference;
    float len = length(dir);
    s.direction = len > 1.0e-4f ? dir / len : g_flowParams.xy;
    return s;
}

/// @return 残り泡を運ぶワールド XZ 速度 [m/s]。材質の flowSpeed は見た目の漂流成分。
float2 WaterFoamDriftVelocity(WaterFlowSample flow)
{
    const float2 velocity = flow.localVelocity
        + g_flowParams.xy * (g_waveDepthParams.w + g_refractionFlowParams.y);
    return velocity * min(1.0f, 4.0f / max(length(velocity), 1.0e-4f));
}

/// @note 短い泡の寿命内で速度を一定とし、履歴を現在の点から上流へ逆追跡する。
/// @see https://doi.org/10.1145/311535.311548 Stam (1999), Stable Fluids, semi-Lagrangian advection.
float3 WaterFoamBacktrace(float3 basePos, float2 velocity, float age)
{
    const float2 travel = velocity * age;
    basePos.xz -= travel * min(1.0f, 8.0f / max(length(travel), 1.0e-4f));
    return basePos;
}

#endif
