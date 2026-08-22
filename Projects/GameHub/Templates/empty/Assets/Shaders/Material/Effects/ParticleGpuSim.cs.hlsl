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
#include "Common/Color.hlsli"
#include "Rendering/ParticleNoise.hlsli"

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
    // 粒子ごとの色倍率 (colorVariation)。毎フレーム作り直す色へ掛け直すために保持する。
    float3 colorScale;
    float  colorScalePad;
};

struct GpuSpawnEntry
{
    float3 position;
    float  lifetime;
    float3 velocity;
    float  size;
    float4 colorStart;
    // 粒子ごとの色ゆらぎ倍率 (xyz)。w は未使用。
    float4 colorScale;
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
    // ── over-lifetime モジュール追加分 (末尾追加で既存オフセットを変えない) ──
    float4   gCurveFlags2;          // x=rotation, y=drag, z/w=予約
    float4   gRotationCurveKeys01;
    float4   gRotationCurveKeys23;
    float4   gDragCurveKeys01;
    float4   gDragCurveKeys23;
    float3   gOrbitalAxis;          // CPU 側で正規化済み
    float    gOrbitalVelocity;
    float    gRadialVelocity;
    // bit0 = spriteRandomStartFrame / bit1 = spriteRandomRow
    uint     gSpriteRandomFlags;
    float    gVelocityPad0;
    float    gVelocityPad1;
    // ── カーブ 8 キー化の追加分 (RenderPassContext.hpp の GpuParticleEmitterCB と対) ──
    // 既存の *Keys01/23 (キー 0〜3) はオフセットを保ち、キー 4〜7 を末尾へ足す。
    float4   gSizeCurveKeys45;
    float4   gSizeCurveKeys67;
    float4   gVelocityCurveKeys45;
    float4   gVelocityCurveKeys67;
    float4   gRotationCurveKeys45;
    float4   gRotationCurveKeys67;
    float4   gDragCurveKeys45;
    float4   gDragCurveKeys67;
    float4   gGradientTimes47;
    float4   gGradientColors47[4];
    float4   gCurveKeyCounts;       // x=size, y=velocity, z=rotation, w=drag の有効キー数
    float4   gCurveModes;           // 補間モード 0=Linear 1=Step 2=Smooth
    float4   gGradientMeta;         // x=キー数, y=補間モード, z/w=予約
};

StructuredBuffer<GpuSpawnEntry>   gSpawnBuffer : register(SB_GPU_SPAWN);
RWStructuredBuffer<GpuParticle>   gParticles   : register(UAV_GPU_PARTICLES);
Texture2D<float>                   gSceneDepth : register(TEX_DEPTH);

// ---------- カールノイズ (乱流ベクトルフィールド) ---------------------------
// 式は ParticlePass.cpp の同名関数と一致させること (CPU/GPU で挙動を揃える)。

// PcgHash / LatticeValue / ValueNoise3D は Rendering/ParticleNoise.hlsli にある。
// 描画側のボリュメトリック煙が同じ式で密度を作るため、共有ヘッダーへ集約した。

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

// ---------- 速度モジュール (周回 / 放射) ------------------------------------

// 周回 (orbital) と放射 (radial) の加速度を速度へ加える。
// 式は ParticlePass.cpp の ApplyOrbitalVelocity と一致させること (CPU/GPU で挙動を揃える)。
// gOrbitalAxis は CPU 側で正規化済み。軸が退化していた場合は gOrbitalVelocity が 0 で渡る。
void ApplyOrbitalVelocity(float3 position, inout float3 velocity)
{
    if (gOrbitalVelocity == 0.0f && gRadialVelocity == 0.0f) return;

    float3 offset = position - gEmitterPos;
    float  dist   = length(offset);
    // 原点に重なった粒子は接線・放射方向が定義できない。ゼロ除算を避けて素通しする。
    if (dist < 1.0e-5f) return;
    float3 radialDir = offset / dist;

    if (gRadialVelocity != 0.0f)
        velocity += radialDir * (gRadialVelocity * gDeltaTime);

    if (gOrbitalVelocity != 0.0f)
    {
        // 接線 = axis × radial。軸と平行な粒子では長さ 0 になるので正規化前に確認する。
        float3 tangent = cross(gOrbitalAxis, radialDir);
        float  tangentLength = length(tangent);
        if (tangentLength > 1.0e-5f)
            velocity += (tangent / tangentLength) * (gOrbitalVelocity * gDeltaTime);
    }
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

// 補間係数へ曲線モードを適用する。
// ParticleEmitter.hpp の ApplyCurveInterpolation と必ず同じ式にすること
// (片方だけ直すと「CPU では正しいが GPU では違う」形で静かに壊れる)。
float ApplyCurveInterpolation(float alpha, float mode)
{
    if (mode > 1.5f) return alpha * alpha * (3.0f - 2.0f * alpha); // Smooth
    if (mode > 0.5f) return 0.0f;                                  // Step
    return alpha;                                                  // Linear
}

// 8キーカーブを評価する。float4 1本へ (time,value) を2点ずつ、計4本で8キー。
// count は有効キー数。超過分は最終キーで埋めてあるが、count で打ち切らないと
// 末尾のダミー区間を踏んで CPU の Evaluate と結果がずれる。
float EvaluateCurve8(float4 keys01, float4 keys23, float4 keys45, float4 keys67,
                     float count, float mode, float t)
{
    float2 keys[8] = {
        keys01.xy, keys01.zw, keys23.xy, keys23.zw,
        keys45.xy, keys45.zw, keys67.xy, keys67.zw
    };
    uint last = (uint)clamp(count, 1.0f, 8.0f) - 1u;
    if (t <= keys[0].x) return keys[0].y;
    for (uint i = 1; i < 8; ++i)
    {
        if (i > last) break;
        if (t <= keys[i].x)
        {
            float alpha = saturate((t - keys[i - 1].x) / max(keys[i].x - keys[i - 1].x, 1.0e-4f));
            return lerp(keys[i - 1].y, keys[i].y, ApplyCurveInterpolation(alpha, mode));
        }
    }
    return keys[last].y;
}

// ---------- グラデーションの色空間 -----------------------------------------
// 式は ParticleEmitter.hpp の ParticleGradient::MixKeys / ParticleColorSpace.hpp と
// 完全に一致させること。ずれると同じ .vfx が CPU/GPU で違う色になる。

float3 SignedCbrt3(float3 v)
{
    return sign(v) * pow(abs(v), 1.0f / 3.0f);
}

float3 LinearToOklab(float3 c)
{
    float3 lms = float3(
        0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b,
        0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b,
        0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b);
    float3 m = SignedCbrt3(lms);
    return float3(
        0.2104542553f * m.x + 0.7936177850f * m.y - 0.0040720468f * m.z,
        1.9779984951f * m.x - 2.4285922050f * m.y + 0.4505937099f * m.z,
        0.0259040371f * m.x + 0.7827717662f * m.y - 0.8086757660f * m.z);
}

float3 OklabToLinear(float3 lab)
{
    float3 m = float3(
        lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z,
        lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z,
        lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z);
    float3 lms = m * m * m;
    return float3(
         4.0767416621f * lms.x - 3.3077115913f * lms.y + 0.2309699292f * lms.z,
        -1.2684380046f * lms.x + 2.6097574011f * lms.y - 0.3413193965f * lms.z,
        -0.0041960863f * lms.x - 0.7034186147f * lms.y + 1.7076147010f * lms.z);
}

// 2 キーをオーサリング空間 (sRGB) で受け取り、指定空間で混ぜて sRGB のまま返す。
// アルファは常に線形補間 (不透明度は光量ではないため色空間の対象外)。
float4 MixGradientKeys(float4 a, float4 b, float alpha, float space)
{
    float w = lerp(a.a, b.a, alpha);
    if (space > 1.5f)   // Oklab
    {
        float3 oa = LinearToOklab(SRGBToLinear(a.rgb));
        float3 ob = LinearToOklab(SRGBToLinear(b.rgb));
        return float4(LinearToSRGB(OklabToLinear(lerp(oa, ob, alpha))), w);
    }
    if (space > 0.5f)   // Linear
    {
        return float4(LinearToSRGB(lerp(SRGBToLinear(a.rgb), SRGBToLinear(b.rgb), alpha)), w);
    }
    return float4(lerp(a.rgb, b.rgb, alpha), w);   // Gamma
}

// 戻り値はリニア。粒子バッファへ書く色は常にリニアで、描画側は変換しない。
float4 EvaluateGradient8(float t)
{
    float times[8] = {
        gGradientTimes.x, gGradientTimes.y, gGradientTimes.z, gGradientTimes.w,
        gGradientTimes47.x, gGradientTimes47.y, gGradientTimes47.z, gGradientTimes47.w
    };
    float4 colors[8] = {
        gGradientColors[0], gGradientColors[1], gGradientColors[2], gGradientColors[3],
        gGradientColors47[0], gGradientColors47[1], gGradientColors47[2], gGradientColors47[3]
    };
    uint last = (uint)clamp(gGradientMeta.x, 1.0f, 8.0f) - 1u;
    float space = gGradientMeta.z;
    if (t <= times[0]) return SRGBToLinear(colors[0]);
    for (uint i = 1; i < 8; ++i)
    {
        if (i > last) break;
        if (t <= times[i])
        {
            float alpha = saturate((t - times[i - 1]) / max(times[i] - times[i - 1], 1.0e-4f));
            alpha = ApplyCurveInterpolation(alpha, gGradientMeta.y);
            return SRGBToLinear(MixGradientKeys(colors[i - 1], colors[i], alpha, space));
        }
    }
    return SRGBToLinear(colors[last]);
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
        // 色ゆらぎ倍率は CPU が求めた値をそのまま受け取る。
        // WHY: 旧実装は colorStart と CB の基準色の「比」から復元していたが、
        //      グラデーション使用時は色が基準色と無関係になるため復元が成立せず、
        //      GPU だけゆらぎが化けていた。CPU/GPU で必ず同じ値を使う。
        p.colorScale = s.colorScale.rgb;
        p.colorScalePad = 0.0f;
    }
    else
    {
        p = gParticles[i];
        // 死亡粒子はそのまま (VS で age >= lifetime をクリップ)
        if (p.age >= p.lifetime) return;
    }

    // 更新後の正規化寿命。drag / 回転 / 速度カーブが同じ値を見るよう先に 1 回だけ求める
    // (CPU 側も age を進めた後の t を全カーブで共有している)。
    float normalizedAge = saturate((p.age + gDeltaTime) / max(p.lifetime, 1.0e-4f));

    // 物理積分 (半陽的オイラー)。スポーン直後も同フレームから重力・力場・Noiseを受ける。
    p.velocity += gGravity * gDeltaTime;
    // 周回・放射。式は ParticlePass.cpp の ApplyOrbitalVelocity と一致させること。
    ApplyOrbitalVelocity(p.position, p.velocity);
    // 速度減衰: CPU の max(0, 1 - damping * dragScale * dt) と同じ式
    float dragScale = gCurveFlags2.y > 0.5f
        ? max(EvaluateCurve8(gDragCurveKeys01, gDragCurveKeys23, gDragCurveKeys45,
                             gDragCurveKeys67, gCurveKeyCounts.w, gCurveModes.w, normalizedAge), 0.0f)
        : 1.0f;
    float damping = max(0.0f, 1.0f - gVelocityDamping * dragScale * gDeltaTime);
    p.velocity *= damping;
    // ベクトルフィールド: シーンの力場 + エミッター固有ノイズを速度へ加算 (CPU と同順)
    ApplyForceFields(p.position, p.velocity);
    if (gNoiseStrength > 0.0f)
    {
        p.velocity += CurlNoise(TurbulenceSamplePoint(
            p.position, gNoiseFrequency, gNoiseSpeed, gTime)) * (gNoiseStrength * gDeltaTime);
    }
    float velocityScale = gCurveFlags.y > 0.5f
        ? max(EvaluateCurve8(gVelocityCurveKeys01, gVelocityCurveKeys23, gVelocityCurveKeys45,
                             gVelocityCurveKeys67, gCurveKeyCounts.y, gCurveModes.y, normalizedAge), 0.0f)
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

    // 回転更新。回転カーブは角速度への時間倍率 (CPU の useRotationCurve と同じ)。
    float rotationScale = gCurveFlags2.x > 0.5f
        ? EvaluateCurve8(gRotationCurveKeys01, gRotationCurveKeys23, gRotationCurveKeys45,
                         gRotationCurveKeys67, gCurveKeyCounts.z, gCurveModes.z, normalizedAge)
        : 1.0f;
    p.rotation += p.angularVelocity * gDeltaTime * rotationScale;

    // 寿命 t [0, 1] で色・サイズ補間 (CPU の colorCurvePower / sizeCurvePower と一致)
    float t = saturate(p.age / p.lifetime);
    // 粒子バッファへ書く色は常にリニア。描画シェーダーは頂点カラーを変換しない。
    p.color = gCurveFlags.z > 0.5f
        ? EvaluateGradient8(t)
        : SRGBToLinear(lerp(gColorStart, gColorEnd, pow(t, gColorCurvePower)));
    // 粒子ごとの色ゆらぎを掛け直す (alpha はフェード制御なので触らない)。
    p.color.rgb *= p.colorScale;
    float sizeT = gCurveFlags.x > 0.5f
        ? saturate(EvaluateCurve8(gSizeCurveKeys01, gSizeCurveKeys23, gSizeCurveKeys45,
                                  gSizeCurveKeys67, gCurveKeyCounts.x, gCurveModes.x, t))
        : pow(t, gSizeCurvePower);
    p.size = lerp(gSizeStart, gSizeEnd, sizeT);

    // スプライトアニメーション (CPU の ComputeSpriteFrameState と一致させること)
    uint spriteStart = gSpriteStartFrame;
    uint spriteEnd   = gSpriteEndFrame;
    // Random Row: 粒子ごとに 1 行を選び、その行の中だけで再生する
    if ((gSpriteRandomFlags & 2u) != 0u && gSpriteRows > 1u)
    {
        uint row = min((uint)(saturate(p.spriteSeed) * (float)gSpriteRows), gSpriteRows - 1u);
        spriteStart = row * gSpriteColumns;
        spriteEnd   = spriteStart + gSpriteColumns - 1u;
    }
    uint spriteSpan = spriteEnd - spriteStart;
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
    // Random Start Frame: 再生位相を粒子ごとにずらす (RandomFrame モードでは不要)。
    // seed の使い回しで行と位相が相関しないよう、CPU 側と同じ係数でずらして小数部を取る。
    if ((gSpriteRandomFlags & 1u) != 0u && spriteSpan > 0u && gFlipbookMode != 2u)
    {
        float phaseSeed = saturate(p.spriteSeed) * 7.13f + 0.37f;
        float decorrelated = phaseSeed - floor(phaseSeed);
        uint cycle = spriteSpan + 1u;
        relativeFrame = (relativeFrame + (uint)(decorrelated * (float)cycle)) % cycle;
    }
    uint frame = spriteStart + relativeFrame;
    uint sx         = frame % gSpriteColumns;
    uint sy         = frame / gSpriteColumns;
    float invCols   = 1.0f / (float)gSpriteColumns;
    float invRows   = 1.0f / (float)gSpriteRows;
    p.uvRect = float4(
        (float)sx * invCols,       (float)sy * invRows,
        (float)(sx + 1) * invCols, (float)(sy + 1) * invRows);

    gParticles[i] = p;
}
