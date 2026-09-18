/// @file    TerrainSurface.hlsli
/// @brief   地形の可変レイヤー合成 (候補収集・自動/高さブレンド・三方向投影・マクロ変化) を Forward と GBuffer で共有する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
///
/// @note LAYOUT: TerrainCB は RenderPassContext.hpp の TerrainObjectCB、TerrainLayer は TerrainLayerGpu と一致させること。
/// @note t0 = 層番号マップ (RGBA8 UNORM)、t1 = 重みマップ。層テクスチャは TerrainLayer の bindless 添字から引く。
/// @see Docs/design/terrain-layers.md
#ifndef TERRAIN_SURFACE_HLSLI
#define TERRAIN_SURFACE_HLSLI

#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/Color.hlsli"

cbuffer TerrainCB : register(CB_OBJECT)
{
    float4x4 worldMatrix;      ///< テレインローカル → ワールド
    float4x4 wvpMatrix;        ///< テレインローカル → クリップ
    float4   weather;          ///< x=wetness y=darkening z=puddleAmount
    float4   terrainParams;    ///< x=ローカル幅 X [m] y=ローカル奥行 Z [m] z=heightBlendDepth w=自動ブレンドを持つ層があるか
    uint     layerBufferIndex; ///< TerrainLayer 配列の bindless 添字
    uint     layerCount;
    uint     splatColumns;
    uint     splatRows;
};

/// @brief 1 層ぶんのパラメーター。テクスチャ添字は常に有効 (パスが 1x1 の代替を詰める)。
struct TerrainLayer
{
    uint  diffuseIndex;
    uint  normalIndex;
    uint  aoRoughnessIndex;
    uint  heightIndex;
    float tilingX;
    float tilingZ;
    float normalStrength;
    float roughness;
    float ambientOcclusion;
    float hasAoRoughness;
    float hasHeight;
    float heightBlend;
    float autoMinHeight;
    float autoMaxHeight;
    float autoHeightFade;
    float autoBlendEnabled;
    float autoMinSlope;
    float autoMaxSlope;
    float autoSlopeFade;
    float autoBlendStrength;
    float triplanar;
    float triplanarSharpness;
    float macroScale;
    float macroStrength;
};

FBZZ_TEX2D_T(float4, g_splatIndices, 0);
FBZZ_TEX2D_T(float4, g_splatWeights, 1);
static StructuredBuffer<TerrainLayer> g_terrainLayers = ResourceDescriptorHeap[NonUniformResourceIndex(layerBufferIndex)];

Texture2D TerrainTexture(uint index) { return ResourceDescriptorHeap[NonUniformResourceIndex(index)]; }

/// @note 地形は画面を広く覆い、層ごとに複数枚を引くので x16 ではなく x4 の異方性にする。見た目はほぼ同じで約 1/3 のコスト。
SamplerState g_sampler : register(SAMPLER_WRAP_ANISO4);

struct TerrainVSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};

struct TerrainPSInput
{
    float4 svPosition   : SV_POSITION;
    float3 worldPos     : TEXCOORD0;
    float3 localPos     : TEXCOORD1;
    float3 localNormal  : TEXCOORD2;
    float3 localTangent : TEXCOORD3;
    float2 uv           : TEXCOORD4;
};

TerrainPSInput VSMain(TerrainVSInput v)
{
    TerrainPSInput o;
    o.svPosition   = mul(float4(v.position, 1.0f), wvpMatrix);
    o.worldPos     = mul(float4(v.position, 1.0f), worldMatrix).xyz;
    o.localPos     = v.position;
    o.localNormal  = v.normal;
    o.localTangent = v.tangent;
    o.uv           = v.uv;
    return o;
}

#define TERRAIN_MAX_CANDIDATES 8
#define TERRAIN_MAX_BLEND      4

/// @brief 番号ごとに重みを合算した候補表。
struct TerrainCandidates
{
    uint  layer[TERRAIN_MAX_CANDIDATES];
    float weight[TERRAIN_MAX_CANDIDATES];
    uint  count;
};

void AddCandidate(inout TerrainCandidates c, uint layer, float weight)
{
    if (weight <= 0.0f)
        return;
    [loop]
    for (uint i = 0; i < c.count; ++i)
    {
        if (c.layer[i] == layer)
        {
            c.weight[i] += weight;
            return;
        }
    }
    if (c.count < TERRAIN_MAX_CANDIDATES)
    {
        c.layer[c.count]  = layer;
        c.weight[c.count] = weight;
        ++c.count;
        return;
    }
    /// @note 表が埋まっていたら最も軽い候補と入れ替える。9 層以上が 1 画素に重なるのは 4 近傍の境界だけ。
    uint lightest = 0;
    [loop]
    for (uint j = 1; j < c.count; ++j)
        if (c.weight[j] < c.weight[lightest]) lightest = j;
    if (weight > c.weight[lightest])
    {
        c.layer[lightest]  = layer;
        c.weight[lightest] = weight;
    }
}

void NormalizeCandidates(inout TerrainCandidates c)
{
    float sum = 0.0f;
    [loop]
    for (uint i = 0; i < c.count; ++i) sum += c.weight[i];
    if (sum <= 1e-5f)
    {
        c.count     = 1;
        c.layer[0]  = 0;
        c.weight[0] = 1.0f;
        return;
    }
    [loop]
    for (uint k = 0; k < c.count; ++k) c.weight[k] /= sum;
}

/// @brief 番号マップを 4 近傍で読み、双線形の重みで候補へ合算する。
/// @note 番号はフィルタできないので Load で読む。頂点 i は uv = i / (columns - 1) にある。
TerrainCandidates GatherPaintedCandidates(float2 uv)
{
    TerrainCandidates c;
    c.count = 0;
    const uint maxLayer = max(layerCount, 1u) - 1u;
    const int2 size = int2(max(splatColumns, 1u), max(splatRows, 1u));
    const float2 texel = saturate(uv) * float2(max(size - 1, int2(0, 0)));
    const int2 base = clamp(int2(floor(texel)), int2(0, 0), max(size - 2, int2(0, 0)));
    const float2 f = saturate(texel - float2(base));

    [loop]
    for (int dy = 0; dy < 2; ++dy)
    {
        [loop]
        for (int dx = 0; dx < 2; ++dx)
        {
            const float bilinear = (dx == 0 ? 1.0f - f.x : f.x) * (dy == 0 ? 1.0f - f.y : f.y);
            if (bilinear <= 0.0f)
                continue;
            const int2 coord = min(base + int2(dx, dy), size - 1);
            const float4 indices = g_splatIndices.Load(int3(coord, 0));
            const float4 weights = g_splatWeights.Load(int3(coord, 0));
            [loop]
            for (uint s = 0; s < 4; ++s)
            {
                const uint layer = (uint)round(indices[s] * 255.0f);
                if (layer > maxLayer)
                    continue;
                AddCandidate(c, layer, weights[s] * bilinear);
            }
        }
    }
    NormalizeCandidates(c);
    return c;
}

/// @brief [minValue, maxValue] を 1、外を 0 に近づける滑らかなマスク。境界を硬く切ると等高線状の境目が見える。
float TerrainBandMask(float value, float minValue, float maxValue, float fade)
{
    if (maxValue <= minValue)
        return 1.0f;
    const float f = max(fade, 0.0001f);
    const float lower = smoothstep(minValue, minValue + f, value);
    const float upper = 1.0f - smoothstep(maxValue - f, maxValue, value);
    return saturate(lower * upper);
}

/// @brief 高さ帯 × 傾斜帯の自動ブレンドを塗りへ混ぜる。混ぜ方は lerp(塗り, 自動, 最大強度)。
void ApplyAutoBlend(inout TerrainCandidates c, float localHeight, float slope)
{
    TerrainCandidates autoC;
    autoC.count = 0;
    float autoSum = 0.0f;
    float blend   = 0.0f;
    [loop]
    for (uint i = 0; i < layerCount; ++i)
    {
        const TerrainLayer layer = g_terrainLayers[i];
        const float strength = saturate(layer.autoBlendStrength) * saturate(layer.autoBlendEnabled);
        if (strength <= 0.0f)
            continue;
        blend = max(blend, strength);
        const float mask = TerrainBandMask(localHeight, layer.autoMinHeight, layer.autoMaxHeight, layer.autoHeightFade)
                         * TerrainBandMask(slope, layer.autoMinSlope, layer.autoMaxSlope, layer.autoSlopeFade)
                         * strength;
        AddCandidate(autoC, i, mask);
        autoSum += mask;
    }
    if (autoSum <= 0.001f)
        return;

    [loop]
    for (uint k = 0; k < c.count; ++k) c.weight[k] *= 1.0f - blend;
    [loop]
    for (uint j = 0; j < autoC.count; ++j) AddCandidate(c, autoC.layer[j], autoC.weight[j] / autoSum * blend);
    NormalizeCandidates(c);
}

/// @brief 重みの大きい順に最大 4 候補だけ残して正規化する。
void KeepTopCandidates(inout TerrainCandidates c)
{
    const uint keep = min(c.count, (uint)TERRAIN_MAX_BLEND);
    [loop]
    for (uint k = 0; k < keep; ++k)
    {
        uint best = k;
        [loop]
        for (uint j = k + 1; j < c.count; ++j)
            if (c.weight[j] > c.weight[best]) best = j;
        const uint layer = c.layer[k];
        const float weight = c.weight[k];
        c.layer[k] = c.layer[best];
        c.weight[k] = c.weight[best];
        c.layer[best] = layer;
        c.weight[best] = weight;
    }
    c.count = keep;
    NormalizeCandidates(c);
}

/// @brief 1 候補ぶんの投影 UV と勾配。三方向投影でない層は top だけを使う。
struct TerrainProjection
{
    float2 uvTop;  float2 dxTop;  float2 dyTop;
    float2 uvX;    float2 dxX;    float2 dyX;
    float2 uvZ;    float2 dxZ;    float2 dyZ;
    float3 weights;   ///< x=側面 X, y=上面, z=側面 Z (合計 1)
    float3 axisSign;
};

/// @note 三方向投影の側面は «1 m あたりの繰り返し数» を上面と揃え、崖で模様の大きさが変わらないようにする。
/// @see https://bgolus.medium.com/normal-mapping-for-a-triplanar-shader-10bf39dca05a
TerrainProjection MakeProjection(TerrainLayer layer, float2 uv, float2 duvdx, float2 duvdy,
                                 float3 localPos, float3 dPdx, float3 dPdy, float3 localNormal)
{
    TerrainProjection o;
    const float2 tiling = float2(layer.tilingX, layer.tilingZ);
    o.uvTop = uv * tiling;
    o.dxTop = duvdx * tiling;
    o.dyTop = duvdy * tiling;
    o.weights  = float3(0.0f, 1.0f, 0.0f);
    o.axisSign = float3(1.0f, 1.0f, 1.0f);
    o.uvX = o.dxX = o.dyX = float2(0.0f, 0.0f);
    o.uvZ = o.dxZ = o.dyZ = float2(0.0f, 0.0f);
    if (layer.triplanar < 0.5f)
        return o;

    float3 w = pow(abs(localNormal), max(layer.triplanarSharpness, 1.0f));
    w /= max(w.x + w.y + w.z, 1e-5f);
    /// @note 側面の寄与が 1% 未満なら上面 1 回に落とす。平地で 3 倍のフェッチを払わない。
    if (w.x < 0.01f) w.x = 0.0f;
    if (w.z < 0.01f) w.z = 0.0f;
    w /= max(w.x + w.y + w.z, 1e-5f);
    o.weights = w;
    o.axisSign = float3(localNormal.x < 0.0f ? -1.0f : 1.0f, 1.0f, localNormal.z < 0.0f ? -1.0f : 1.0f);

    const float perMeterX = layer.tilingX / max(terrainParams.x, 1e-3f);
    const float perMeterZ = layer.tilingZ / max(terrainParams.y, 1e-3f);
    o.uvX = float2(localPos.z * perMeterZ * o.axisSign.x, localPos.y * perMeterX);
    o.dxX = float2(dPdx.z * perMeterZ * o.axisSign.x, dPdx.y * perMeterX);
    o.dyX = float2(dPdy.z * perMeterZ * o.axisSign.x, dPdy.y * perMeterX);
    o.uvZ = float2(localPos.x * perMeterX * -o.axisSign.z, localPos.y * perMeterZ);
    o.dxZ = float2(dPdx.x * perMeterX * -o.axisSign.z, dPdx.y * perMeterZ);
    o.dyZ = float2(dPdy.x * perMeterX * -o.axisSign.z, dPdy.y * perMeterZ);
    return o;
}

float4 SampleProjected(Texture2D tex, TerrainProjection pr)
{
    float4 result = tex.SampleGrad(g_sampler, pr.uvTop, pr.dxTop, pr.dyTop) * pr.weights.y;
    if (pr.weights.x > 0.0f)
        result += tex.SampleGrad(g_sampler, pr.uvX, pr.dxX, pr.dyX) * pr.weights.x;
    if (pr.weights.z > 0.0f)
        result += tex.SampleGrad(g_sampler, pr.uvZ, pr.dxZ, pr.dyZ) * pr.weights.z;
    return result;
}

/// @brief 投影ごとのタンジェント空間法線を Whiteout ブレンドでローカル空間へ合成する。
/// @see https://bgolus.medium.com/normal-mapping-for-a-triplanar-shader-10bf39dca05a
float3 SampleProjectedNormal(Texture2D tex, TerrainProjection pr, float strength,
                             float3 Ng, float3 T, float3 B)
{
    float3 top = tex.SampleGrad(g_sampler, pr.uvTop, pr.dxTop, pr.dyTop).xyz * 2.0f - 1.0f;
    top.xy *= strength;
    if (pr.weights.x <= 0.0f && pr.weights.z <= 0.0f)
    {
        const float3 tn = normalize(top);
        return normalize(T * tn.x + B * tn.y + Ng * tn.z);
    }

    float3 blended = float3(0.0f, 0.0f, 0.0f);
    if (pr.weights.x > 0.0f)
    {
        float3 tn = tex.SampleGrad(g_sampler, pr.uvX, pr.dxX, pr.dyX).xyz * 2.0f - 1.0f;
        tn.xy *= strength;
        tn.x  *= pr.axisSign.x;
        tn = float3(tn.xy + Ng.zy, abs(tn.z) * Ng.x);
        blended += tn.zyx * pr.weights.x;
    }
    if (pr.weights.z > 0.0f)
    {
        float3 tn = tex.SampleGrad(g_sampler, pr.uvZ, pr.dxZ, pr.dyZ).xyz * 2.0f - 1.0f;
        tn.xy *= strength;
        tn.x  *= -pr.axisSign.z;
        tn = float3(tn.xy + Ng.xy, abs(tn.z) * Ng.z);
        blended += tn.xyz * pr.weights.z;
    }
    {
        float3 tn = float3(top.xy + Ng.xz, abs(top.z) * Ng.y);
        blended += tn.xzy * pr.weights.y;
    }
    return normalize(blended);
}

/// @brief 合成済みの地表。法線はワールド空間。
struct TerrainSurface
{
    float3 albedo;     ///< 線形色
    float  roughness;
    float  ao;
    float3 normal;     ///< 法線マップ適用後のワールド法線
    float3 geometricNormal;
};

/// @brief 地形 1 画素ぶんの材質を合成する。
/// @param normalBoost タンジェント法線 XY の増幅率。Deferred は鏡面に頼れないので 3、Forward は 1。
TerrainSurface EvaluateTerrainSurface(TerrainPSInput p, float normalBoost)
{
    const float3 Nl = normalize(p.localNormal);
    const float3 Tl = normalize(p.localTangent - Nl * dot(Nl, p.localTangent));
    const float3 Bl = normalize(cross(Nl, Tl));
    const float3 NgWorld = normalize(mul(Nl, (float3x3)worldMatrix));

    /// @note 勾配はすべて分岐の外で一度だけ取る。候補ループは層ごとにフェッチを省くため、
    ///       分岐内の Sample はクアッド内で制御フローが割れた画素で勾配が未定義になる。
    const float2 duvdx = ddx(p.uv);
    const float2 duvdy = ddy(p.uv);
    const float3 dPdx  = ddx(p.localPos);
    const float3 dPdy  = ddy(p.localPos);

    TerrainCandidates c = GatherPaintedCandidates(p.uv);
    if (terrainParams.w > 0.5f)
        ApplyAutoBlend(c, p.localPos.y, saturate(1.0f - abs(NgWorld.y)));
    KeepTopCandidates(c);

    float3 albedos[TERRAIN_MAX_BLEND];
    float  heights[TERRAIN_MAX_BLEND];
    TerrainProjection projections[TERRAIN_MAX_BLEND];
    float maxHeightBlend = 0.0f;

    [loop]
    for (uint k = 0; k < c.count; ++k)
    {
        const TerrainLayer layer = g_terrainLayers[c.layer[k]];
        projections[k] = MakeProjection(layer, p.uv, duvdx, duvdy, p.localPos, dPdx, dPdy, Nl);
        Texture2D diffuse = TerrainTexture(layer.diffuseIndex);
        float3 albedo = SRGBToLinear(SampleProjected(diffuse, projections[k]).rgb);

        /// @note マクロ変化: 引き伸ばした同じテクスチャの輝度を «テクスチャ全体の平均» で割って掛ける。
        ///       平均で割らないと暗い素材ほど全体が暗くなるだけの乗算になる。
        if (layer.macroStrength > 0.0f)
        {
            const float scale = max(layer.macroScale, 1e-3f);
            const float3 far  = SRGBToLinear(diffuse.SampleGrad(g_sampler, projections[k].uvTop * scale,
                                                                projections[k].dxTop * scale,
                                                                projections[k].dyTop * scale).rgb);
            const float3 mean = SRGBToLinear(diffuse.SampleLevel(g_sampler, projections[k].uvTop, 16.0f).rgb);
            const float ratio = clamp(Luminance(far) / max(Luminance(mean), 1e-3f), 0.25f, 2.0f);
            albedo *= lerp(1.0f, ratio, saturate(layer.macroStrength));
        }
        albedos[k] = albedo;

        const float heightBlend = saturate(layer.heightBlend);
        maxHeightBlend = max(maxHeightBlend, heightBlend);
        heights[k] = 0.0f;
        if (heightBlend > 0.0f)
        {
            heights[k] = layer.hasHeight > 0.5f
                ? SampleProjected(TerrainTexture(layer.heightIndex), projections[k]).r
                : Luminance(albedo);
            heights[k] *= heightBlend;
        }
    }

    /// @note 高さブレンド: h = w + height, ma = max(h) - depth, b = max(h - ma, 0)。heightBlend = 0 の層だけなら w のまま。
    /// @see https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting
    if (maxHeightBlend > 0.0f)
    {
        float hMax = -1e9f;
        float h[TERRAIN_MAX_BLEND];
        [loop]
        for (uint i = 0; i < c.count; ++i)
        {
            h[i] = c.weight[i] + heights[i];
            hMax = max(hMax, h[i]);
        }
        const float ma = hMax - max(terrainParams.z, 1e-3f);
        float bSum = 0.0f;
        float b[TERRAIN_MAX_BLEND];
        [loop]
        for (uint j = 0; j < c.count; ++j)
        {
            b[j] = max(h[j] - ma, 0.0f);
            bSum += b[j];
        }
        if (bSum > 1e-5f)
        {
            [loop]
            for (uint m = 0; m < c.count; ++m)
                c.weight[m] = lerp(c.weight[m], b[m] / bSum, maxHeightBlend);
        }
    }

    TerrainSurface s;
    s.albedo    = float3(0.0f, 0.0f, 0.0f);
    s.roughness = 0.0f;
    s.ao        = 0.0f;
    float3 blendedN = float3(0.0f, 0.0f, 0.0f);
    float  total    = 0.0f;

    [loop]
    for (uint n = 0; n < c.count; ++n)
    {
        const float w = c.weight[n];
        if (w <= 0.001f)
            continue;
        const TerrainLayer layer = g_terrainLayers[c.layer[n]];
        const float2 aoRough = SampleProjected(TerrainTexture(layer.aoRoughnessIndex), projections[n]).rg;
        const float hasTex = saturate(layer.hasAoRoughness);
        s.albedo    += albedos[n] * w;
        s.roughness += lerp(saturate(layer.roughness), aoRough.g, hasTex) * w;
        s.ao        += lerp(saturate(layer.ambientOcclusion), aoRough.r, hasTex) * w;
        blendedN    += SampleProjectedNormal(TerrainTexture(layer.normalIndex), projections[n],
                                             layer.normalStrength * normalBoost, Nl, Tl, Bl) * w;
        total       += w;
    }
    if (total > 1e-5f)
    {
        s.albedo    /= total;
        s.roughness /= total;
        s.ao        /= total;
    }

    const float3 Nlocal = dot(blendedN, blendedN) > 1e-10f ? normalize(blendedN) : Nl;
    s.normal          = normalize(mul(Nlocal, (float3x3)worldMatrix));
    s.geometricNormal = NgWorld;
    return s;
}

#endif
