/// @file    FlowField.hlsli
/// @brief   流れの場 (FlowField) の GPU 評価。GPU 粒子と表面繊維が同じ式で媒質速度を得る。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see Docs/design/flow-field.md
#ifndef FBZZ_RENDERING_FLOW_FIELD_HLSLI
#define FBZZ_RENDERING_FLOW_FIELD_HLSLI

#include "Rendering/ParticleNoise.hlsli"

/// @brief 流れ 1 本分 (96 bytes)。StructuredBuffer で渡すので本数に上限が無い。
/// @note LAYOUT: RenderPassContext.hpp の GpuFlowField と完全に一致させること。
struct GpuFlowField
{
    float4 posRadius;     ///< @brief xyz=ワールド位置, w=影響半径 (<=0 で無限)
    float4 dirStrength;   ///< @brief xyz=流向/渦軸 (正規化済み), w=流速 [m/s]
    float4 params;        ///< @brief x=種類(FlowFieldType), y=falloffPower, z=noiseFrequency, w=noiseSpeed
    float4 fieldRotation; ///< @brief Baked: ワールド → 場のローカルへ戻す逆回転 (クォータニオン)
    float4 fieldExtents;  ///< @brief Baked: xyz=ワールド半径 [m], w=予約
    /// @brief x=Baked のアトラスタイル番号 (<0 で無効), y=maxMagnitude, z=channels のビット列 (asuint で読む), w=予約
    float4 fieldTile;
};

/// @note FlowFieldType と数値を一致させる。LEGACY_DRAG は読み込み専用だが番号は詰めない。
#define FF_UNIFORM     0
#define FF_SINK        1
#define FF_SOURCE      2
#define FF_VORTEX      3
#define FF_CURL        4
#define FF_LEGACY_DRAG 5
#define FF_BAKED       6

/// @note 速度場アトラスの 1 タイルの 1 辺。VectorFieldAsset.hpp の kVelocityFieldTileResolution と一致させる。
#define VELOCITY_FIELD_TILE 32

/// @note channels にこの値を渡すと場ごとのチャンネル判定を省く (CPU で絞り込み済みの GPU 粒子)。
#define FLOW_ALL_CHANNELS 0xFFFFFFFFu

/// @brief ベクトルポテンシャルの回転 (∇×ψ) を中心差分で求める発散ゼロの乱流。
/// @note 式は Math/CurlNoise.hpp と一致させる。CPU の FlowFieldEval と同じ値を出す。
float3 CurlNoise(float3 p)
{
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

/// @note 時間スクロールは軸ごとに速度を変え、場全体が一方向へ流れて見えないようにする (CPU と一致)。
float3 TurbulenceSamplePoint(float3 position, float frequency, float speed, float time)
{
    float scroll = time * speed;
    return position * frequency + float3(scroll, scroll * 0.35f, scroll * 0.7f);
}

/// @brief クォータニオン q でベクトル v を回す。CPU の math::Quaternion::operator* と同じ式。
float3 QuatRotate(float4 q, float3 v)
{
    const float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

/// @brief 速度場アトラスからタイル 1 枚ぶんを引く。
/// @param local 場のローカル正規化座標 [0,1]³
/// @note タイルは Z 方向に積んである。ハードウェアのトリリニアでは境界で隣の場が混ざるため、タイル内の 2 スライスを自分で補間する。
float3 SampleVelocityField(Texture3D<float4> atlas, SamplerState samp, uint tile, float3 local, float maxMagnitude)
{
    uint atlasW, atlasH, atlasD;
    atlas.GetDimensions(atlasW, atlasH, atlasD);
    const float tileTexels = (float)VELOCITY_FIELD_TILE;

    const float z  = local.z * tileTexels - 0.5f;
    const float z0 = clamp(floor(z), 0.0f, tileTexels - 1.0f);
    const float z1 = min(z0 + 1.0f, tileTexels - 1.0f);
    const float t  = saturate(z - z0);

    const float base = (float)tile * tileTexels;
    const float3 a = atlas.SampleLevel(samp, float3(local.xy, (base + z0 + 0.5f) / (float)atlasD), 0.0f).rgb;
    const float3 b = atlas.SampleLevel(samp, float3(local.xy, (base + z1 + 0.5f) / (float)atlasD), 0.0f).rgb;
    /// @note 復元は CPU の QuantizeVectorFieldValue と同じ式。
    return (lerp(a, b, t) * 2.0f - 1.0f) * maxMagnitude;
}

/// @brief fields[first, first+count) を合成した 1 点の媒質速度 [m/s]。
/// @param channels 受け手のチャンネル。FLOW_ALL_CHANNELS なら場ごとの判定を省く。
/// @param covered この点を覆う場が 1 本でもあったか。
/// @note 式は FlowFieldEval.cpp の SampleFlow と一致させる。
float3 SampleFlowFields(StructuredBuffer<GpuFlowField> fields, uint first, uint count, uint channels,
                        Texture3D<float4> atlas, SamplerState samp, float3 position, float time, out bool covered)
{
    float3 flow = float3(0.0f, 0.0f, 0.0f);
    covered = false;

    [loop]
    for (uint fi = first; fi < first + count; ++fi)
    {
        GpuFlowField f = fields[fi];
        if (channels != FLOW_ALL_CHANNELS && (asuint(f.fieldTile.z) & channels) == 0u) continue;
        float3 toPoint = position - f.posRadius.xyz;
        float  radius  = f.posRadius.w;
        float  influence = 1.0f;
        if (radius > 0.0f)
        {
            float dist = length(toPoint);
            if (dist >= radius) continue;
            /// @note dist < radius で底は正だが、分岐条件を考慮しない警告 X3571 を abs で避ける。
            influence = pow(abs(1.0f - dist / radius), f.params.y);
        }
        uint fieldType = (uint)f.params.x;
        if (fieldType == FF_LEGACY_DRAG) continue;
        covered = true;

        float scale = f.dirStrength.w * influence;
        if (fieldType == FF_UNIFORM)
        {
            flow += f.dirStrength.xyz * scale;
        }
        else if (fieldType == FF_SINK || fieldType == FF_SOURCE)
        {
            float  dist   = max(length(toPoint), 1.0e-4f);
            float3 radial = toPoint / dist;
            flow += radial * (fieldType == FF_SOURCE ? scale : -scale);
        }
        else if (fieldType == FF_VORTEX)
        {
            float3 tangent = cross(f.dirStrength.xyz, toPoint);
            float  len     = length(tangent);
            if (len > 1.0e-4f)
                flow += tangent * (scale / len);
        }
        else if (fieldType == FF_CURL)
        {
            flow += CurlNoise(TurbulenceSamplePoint(position, f.params.z, f.params.w, time)) * scale;
        }
        else if (fieldType == FF_BAKED)
        {
            /// @note 常駐できなかった場は tile < 0 で届く。詰めると本数がずれて別の流れに化けるので、ここで捨てる。
            if (f.fieldTile.x < 0.0f) continue;
            const float3 localOffset = QuatRotate(f.fieldRotation, toPoint);
            const float3 uvw = localOffset / f.fieldExtents.xyz * 0.5f + 0.5f;
            if (any(uvw < 0.0f) || any(uvw > 1.0f)) continue;
            const float3 fieldValue = SampleVelocityField(atlas, samp, (uint)f.fieldTile.x, uvw, f.fieldTile.y);
            /// @note 焼いた値は既に [m/s] なので、Baked の strength だけは無次元の倍率。
            flow += QuatRotate(float4(-f.fieldRotation.xyz, f.fieldRotation.w), fieldValue) * scale;
        }
    }
    return flow;
}

#endif
