/// @file    FiberCommon.hlsli
/// @brief   毛皮と芝の根元固定変形、手続き密度、照明。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#ifndef FBZZ_FIBER_COMMON_HLSLI
#define FBZZ_FIBER_COMMON_HLSLI
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/LodDither.hlsli"
#include "Rendering/FlowField.hlsli"
#include "Common/MaterialTextures.hlsli"

/// @note Asset/FiberMaterialSettings.hpp と一致。風の応答は [s]、長さと曲げ量はワールド [m]。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
#include "Fiber/FiberMaterialFields.hlsli"
#ifdef FBZZ_FIBER_MATERIAL_FIELDS
#include FBZZ_FIBER_MATERIAL_FIELDS
#endif
};

FBZZ_MATERIAL_TEX(fiberMaskTexture, tex5Index);
SamplerState fiberMaskSampler : register(SAMPLER_DEFAULT);

/// @brief fiberMask をメッシュの UV で引く。R=密度 G=長さ B=束の寄り A=毛先色の混ぜ具合で、どれも .mat の値に掛ける。
/// @note 頂点シェーダーでも読むのでミップは 0 に固定する。長さは頂点単位でしか変えられない。
float4 FiberMaskLevel0(float2 uv)
{
    return fiberMaskTexture.SampleLevel(fiberMaskSampler, uv, 0.0f);
}

/// @brief ピクセルシェーダー用。画面微分でミップを選ぶので clip より前に呼ぶ。
float4 FiberMaskFiltered(float2 uv)
{
    return fiberMaskTexture.Sample(fiberMaskSampler, uv);
}

cbuffer FiberFrameConstants : register(b5)
{
    float3 fiberWind;
    float fiberTime;
    float fiberShellCount;
    float fiberHybrid;
    float fiberTurbulence;
    float fiberPulseFrequency;
    float4 fiberLod;
    /// @note 局所 FlowField の範囲。今フレームは [0, fiberFlowCount)、前フレームは [fiberFlowPreviousFirst, +fiberFlowPreviousCount)。
    uint fiberFlowCount;
    uint fiberFlowPreviousFirst;
    uint fiberFlowPreviousCount;
    /// @note FiberComponent::flowChannels。場の channels と 1 ビットでも重なる場だけを受ける。
    uint fiberFlowChannels;
    float4 fiberTerrainWaterClip;
};

/// @note 環境流は fiberWind が持つ。ここにはシーンに置いた FlowField だけが入り、二重に足さない。
FBZZ_VS_SBUFFER(GpuFlowField, fiberFlowFields, VS_SB_BUFFER1_SLOT);
FBZZ_TEX3D_T(float4, fiberVelocityAtlas, TEX_VELOCITY_FIELD_SLOT);
SamplerState fiberFlowSampler : register(SAMPLER_LINEAR_CLAMP);

/// @brief 根元での局所 FlowField の媒質速度 [m/s]。頂点シェーダー専用。
/// @note 場に覆われない点は 0。GPU 粒子と同じ SampleFlowFields で評価する。
float3 FiberLocalFlow(float3 root, float time, bool previous)
{
    uint count = previous ? fiberFlowPreviousCount : fiberFlowCount;
    if (count == 0u) return float3(0.0f, 0.0f, 0.0f);
    bool covered = false;
    return SampleFlowFields(fiberFlowFields, previous ? fiberFlowPreviousFirst : 0u, count, fiberFlowChannels,
        fiberVelocityAtlas, fiberFlowSampler, root, time, covered);
}

/// @note チャンクに届く最大32接触。発生時刻より前の速度評価には接触を適用しない。
/// @note LAYOUT: FiberRenderPass.hpp の FiberContactCB と一致させる。
cbuffer FiberContactConstants : register(b10)
{
    float4 fiberContactCenters[64];
    float4 fiberContactTimes[64];
    float4 fiberContactExtents[64];
    float4 fiberContactRotations[64];
    /// @note 走査する枠数 [0,32]。これより後ろの枠は寄与しない (CPU が保証する)。
    uint fiberContactCount;
    uint fiberContactPreviousCount;
    uint _fiberContactPadding0;
    uint _fiberContactPadding1;
};

float3 FiberContactRotate(float3 value, float4 rotation)
{
    float3 twice = 2.0f * cross(rotation.xyz, value);
    return value + rotation.w * twice + cross(rotation.xyz, twice);
}

/// @note Degenerate and parallel segments are supported; both returned points lie on their closed segments.
/// @see https://realtimecollisiondetection.net/books/rtcd/ Section 5.1.9, Closest Points of Two Line Segments.
void FiberContactClosestSegments(float3 p, float3 q, float3 a, float3 b, out float3 grass, out float3 collider)
{
    float3 u = q - p, v = b - a, r = p - a;
    float uu = dot(u, u), vv = dot(v, v), uv = dot(u, v);
    float ur = dot(u, r), vr = dot(v, r);
    float s = 0.0f, t = 0.0f;
    if (uu > 1.0e-8f) {
        if (vv > 1.0e-8f) {
            float denominator = uu * vv - uv * uv;
            if (denominator > 1.0e-8f * uu * vv) s = saturate((uv * vr - ur * vv) / denominator);
            t = (uv * s + vr) / vv;
            if (t < 0.0f) { t = 0.0f; s = saturate(-ur / uu); }
            else if (t > 1.0f) { t = 1.0f; s = saturate((uv - ur) / uu); }
        } else s = saturate(-ur / uu);
    } else if (vv > 1.0e-8f) t = saturate(vr / vv);
    grass = p + u * s;
    collider = a + v * t;
}

/// @note The undeformed blade segment is tested in 3D, so overhead colliders do not flatten the ground below them.
float FiberContactWeight(uint index, float3 root, float3 normal, float lengthValue, out float3 direction)
{
    float3 center = fiberContactCenters[index].xyz;
    float radius = fiberContactCenters[index].w;
    float kind = fiberContactTimes[index].w;
    float3 extent = fiberContactExtents[index].xyz;
    float3 delta;
    float weight;
    if (kind > 1.5f) {
        float4 rotation = fiberContactRotations[index];
        float4 inverse = float4(-rotation.xyz, rotation.w);
        float3 localRoot = FiberContactRotate(root - center, inverse);
        float3 segment = FiberContactRotate(normal * lengthValue, inverse);
        float enter = 0.0f, leave = 1.0f;
        [unroll] for (uint axis = 0; axis < 3; ++axis) {
            if (abs(segment[axis]) < 1.0e-7f) {
                if (abs(localRoot[axis]) > extent[axis]) { direction = 0; return 0.0f; }
            } else {
                float first = (-extent[axis] - localRoot[axis]) / segment[axis];
                float last = (extent[axis] - localRoot[axis]) / segment[axis];
                enter = max(enter, min(first, last));
                leave = min(leave, max(first, last));
            }
        }
        if (enter > leave) { direction = 0; return 0.0f; }
        float3 localSample = localRoot + segment * ((enter + leave) * 0.5f);
        float3 penetration = extent - abs(localSample);
        weight = saturate(min(penetration.x, min(penetration.y, penetration.z))
            / max(min(extent.x, min(extent.y, extent.z)), 0.001f));
        delta = FiberContactRotate(localSample, rotation);
    } else {
        float3 grass, collider;
        if (kind > 0.5f) {
            FiberContactClosestSegments(root, root + normal * lengthValue, center - extent, center + extent, grass, collider);
        } else {
            grass = root + normal * clamp(dot(center - root, normal), 0.0f, lengthValue);
            collider = center;
        }
        delta = grass - collider;
        float distanceSq = dot(delta, delta);
        if (distanceSq >= radius * radius) { direction = 0; return 0.0f; }
        weight = saturate(1.0f - sqrt(distanceSq) / max(radius, 0.001f));
    }
    float3 projected = delta - normal * dot(delta, normal);
    float3 fallback = abs(normal.y) < 0.9f ? cross(normal, float3(0, 1, 0)) : cross(normal, float3(0, 0, 1));
    direction = dot(projected, projected) > 1.0e-8f ? normalize(projected) : normalize(fallback);
    return weight;
}

float3 FiberContactOffset(float3 root, float3 normal, float height, float time, float lengthValue, bool previous = false)
{
    /// @note 戻り値は planarShading を掛けるので、毛皮 (0) と接触 0 件では頂点ごとの 32 回の走査を丸ごと省く。
    uint count = min(previous ? fiberContactPreviousCount : fiberContactCount, 32u);
    if (count == 0u || planarShading <= 0.0f) return float3(0.0f, 0.0f, 0.0f);
    float strongest = 0.0f;
    float3 direction = float3(1, 0, 0);
    [loop] for (uint j=0; j<count; ++j) {
        uint i=j+(previous ? 32 : 0);
        float age = time - fiberContactTimes[i].x;
        float radius = fiberContactCenters[i].w;
        if (radius <= 0 || age < 0) continue;
        float phase = age / max(fiberContactTimes[i].z, 0.05f);
        if (phase >= 1.0f) continue;
        float3 contactDirection;
        float weight = FiberContactWeight(i, root, normal, lengthValue, contactDirection);
        /// @note Analytic finite-time recovery needs no per-blade CPU state or frame-rate-dependent integration.
        weight *= (1.0f-phase) * (1.0f-phase) * cos(phase * 6.283185307f) * fiberContactTimes[i].y;
        if (abs(weight) > abs(strongest)) {
            strongest = weight;
            direction = contactDirection;
        }
    }
    return (direction*strongest-normal*abs(strongest))*lengthValue*0.85f*height*height*planarShading;
}

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_TEXCUBE(texFiberIrradiance, TEX_IBL_IRRADIANCE_SLOT);
FBZZ_TEXCUBE(texFiberPrefilter, TEX_IBL_PREFILTER_SLOT);
FBZZ_TEX2D_T(float4, texFiberBRDFLut, TEX_IBL_BRDF_LUT_SLOT);
SamplerComparisonState sampShadow : register(SAMPLER_SHADOW);

float3 FiberNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return lenSq > 1.0e-12f ? value * rsqrt(lenSq) : fallback;
}

/// @note 位相は根元のワールド位置で固定し、Shell の層やビューごとに変更しない。
/// @note h^2 は根元固定の見た目用近似。毛の物理シミュレーションではない。
/// @param localFlow 局所 FlowField の媒質速度 [m/s]。環境風と同じ応答係数で曲げへ足す。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-7-rendering-countless-blades-waving-grass
float3 FiberBendAt(float3 root, float3 normal, float3 wind, float time,
    float turbulence, float pulseFrequency, float response, float bendLimit, float gravity, float3 localFlow,
    float2 cardMotion)
{
    float phase = dot(root, float3(0.73f, 0.31f, 0.57f)) + time * pulseFrequency * 6.2831853f;
    float3 gust = float3(sin(phase), 0.0f, cos(phase * 0.79f)) * turbulence;
    float3 force = (wind + gust + localFlow) * response + float3(0.0f, -gravity, 0.0f);
#ifdef FBZZ_FIBER_GRASS_CARD
    /// @note World-space phase keeps the breeze coherent across patches and identical in the velocity pass.
    float swayPhase = dot(root.xz, float2(0.81f, 0.47f)) + time * clamp(cardSwayHz * cardMotion.y, 0.0f, 4.0f) * 6.2831853f;
    force += float3(0.89f, 0.0f, 0.46f) * (sin(swayPhase) * clamp(cardSway * cardMotion.x, 0.0f, 0.25f));
#endif
    float3 bend = force - normal * dot(force, normal);
    float len = length(bend);
    return bend * min(1.0f, bendLimit / max(len, 1.0e-6f));
}

float3 FiberBend(float3 root, float3 normal, float3 localFlow, float2 cardMotion)
{
    return FiberBendAt(root, normal, fiberWind, fiberTime, fiberTurbulence,
        fiberPulseFrequency, windResponse, maxBend, gravityBend, localFlow, cardMotion);
}

/// @param lengthScale fiberMask の G。毛丈と曲げ量の両方に掛け、短い毛ほど曲がらない。
float3 FiberPosition(float3 root, float3 normal, float height, float3 localFlow, float lengthScale, float2 cardMotion)
{
    return root + normal * (fiberLength * lengthScale * height)
        + FiberBend(root, normal, localFlow, cardMotion) * (lengthScale * height * height)
        + FiberContactOffset(root, normal, height, fiberTime, fiberLength * lengthScale);
}

float2 FiberRootCoordinates(float2 uv, float3 root)
{
    return (worldMapping >= 0.5f ? root.xz : uv) * fiberFrequency;
}

float FiberHash(float2 cell)
{
    float3 p = frac(float3(cell.xyx) * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

#ifdef FBZZ_FIBER_GRASS_CARD
/// @return Atlas cell in row-major order, or 4 for a single-card material.
/// @note Local roots retain their species across streaming, object movement, and all render passes.
uint FiberCardSpecies(float2 localRoot)
{
    if (cardSpeciesAtlas == 0u) return 4u;

    const float4 weights = max(cardSpeciesWeights, 0.0f);
    const float3 limits = float3(weights.x, weights.x + weights.y, weights.x + weights.y + weights.z);
    const float total = limits.z + weights.w;
    if (total <= 0.0f) return 4u;

    const float individual = FiberHash(localRoot + float2(17.83f, 29.41f));
    const float patchShare = 0.42f;
    const float patch = FiberHash(floor(localRoot * 0.36f) + float2(31.2f, 15.8f));
    const float roll = (individual < patchShare ? patch : (individual - patchShare) / (1.0f - patchShare)) * total;
    return uint(roll >= limits.x) + uint(roll >= limits.y) + uint(roll >= limits.z);
}

float2 FiberCardMotion(uint species)
{
    if (species >= 4u) return float2(1.0f, 1.0f);
    return float2(cardSpeciesSway[species], cardSpeciesSwayHz[species]);
}
#endif

/// @brief 点 position から視点へ向かう単位ベクトル。平行投影ではビューの前方の逆。
/// @note 影のパスでは frameCB が光源のビューを持つので、光源から見た向きになる。
float3 FiberViewDirection(float3 position, float3 fallback)
{
    return isOrthographic > 0.5f
        ? FiberNormalize(float3(view[0][2], view[1][2], view[2][2]) * -1.0f, fallback)
        : FiberNormalize(cameraPos - position, fallback);
}

/// @brief coverage を通った 1 本の毛。照明が毛ごとの丸み・色むら・ハイライトのずらしに使う。
struct FiberStrand {
    /// @note 毛の軸から画素へのずれを半径で割った値 (長さ < 1)。Shell はセル座標の 2 軸、Fin は x だけ。
    float2 offset;
    /// @note 毛ごとの乱数 [0,1)。
    float hash;
    /// @note 高さを毛自身の長さで割った値。1 が毛先。
    float tip;
    /// @note offset をワールドへ戻したもの。0 なら offset.x を画面の横ずれとみなす。
    float3 across;
    /// @note fiberMask の A。根元色から毛先色へ進む割合に掛ける (0 で毛全体が根元色)。
    float colorMix;
#ifdef FBZZ_FIBER_GRASS_CARD
    float3 cardColor;
#endif
};

FiberStrand FiberDefaultStrand(float height, float hash)
{
    FiberStrand strand;
    strand.offset = float2(0.0f, 0.0f);
    strand.hash = hash;
    strand.tip = height;
    strand.across = float3(0.0f, 0.0f, 0.0f);
    strand.colorMix = 1.0f;
#ifdef FBZZ_FIBER_GRASS_CARD
    strand.cardColor = float3(1.0f, 1.0f, 1.0f);
#endif
    return strand;
}

/// @brief 束 1 つが覆う正方形の一辺 [セル]。
static const float FIBER_CLUMP_CELLS = 3.0f;
/// @brief 毛先での束の中心への収束率の上限。1 では毛先が 1 点に潰れる。
static const float FIBER_CLUMP_MAX_PULL = 0.7f;

/// @brief セル座標 location を含む束の中心 [セル座標] と、高さ height での収束率・回転 (cos, sin)。
/// @param amount 束の寄り [0,1] (clumping × fiberMask の B)。
/// @note ねじれは束が寄るほど見えるので amount を掛ける。寄らない束を回すと束の境で毛が切れる。
void FiberClumpAt(float2 location, float height, float amount, out float2 center, out float pull, out float2 rotation)
{
    float2 clumpCell = floor(location / FIBER_CLUMP_CELLS);
    float2 jitter = float2(FiberHash(clumpCell + 41.9f), FiberHash(clumpCell + 5.3f)) - 0.5f;
    center = (clumpCell + 0.5f + 0.7f * jitter) * FIBER_CLUMP_CELLS;
    amount = saturate(amount);
    pull = amount * FIBER_CLUMP_MAX_PULL * height;
    float turn = FiberHash(clumpCell + 3.7f) < 0.5f ? -1.0f : 1.0f;
    sincos(clumpTwist * amount * 6.2831853f * height * turn, rotation.y, rotation.x);
}

/// @brief 根元の軸がセル座標 root にある毛の、高さ height での軸の位置 [セル座標]。
/// @note 束の中心 C へ (1-k) 倍に縮めて C 回りに θ 回す: q = C + R(θ)(root - C)(1 - k)。k と θ は高さに比例する (clump-rate)。
/// @note 縮小は束の正方形の内側へ写すので、隣の束へ入り込むのは回転で角がはみ出す分だけ。
/// @see https://ieeexplore.ieee.org/document/803368 (Bruderlin, A Method to Generate Wet and Broken-up Animal Fur: clumping)
/// @see https://robbredow.com/2000/07/fur-in-stuart-little/ (Fur in Stuart Little: clump-percent / clump-rate)
float2 FiberClumpedAxis(float2 root, float height, float amount)
{
    if (amount <= 0.0f) return root;
    float2 center, rotation;
    float pull;
    FiberClumpAt(root, height, amount, center, pull, rotation);
    float2 local = (root - center) * (1.0f - pull);
    return center + float2(rotation.x * local.x - rotation.y * local.y, rotation.y * local.x + rotation.x * local.y);
}

/// @brief FiberClumpedAxis を画素 coordinate の束について逆にたどった根元側の位置 [セル座標]。近傍探索の中心に使う。
float2 FiberClumpedSource(float2 coordinate, float height, float amount)
{
    if (amount <= 0.0f) return coordinate;
    float2 center, rotation;
    float pull;
    FiberClumpAt(coordinate, height, amount, center, pull, rotation);
    float2 local = coordinate - center;
    return center + float2(rotation.x * local.x + rotation.y * local.y, -rotation.y * local.x + rotation.x * local.y) / (1.0f - pull);
}

/// @note 全層が同じセル中心と高さを参照することで、点の集合を連続した繊維にする。
/// @note 論文のボリューム断面を、決定的なセル内分布で近似する。層ごとの独立ノイズは使わない。
/// @note 毛は束へ寄り、視差の上乗せで太りもするのでセルをはみ出す。近傍 3×3 セルの毛から軸に最も近いものを採る。
/// @note マスクは毛の根元でなく画素で引く (9 本ぶん引くと重い)。マスクの境をまたぐ毛はそこで欠ける。
/// @see https://hhoppe.com/fur.pdf
/// @param widen 根元での半径の上乗せ [セル単位]。先細りと同じ比で先端ほど小さくする。
/// @param mask 画素の fiberMask。R を密度、B を束の寄りに掛ける。
FiberStrand FiberClipShell(float2 coordinate, float height, float widen, float4 mask)
{
    float density = fiberDensity * mask.r;
    float clumpAmount = clumping * mask.b;
    float2 base = floor(FiberClumpedSource(coordinate, height, clumpAmount));
    FiberStrand strand = FiberDefaultStrand(height, 0.5f);
    float nearest = 1.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float2 cell = base + float2(x, y);
            if (FiberHash(cell + 19.7f) > density - 1.0e-6f) continue;
            float strandHeight = lerp(0.55f, 1.0f, FiberHash(cell + 73.1f));
            if (height > strandHeight) continue;
            float2 root = cell + 0.25f + 0.5f * float2(FiberHash(cell), FiberHash(cell + 11.3f));
            float radius = (fiberThickness + widen) * (1.0f - fiberTaper * height / strandHeight);
            float2 offset = (coordinate - FiberClumpedAxis(root, height, clumpAmount)) / max(radius, 1.0e-6f);
            float distance = length(offset);
            if (distance < nearest) {
                nearest = distance;
                strand.offset = offset;
                strand.hash = FiberHash(cell + 29.1f);
                strand.tip = height / strandHeight;
            }
        }
    }
    clip(nearest < 1.0f ? 1.0f : -1.0f);
    strand.colorMix = mask.a;
    return strand;
}

/// @brief 影用の Shell の被覆。毛 1 本ずつの形 (円・束・3×3 の探索) を解かず、毛のセル単位で残すか決める。
/// @note 被覆率 = 密度 × (h まで届く毛の割合) × 先細り。毛丈は [0.55,1] の一様分布なので、届く割合は saturate((1-h)/0.45)。根元はほぼ埋まり、毛先ほど薄い。
/// @note 乱数はセルごとで層に依らない。上の層で残るセルは下の層でも残り、毛の柱として影にまとまる。影の画素ごとの乱数は受け手に点のノイズを落とした。
/// @param coordinate FiberRootCoordinates と同じセル座標。
void FiberClipShellShadow(float2 coordinate, float height, float4 mask)
{
    float reach = saturate((1.0f - height) / 0.45f);
    float coverage = fiberDensity * mask.r * reach * saturate(1.0f - fiberTaper * height);
    clip(coverage - FiberHash(floor(coordinate) + 19.7f) - 1.0e-6f);
}

/// @note Fin は毛の側面投影を一次元の分布で近似する。Shell と同じ高さ・先細り分布を使う。
/// @see https://hhoppe.com/fur.pdf
/// @param mask 画素の fiberMask。R を密度に掛け、A を色に渡す。
FiberStrand FiberClipFin(float coordinate, float height, float4 mask)
{
    float cell = floor(coordinate);
    float2 key = float2(cell, 7.0f);
    clip(fiberDensity * mask.r - FiberHash(key + 19.7f) - 1.0e-6f);
    float strandHeight = lerp(0.55f, 1.0f, FiberHash(key + 73.1f));
    clip(strandHeight - height);
    float center = 0.25f + 0.5f * FiberHash(key);
    float radius = fiberThickness * (1.0f - fiberTaper * height / strandHeight);
    float offset = (frac(coordinate) - center) / max(radius, 1.0e-6f);
    clip(1.0f - abs(offset));
    FiberStrand strand = FiberDefaultStrand(height / strandHeight, FiberHash(key + 29.1f));
    strand.offset = float2(offset, 0.0f);
    strand.colorMix = mask.a;
    return strand;
}

/// @brief 毛の色。根元→毛先の色を毛自身の長さで塗り、毛ごとに明度をばらつかせる。
/// @note 草 (planarShading = 1) は従来どおり層の高さで塗る。fiberMask の A (colorMix) で毛先色への進みを抑え、縞や斑を描く。
float3 FiberStrandColor(FiberStrand strand, float height)
{
    float t = lerp(saturate(strand.tip), height, planarShading) * saturate(strand.colorMix);
    float3 color = lerp(rootColor.rgb, tipColor.rgb, t);
#ifdef FBZZ_FIBER_GRASS_CARD
    color *= strand.cardColor;
#endif
    return color * max(1.0f + colorVariation * (strand.hash * 2.0f - 1.0f), 0.0f);
}

/// @brief 毛を円柱として見た画素の法線。
/// @note Kajiya-Kay は法線を T と V の張る面に置く。毛を横切る向き (T×V) へのずれ分だけその面から傾け、1 本ごとの丸みを出す。
/// @see https://web.engr.oregonstate.edu/~mjb/cs557/Projects/Papers/HairRendering.pdf (Scheuermann, Hair Rendering and Shading, GDC 2004: Hair Lighting: Kajiya-Kay Model)
float3 FiberStrandNormal(float3 T, float3 V, float3 N, FiberStrand strand)
{
    float3 facing = FiberNormalize(V - T * dot(V, T), N);
    float3 side = FiberNormalize(cross(T, facing), N);
    float lateral = dot(strand.across, strand.across) > 1.0e-8f ? dot(strand.across, side) : strand.offset.x;
    lateral = clamp(lateral, -1.0f, 1.0f);
    return FiberNormalize(side * lateral + facing * sqrt(saturate(1.0f - lateral * lateral)), facing);
}

/// @note 正のずらしはハイライトを根元へ、負は毛先へ動かす (T は根元→毛先)。
/// @see https://web.engr.oregonstate.edu/~mjb/cs557/Projects/Papers/HairRendering.pdf (Scheuermann GDC 2004: Shifting Specular Highlights, ShiftTangent)
float3 FiberShiftTangent(float3 T, float3 N, float shift)
{
    return FiberNormalize(T + shift * N, T);
}

/// @see https://web.engr.oregonstate.edu/~mjb/cs557/Projects/Papers/HairRendering.pdf (Scheuermann GDC 2004: Specular Strand Lighting, StrandSpecular)
float FiberStrandSpecular(float3 T, float3 V, float3 L, float exponent)
{
    float3 H = FiberNormalize(L + V, T);
    float dotTH = dot(T, H);
    float sinTH = sqrt(saturate(1.0f - dotTH * dotTH));
    return smoothstep(-1.0f, 0.0f, dotTH) * pow(sinTH, exponent);
}

/// @brief 1 光源の直接光。毛皮は Kajiya-Kay にずらした 2 本のハイライト、草は Lambert と透過。
/// @param selfShadow 根元での上の毛による光学的厚み。光が斜めに入るほど長く通る (Banks の自己遮蔽近似)。
/// @note 主ハイライト (R) は毛先寄りで光の色、副ハイライト (TRT) は根元寄りで毛の色を帯び、幅が広く、毛ごとにきらめく (Marschner の観察)。
/// @see https://web.engr.oregonstate.edu/~mjb/cs557/Projects/Papers/HairRendering.pdf (Scheuermann GDC 2004: Hair Lighting: Marschner Model / Putting it All Together)
/// @see https://hhoppe.com/fur.pdf (Lengyel et al., Real-Time Fur over Arbitrary Surfaces, 2.4 Rendering discussion: Hair lighting)
float3 FiberDirect(float3 N, float3 strandNormal, float3 T, float3 V, float3 L, float3 color,
    FiberStrand strand, float selfShadow, float3 radiance)
{
    float strandWeight = 1.0f - planarShading;
    float NdotL = dot(N, L);
    float lambert = saturate(NdotL);
    /// @note lerp(0.25,1,N·L) で影の境界を裏へ回す (sin(T,L) は自己遮蔽なしでは明るすぎる)。毛の丸みは half-Lambert で掛ける。
    float wrapped = saturate(lerp(0.25f, 1.0f, NdotL));
    float strandDiffuse = wrapped * saturate(0.5f + 0.5f * dot(strandNormal, L));
    float diffuse = lerp(lambert, strandDiffuse, strandWeight);
    float backlight = saturate(dot(-L, V)) * transmission * (1.0f - lambert);
    float attenuation = lerp(1.0f, exp(-selfShadow / max(NdotL, 0.15f)), strandWeight);
    float exponent = lerp(256.0f, 8.0f, roughness);
    float jitter = (frac(strand.hash * 7.31f) - 0.5f) * 0.3f;
    float primary = FiberStrandSpecular(FiberShiftTangent(T, strandNormal, jitter - specularShift), V, L, exponent);
    float sparkle = smoothstep(0.35f, 0.85f, frac(strand.hash * 13.7f));
    float secondary = FiberStrandSpecular(FiberShiftTangent(T, strandNormal, jitter + 1.5f * specularShift), V, L, exponent * 0.35f)
        * secondarySpecular * sparkle;
    float3 specular = specularStrength * (primary + secondary * color) * wrapped * strandWeight;
    return radiance * ((color * diffuse + specular) * attenuation + color * (planarShading * backlight));
}

/// @param localFlow 頂点で評価した局所 FlowField。ピクセルごとに場を引き直さない。
/// @param strand coverage が選んだ毛。
float4 FiberLighting(float3 position, float3 normal, float3 root, float height, float2 pixel,
    float3 localFlow, FiberStrand strand, float2 cardMotion)
{
    float3 N = FiberNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    float3 T = FiberNormalize(N * fiberLength + 2.0f * height * FiberBend(root, N, localFlow, cardMotion), N);
    float3 V = FiberViewDirection(position, N);
    float3 L = FiberNormalize(-lightDir, N);
    float3 strandNormal = FiberStrandNormal(T, V, N, strand);
    float3 color = FiberStrandColor(strand, height);
    float ao = (1.0f - rootOcclusion * (1.0f - height)) * FBZZ_ScreenAO(pixel);
    /// @note rootOcclusion を根元での光学的厚みの目安にも使う。値を分けると同じ «根元の暗さ» を 2 か所で合わせることになる。
    float selfShadow = 1.5f * rootOcclusion * (1.0f - height);
    float shadow = ComputeShadow(texShadow, sampShadow, position,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    shadow *= FBZZ_ScreenContactShadow(pixel);
    float3 result = color * ambientColor * ao;
    if (iblIntensity > 0.0f) {
        /// @note 環境反射は繊維の法線を使う非金属 split-sum 近似。直接光の二葉ローブとは別に評価し、定数環境光を置換する。
        /// @see Rendering/IBL.hlsli EvaluateIBL の放射照度・AO・鏡面遮蔽の契約。
        result = EvaluateIBL(position, strandNormal, V, color, 0.0f, clamp(roughness, 0.045f, 1.0f), ao,
            texFiberIrradiance, texFiberPrefilter, texFiberBRDFLut, iblMaxMipLevel,
            iblDiffuseScale, iblSpecularScale * max(specularStrength, 0.0f),
            fiberMaskSampler, fiberFlowSampler) * iblIntensity;
    }
    result += FiberDirect(N, strandNormal, T, V, L, color, strand, selfShadow, lightColor * lightIntensity * shadow);
    FBZZ_PUNCTUAL_BEGIN(position, pixel, N)
        result += FiberDirect(N, strandNormal, T, V, ps.L, color, strand, selfShadow, ps.color * ps.intensity);
    FBZZ_PUNCTUAL_END
    return float4(result, 1.0f);
}
#endif
