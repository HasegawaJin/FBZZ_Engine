/// @file    ParticleGpuSim.cs.hlsl
/// @brief   GPU パーティクルシミュレーション: スポーン + 物理積分 + 流れ/ノイズ + 色/サイズ/スプライト補間
/// @author  Hasegawa Jin
/// @date    2026-06-14
//
// dispatch: ceil(maxParticles / 64) × 1 × 1
// スロット:
//   b0  = GpuEmitterCB
//   t15 = StructuredBuffer<GpuSpawnEntry>  (スポーンバッファ DYNAMIC SRV)
//   t29 = StructuredBuffer<GpuFlowField>   (このエミッターに効く流れ一式)
//   t26 = Texture3D<float4>                (速度場アトラス。空でも常に束縛される)
//   u2  = RWStructuredBuffer<GpuParticle> (パーティクルプール DEFAULT)

#include "Common/Binding.hlsli"
#include "Common/Color.hlsli"
#include "Rendering/ParticleNoise.hlsli"
#include "Rendering/FlowField.hlsli"
#include "Common/BindlessIndices.hlsli"

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
    float  spriteBlend;   // 次のコマへの補間率 (Frame Blending)
    float4 nextUvRect;    // 次のコマの UV 矩形
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
    float    gFlowCoupling;      // 流れへ寄る速さ [1/s] (旧 gVelocityDamping と同じ枠)
    float    gSizeStart;
    float    gSizeEnd;
    float    gSizeCurvePower;
    float    gPad0;
    uint     gSpriteColumns;
    uint     gSpriteRows;
    uint     gSpriteStartFrame;
    uint     gSpriteEndFrame;
    // ── ノイズモジュール + 流れ (末尾追加で既存オフセットを変えない) ──
    float    gTime;             // カールノイズのスクロールに使う経過時間
    float    gNoiseStrength;    // エミッター固有乱流の強さ (0 で無効)
    float    gNoiseFrequency;
    float    gNoiseSpeed;
    uint     gFlowFieldCount;   // gParticleForces (SRV) の有効本数
    uint     gFlipbookMode;
    float    gFlipbookFramesPerSecond;
    float    gPad1;
    // 流れは StructuredBuffer (gParticleForces) へ移った (2026-09-11)。
    // 枠は **空けたまま残す**。詰めると後続の gViewProjection 以降が丸ごと別の値を読む。
    float4   gReservedFlowFields[24];
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

FBZZ_SBUFFER_T(GpuSpawnEntry, gSpawnBuffer, SB_GPU_SPAWN_SLOT);
FBZZ_RWSBUFFER_T(GpuParticle, gParticles, UAV_GPU_PARTICLES_SLOT);
FBZZ_TEX2D_T(float, gSceneDepth, TEX_DEPTH_SLOT);
// このエミッターに効く流れ一式。本数は gFlowFieldCount。上限は無い。
FBZZ_SBUFFER_T(GpuFlowField, gParticleForces, SB_PARTICLE_FORCES_SLOT);
// 常駐中の速度場を積んだアトラス。場が 1 枚も無くても 1x1x1 が必ず束縛される。
FBZZ_TEX3D_T(float4, gVelocityAtlas, TEX_VELOCITY_FIELD_SLOT);
SamplerState                       gVelocitySamp   : register(SAMPLER_LINEAR_CLAMP);

// ---------- 速度モジュール (周回 / 放射) ------------------------------------

// 周回 (orbital) と放射 (radial) の加速度を速度へ加える。
// 対になる CPU 実装は無い。ParticlePass.cpp の同名関数は orbital スロットの廃止で削除済みで、
// gOrbitalVelocity / gRadialVelocity は常に 0 が入るため、ここは実質 no-op。
// WHY 消さないか: この 2 枠と gOrbitalAxis は GpuEmitterCB のレイアウトの一部で、
//      式だけ削っても CB は縮まない。常に 0 なら素通りするので残しておく方が安全。
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

// ---------- 流れの場 (FlowField) -------------------------------------------

// 1 点の媒質速度 [m/s]。式の本体は Rendering/FlowField.hlsli (Fiber と共有)。
// チャンネルは CPU の ResolveEmitterForces で絞り込み済みなので全チャンネルで評価する。
float3 SampleFlow(float3 position, out bool covered)
{
    return SampleFlowFields(gParticleForces, 0u, gFlowFieldCount, FLOW_ALL_CHANNELS,
                            gVelocityAtlas, gVelocitySamp, position, gTime, covered);
}

// 流れへ粒子速度を緩和させる。式は FlowFieldEval.cpp の ApplyFlowFields と一致させること。
// 場に覆われていない点では何もしない («場が無い» は «流速 0 の静止した空気» ではない)。
void ApplyFlowFields(float3 position, inout float3 velocity, float coupling)
{
    if (gFlowFieldCount == 0 || coupling <= 0.0f) return;
    bool covered = false;
    float3 flow = SampleFlow(position, covered);
    if (!covered) return;
    velocity += (flow - velocity) * saturate(coupling * gDeltaTime);
}

// 補間係数へ曲線モードを適用する。
// ParticleEmitter.hpp の ApplyCurveInterpolation と必ず同じ式にすること
// (片方だけ直すと「CPU では正しいが GPU では違う」形で静かに壊れる)。
float ApplyCurveInterpolation(float alpha, float mode)
{
    if (mode > 1.5f) return alpha * alpha * (3.0f - 2.0f * alpha); // Smooth
    if (mode > 0.5f) return alpha >= 1.0f ? 1.0f : 0.0f;           // Step (区間は [前, 次))
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
// NOTE: 出口を 1 つにしてある。ループ内で展開される関数の早期 return は FXC が X4000 で咎める。
float4 MixGradientKeys(float4 a, float4 b, float alpha, float space)
{
    float3 rgb;
    if (space > 1.5f)   // Oklab
    {
        float3 oa = LinearToOklab(SRGBToLinear(a.rgb));
        float3 ob = LinearToOklab(SRGBToLinear(b.rgb));
        rgb = LinearToSRGB(OklabToLinear(lerp(oa, ob, alpha)));
    }
    else if (space > 0.5f)   // Linear
    {
        rgb = LinearToSRGB(lerp(SRGBToLinear(a.rgb), SRGBToLinear(b.rgb), alpha));
    }
    else   // Gamma
    {
        rgb = lerp(a.rgb, b.rgb, alpha);
    }
    return float4(rgb, lerp(a.a, b.a, alpha));
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
        p.spriteBlend = 0.0f;
        p.nextUvRect  = s.uvRect;
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

    // 物理積分 (半陽的オイラー)。スポーン直後も同フレームから重力・流れ・Noiseを受ける。
    p.velocity += gGravity * gDeltaTime;
    // 周回・放射。定数が常に 0 で渡るため現状は no-op (ApplyOrbitalVelocity の注記を参照)。
    ApplyOrbitalVelocity(p.position, p.velocity);
    // Drag over Lifetime は結合係数への時間倍率 (CPU と同じ)。
    float dragScale = gCurveFlags2.y > 0.5f
        ? max(EvaluateCurve8(gDragCurveKeys01, gDragCurveKeys23, gDragCurveKeys45,
                             gDragCurveKeys67, gCurveKeyCounts.w, gCurveModes.w, normalizedAge), 0.0f)
        : 1.0f;
    // 流れへの緩和: シーンの場 + エミッター内蔵の流れ (CPU と同順)
    ApplyFlowFields(p.position, p.velocity, gFlowCoupling * dragScale);
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
                /// @note カメラ深度は Reversed-Z (奥ほど小さい)。面の奥へ gDepthThickness 以内に入ったら当たり。
                if (ndc.z <= sceneDepth && sceneDepth - ndc.z <= gDepthThickness)
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

    // スプライトアニメーション (asset::EvaluateFlipbookFrame と 1:1 に保つこと)
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
    // コマ位置を小数で持ち、整数部 = 現コマ・小数部 = 次コマへの補間率とする。
    float fps = max(gFlipbookFramesPerSecond, 0.0f);
    float framePosition = saturate(t) * (float)spriteSpan;
    bool  wrapNext = false;
    if (gFlipbookMode == 1u)
    {
        framePosition = spriteSpan > 0u ? fmod(p.age * fps, (float)(spriteSpan + 1u)) : 0.0f;
        wrapNext = true;
    }
    else if (gFlipbookMode == 2u)
    {
        framePosition = floor(saturate(p.spriteSeed) * (float)spriteSpan);
    }
    else if (gFlipbookMode == 3u)
    {
        float cycleLength = (float)max(spriteSpan * 2u, 1u);
        float cycleFrame  = fmod(p.age * fps, cycleLength);
        framePosition = cycleFrame <= (float)spriteSpan ? cycleFrame : (float)(spriteSpan * 2u) - cycleFrame;
    }
    // Random Start Frame: 再生位相を粒子ごとにずらす (RandomFrame モードでは不要)。
    // seed の使い回しで行と位相が相関しないよう、CPU 側と同じ係数でずらして小数部を取る。
    if ((gSpriteRandomFlags & 1u) != 0u && spriteSpan > 0u && gFlipbookMode != 2u)
    {
        float phaseSeed = saturate(p.spriteSeed) * 7.13f + 0.37f;
        float decorrelated = phaseSeed - floor(phaseSeed);
        float cycle = (float)(spriteSpan + 1u);
        framePosition = fmod(framePosition + floor(decorrelated * cycle), cycle);
        wrapNext = true;
    }
    uint relativeFrame     = min((uint)max(floor(framePosition), 0.0f), spriteSpan);
    uint nextRelativeFrame = min(relativeFrame + 1u, spriteSpan);
    if (wrapNext && relativeFrame == spriteSpan) nextRelativeFrame = 0u;
    // curveFlags.w = Frame Blending。RandomFrame はコマが飛ぶだけなので混ぜない。
    p.spriteBlend = (gCurveFlags.w > 0.5f && gFlipbookMode != 2u)
        ? framePosition - floor(framePosition) : 0.0f;

    float2 cellSize  = float2(1.0f / (float)gSpriteColumns, 1.0f / (float)gSpriteRows);
    uint   frame     = spriteStart + relativeFrame;
    uint   nextFrame = spriteStart + nextRelativeFrame;
    float2 frameOrigin = float2((float)(frame % gSpriteColumns), (float)(frame / gSpriteColumns)) * cellSize;
    float2 nextOrigin  = float2((float)(nextFrame % gSpriteColumns), (float)(nextFrame / gSpriteColumns)) * cellSize;
    p.uvRect     = float4(frameOrigin, frameOrigin + cellSize);
    p.nextUvRect = float4(nextOrigin, nextOrigin + cellSize);

    gParticles[i] = p;
}
