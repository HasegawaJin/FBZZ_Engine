// FBZZ Engine
// Material/Effects/ParticleGpuSim.cs.hlsl | Compute Shader
// GPU パーティクルシミュレーション: スポーン + 物理積分 + 力場/ノイズ + 色/サイズ/スプライト補間
//
// dispatch: ceil(maxParticles / 64) × 1 × 1
// スロット:
//   b0  = GpuEmitterCB
//   t15 = StructuredBuffer<GpuSpawnEntry>  (スポーンバッファ DYNAMIC SRV)
//   u2  = RWStructuredBuffer<GpuParticle> (パーティクルプール DEFAULT)

#include "Common/Binding.hlsli"

// ---------- 構造体 --------------------------------------------------------

struct GpuParticle
{
    float3 position;
    float  size;
    float3 velocity;
    float  age;
    float4 color;
    float  lifetime;
    float  rotation;
    float  angularVelocity;
    float  spriteSeed;
    float4 uvRect;
};

struct GpuSpawnEntry
{
    float3 position;
    float  lifetime;
    float3 velocity;
    float  size;
    float4 colorStart;
    float4 colorEnd;
    float4 uvRect;
    float  rotation;
    float  angularVelocity;
    float  spriteSeed;
    float  pad1;
};

// 力場 1 本分 (48 bytes)。
// LAYOUT: RenderPassContext.hpp の GpuForceField と完全に一致させること。
struct GpuForceField
{
    float4 posRadius;   // xyz=ワールド位置, w=影響半径 (<=0 で無限)
    float4 dirStrength; // xyz=風向き/渦軸 (正規化済み), w=強さ
    float4 params;      // x=種類(ParticleForceFieldType), y=falloffPower,
                        // z=noiseFrequency, w=noiseSpeed
};

// ParticleForceFieldType (C++ 側 enum と数値を一致させること)
#define FF_WIND       0
#define FF_ATTRACT    1
#define FF_REPULSE    2
#define FF_VORTEX     3
#define FF_TURBULENCE 4
#define FF_DRAG       5

#define MAX_FORCE_FIELDS 8

// ---------- リソース -------------------------------------------------------

cbuffer GpuEmitterCB : register(b0)
{
    float3   gEmitterPos;
    float    gDeltaTime;
    float3   gGravity;
    uint     gMaxParticles;
    float4   gColorStart;
    float4   gColorEnd;
    uint     gSpawnCount;       // 今フレームにスポーンする粒子数
    uint     gSpawnOffset;      // リングバッファの書き込み開始インデックス
    float    gColorCurvePower;
    float    gVelocityDamping;
    float    gSizeStart;
    float    gSizeEnd;
    float    gSizeCurvePower;
    float    gPad0;
    uint     gSpriteColumns;
    uint     gSpriteRows;
    uint     gSpriteStartFrame;
    uint     gSpriteEndFrame;
    // ── ノイズモジュール + 力場 (末尾追加で既存オフセットを変えない) ──
    float    gTime;             // カールノイズのスクロールに使う経過時間
    float    gNoiseStrength;    // エミッター固有乱流の強さ (0 で無効)
    float    gNoiseFrequency;
    float    gNoiseSpeed;
    uint     gForceFieldCount;  // gForceFields の有効本数
    uint     gFlipbookMode;
    float    gFlipbookFramesPerSecond;
    float    gPad1;
    GpuForceField gForceFields[MAX_FORCE_FIELDS];
    float4   gCurveFlags;       // x=size, y=velocity, z=gradient, w=frameBlend
    float4   gSizeCurveKeys01;
    float4   gSizeCurveKeys23;
    float4   gVelocityCurveKeys01;
    float4   gVelocityCurveKeys23;
    float4   gGradientTimes;
    float4   gGradientColors[4];
    float4x4 gViewProjection;
    float    gScreenWidth;
    float    gScreenHeight;
    float    gDepthThickness;
    float    gDepthBounciness;
    uint     gDepthCollision;
    uint     gDepthResponse;
    float    gDepthDamping;
    float    gDepthPad;
};

StructuredBuffer<GpuSpawnEntry>   gSpawnBuffer : register(SB_GPU_SPAWN);
RWStructuredBuffer<GpuParticle>   gParticles   : register(UAV_GPU_PARTICLES);
Texture2D<float>                   gSceneDepth : register(TEX_DEPTH);

// ---------- カールノイズ (乱流ベクトルフィールド) ---------------------------
// 式は ParticlePass.cpp の同名関数と一致させること (CPU/GPU で挙動を揃える)。

// 整数ハッシュ (PCG 系)。格子点から再現可能な擬似乱数を作る。
uint PcgHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// 格子点 (整数座標) → [-1, 1] の擬似乱数値
float LatticeValue(int3 c)
{
    uint h = PcgHash((uint)c.x * 73856093u ^ (uint)c.y * 19349663u ^ (uint)c.z * 83492791u);
    return (float)h * (2.0f / 4294967295.0f) - 1.0f;
}

// 3D 値ノイズ [-1, 1]。8 格子点を smoothstep 重みでトリリニア補間する。
float ValueNoise3D(float3 p)
{
    float3 f = floor(p);
    int3   c = (int3)f;
    float3 t = p - f;
    // smoothstep フェード: 格子境界で勾配を連続にする
    t = t * t * (3.0f - 2.0f * t);
    float c000 = LatticeValue(c + int3(0, 0, 0));
    float c100 = LatticeValue(c + int3(1, 0, 0));
    float c010 = LatticeValue(c + int3(0, 1, 0));
    float c110 = LatticeValue(c + int3(1, 1, 0));
    float c001 = LatticeValue(c + int3(0, 0, 1));
    float c101 = LatticeValue(c + int3(1, 0, 1));
    float c011 = LatticeValue(c + int3(0, 1, 1));
    float c111 = LatticeValue(c + int3(1, 1, 1));
    float x00 = lerp(c000, c100, t.x);
    float x10 = lerp(c010, c110, t.x);
    float x01 = lerp(c001, c101, t.x);
    float x11 = lerp(c011, c111, t.x);
    float y0  = lerp(x00, x10, t.y);
    float y1  = lerp(x01, x11, t.y);
    return lerp(y0, y1, t.z);
}

// カールノイズ: 3 成分のベクトルポテンシャル ψ の回転 (∇×ψ) を中心差分で求める。
// WHY: 回転場は発散ゼロのため粒子が一点に溜まらず、煙・炎らしい滑らかな渦を作れる。
float3 CurlNoise(float3 p)
{
    // 各ポテンシャル成分は同じノイズを離れた位置からサンプリングして独立させる
    float3 p1 = p + 31.341f;
    float3 p2 = p - 47.853f;
    float3 p3 = p + 12.793f;
    const float eps = 0.25f;
    const float invTwoEps = 1.0f / (2.0f * eps);
    float3 dx = float3(eps, 0.0f, 0.0f);
    float3 dy = float3(0.0f, eps, 0.0f);
    float3 dz = float3(0.0f, 0.0f, eps);
    float dp1dy = (ValueNoise3D(p1 + dy) - ValueNoise3D(p1 - dy)) * invTwoEps;
    float dp1dz = (ValueNoise3D(p1 + dz) - ValueNoise3D(p1 - dz)) * invTwoEps;
    float dp2dx = (ValueNoise3D(p2 + dx) - ValueNoise3D(p2 - dx)) * invTwoEps;
    float dp2dz = (ValueNoise3D(p2 + dz) - ValueNoise3D(p2 - dz)) * invTwoEps;
    float dp3dx = (ValueNoise3D(p3 + dx) - ValueNoise3D(p3 - dx)) * invTwoEps;
    float dp3dy = (ValueNoise3D(p3 + dy) - ValueNoise3D(p3 - dy)) * invTwoEps;
    return float3(dp3dy - dp2dz, dp1dz - dp3dx, dp2dx - dp1dy);
}

// Turbulence / Noise モジュール共通のサンプル座標。時間スクロールは軸ごとに
// 速度を変え、場全体が一方向へ流れて見えないようにする (CPU 側と一致)。
float3 TurbulenceSamplePoint(float3 position, float frequency, float speed, float time)
{
    float scroll = time * speed;
    return position * frequency + float3(scroll, scroll * 0.35f, scroll * 0.7f);
}

// ---------- 力場 (ParticleForceField) --------------------------------------

// 力場を粒子速度へ適用する。式は ParticlePass.cpp の ApplyForceFields と一致させること。
void ApplyForceFields(float3 position, inout float3 velocity)
{
    [loop]
    for (uint fi = 0; fi < gForceFieldCount; ++fi)
    {
        GpuForceField f = gForceFields[fi];
        float3 toParticle = position - f.posRadius.xyz;
        float  radius     = f.posRadius.w;
        float  influence  = 1.0f;
        if (radius > 0.0f)
        {
            float dist = length(toParticle);
            if (dist >= radius) continue;
            // WHY: dist < radius により底は数学的に正だが、FXC は分岐条件を考慮せず
            // X3571 を出すため、abs で非負値であることを明示する。
            influence = pow(abs(1.0f - dist / radius), f.params.y);
        }
        float impulse   = f.dirStrength.w * influence * gDeltaTime;
        uint  fieldType = (uint)f.params.x;
        if (fieldType == FF_WIND)
        {
            velocity += f.dirStrength.xyz * impulse;
        }
        else if (fieldType == FF_ATTRACT || fieldType == FF_REPULSE)
        {
            float  dist = max(length(toParticle), 1.0e-4f);
            float3 dir  = toParticle / dist;
            velocity += dir * (fieldType == FF_REPULSE ? impulse : -impulse);
        }
        else if (fieldType == FF_VORTEX)
        {
            // 軸×粒子方向の外積 = 接線方向。軸周りに回す
            float3 tangent = cross(f.dirStrength.xyz, toParticle);
            float  len     = length(tangent);
            if (len > 1.0e-4f)
                velocity += tangent * (impulse / len);
        }
        else if (fieldType == FF_TURBULENCE)
        {
            velocity += CurlNoise(TurbulenceSamplePoint(
                position, f.params.z, f.params.w, gTime)) * impulse;
        }
        else if (fieldType == FF_DRAG)
        {
            // strength を減衰係数 [1/s] として扱う (velocityDamping と同じ式)
            velocity *= max(0.0f, 1.0f - impulse);
        }
    }
}

// 4キー線形カーブを評価する。xy/zwに(time,value)を2点ずつ格納する。
float EvaluateCurve4(float4 keys01, float4 keys23, float t)
{
    float2 keys[4] = { keys01.xy, keys01.zw, keys23.xy, keys23.zw };
    if (t <= keys[0].x) return keys[0].y;
    [unroll]
    for (uint i = 1; i < 4; ++i)
    {
        if (t <= keys[i].x)
        {
            float alpha = saturate((t - keys[i - 1].x) / max(keys[i].x - keys[i - 1].x, 1.0e-4f));
            return lerp(keys[i - 1].y, keys[i].y, alpha);
        }
    }
    return keys[3].y;
}

float4 EvaluateGradient4(float t)
{
    if (t <= gGradientTimes.x) return gGradientColors[0];
    [unroll]
    for (uint i = 1; i < 4; ++i)
    {
        if (t <= gGradientTimes[i])
        {
            float alpha = saturate((t - gGradientTimes[i - 1])
                / max(gGradientTimes[i] - gGradientTimes[i - 1], 1.0e-4f));
            return lerp(gGradientColors[i - 1], gGradientColors[i], alpha);
        }
    }
    return gGradientColors[3];
}

// ---------- カーネル -------------------------------------------------------

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= gMaxParticles) return;

    // このスレッドがスポーンスロットかどうかを判定する。
    // リングバッファ: [spawnOffset, spawnOffset + spawnCount) % maxParticles に新粒子を配置。
    uint relIdx = (i + gMaxParticles - gSpawnOffset) % gMaxParticles;
    bool isSpawnSlot = (relIdx < gSpawnCount);

    GpuParticle p;
    if (isSpawnSlot)
    {
        GpuSpawnEntry s = gSpawnBuffer[relIdx];
        p.position        = s.position;
        p.velocity        = s.velocity;
        p.size            = s.size;
        p.age             = 0.0f;
        p.lifetime        = s.lifetime;
        p.color           = s.colorStart;
        p.rotation        = s.rotation;
        p.angularVelocity = s.angularVelocity;
        p.spriteSeed      = s.spriteSeed;
        p.uvRect          = s.uvRect;
    }
    else
    {
        p = gParticles[i];
        // 死亡粒子はそのまま (VS で age >= lifetime をクリップ)
        if (p.age >= p.lifetime) return;
    }

    // 物理積分 (半陽的オイラー)。スポーン直後も同フレームから重力・力場・Noiseを受ける。
    p.velocity += gGravity * gDeltaTime;
    // 速度減衰: CPU の max(0, 1 - damping * dt) と同じ式
    float damping = max(0.0f, 1.0f - gVelocityDamping * gDeltaTime);
    p.velocity *= damping;
    // ベクトルフィールド: シーンの力場 + エミッター固有ノイズを速度へ加算 (CPU と同順)
    ApplyForceFields(p.position, p.velocity);
    if (gNoiseStrength > 0.0f)
    {
        p.velocity += CurlNoise(TurbulenceSamplePoint(
            p.position, gNoiseFrequency, gNoiseSpeed, gTime)) * (gNoiseStrength * gDeltaTime);
    }
    float normalizedAge = saturate((p.age + gDeltaTime) / max(p.lifetime, 1.0e-4f));
    float velocityScale = gCurveFlags.y > 0.5f
        ? max(EvaluateCurve4(gVelocityCurveKeys01, gVelocityCurveKeys23, normalizedAge), 0.0f)
        : 1.0f;
    float3 previousPosition = p.position;
    p.position += p.velocity * (gDeltaTime * velocityScale);
    if (gDepthCollision != 0u)
    {
        float4 clip = mul(float4(p.position, 1.0f), gViewProjection);
        if (clip.w > 1.0e-5f)
        {
            float3 ndc = clip.xyz / clip.w;
            float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
            if (all(uv >= 0.0f) && all(uv <= 1.0f) && ndc.z >= 0.0f && ndc.z <= 1.0f)
            {
            // UV=1.0 は解像度ちょうどの範囲外座標になるため、右端・下端を必ず有効画素へ収める。
            int2 pixel = clamp(int2(uv * float2(gScreenWidth, gScreenHeight)),
                               int2(0, 0), int2(gScreenWidth - 1u, gScreenHeight - 1u));
                float sceneDepth = gSceneDepth.Load(int3(pixel, 0));
                if (ndc.z >= sceneDepth && ndc.z - sceneDepth <= gDepthThickness)
                {
                    p.position = previousPosition;
                    if (gDepthResponse == 1u)
                    {
                        p.age = p.lifetime;
                        gParticles[i] = p;
                        return;
                    }
                    if (gDepthResponse == 2u) p.velocity = 0.0f;
                    else p.velocity = -p.velocity * gDepthBounciness;
                    p.velocity *= max(0.0f, 1.0f - gDepthDamping);
                }
            }
        }
    }
    p.age      += gDeltaTime;

    // 回転更新
    p.rotation += p.angularVelocity * gDeltaTime;

    // 寿命 t [0, 1] で色・サイズ補間 (CPU の colorCurvePower / sizeCurvePower と一致)
    float t = saturate(p.age / p.lifetime);
    p.color = gCurveFlags.z > 0.5f
        ? EvaluateGradient4(t)
        : lerp(gColorStart, gColorEnd, pow(t, gColorCurvePower));
    float sizeT = gCurveFlags.x > 0.5f
        ? saturate(EvaluateCurve4(gSizeCurveKeys01, gSizeCurveKeys23, t))
        : pow(t, gSizeCurvePower);
    p.size = lerp(gSizeStart, gSizeEnd, sizeT);

    // スプライトアニメーション (CPU の ComputeSpriteRect と一致)
    uint spriteSpan = gSpriteEndFrame - gSpriteStartFrame;
    uint relativeFrame = (uint)(t * (float)spriteSpan);
    if (gFlipbookMode == 1 && spriteSpan > 0)
        relativeFrame = (uint)(p.age * gFlipbookFramesPerSecond) % (spriteSpan + 1);
    else if (gFlipbookMode == 2)
        relativeFrame = (uint)(saturate(p.spriteSeed) * (float)spriteSpan);
    else if (gFlipbookMode == 3 && spriteSpan > 0)
    {
        uint cycle = (uint)(p.age * gFlipbookFramesPerSecond) % max(spriteSpan * 2, 1u);
        relativeFrame = cycle <= spriteSpan ? cycle : spriteSpan * 2 - cycle;
    }
    uint frame = gSpriteStartFrame + relativeFrame;
    uint sx         = frame % gSpriteColumns;
    uint sy         = frame / gSpriteColumns;
    float invCols   = 1.0f / (float)gSpriteColumns;
    float invRows   = 1.0f / (float)gSpriteRows;
    p.uvRect = float4(
        (float)sx * invCols,       (float)sy * invRows,
        (float)(sx + 1) * invCols, (float)(sy + 1) * invRows);

    gParticles[i] = p;
}
