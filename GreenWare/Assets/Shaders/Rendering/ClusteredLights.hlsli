// FBZZ Engine
// ClusteredLights.hlsli | Rendering
// 点光源 / スポットライトの走査を 1 か所へ集約する層
//
// 各マテリアルシェーダーは「点光源ループ」と「スポットループ」を別々に手書きしていたが、
// 中身は全ファイルで同一だった。ここへ畳んでおくことで、ライトの供給元
// (b3 の固定長 cbuffer / StructuredBuffer / クラスタリスト) を差し替えても
// 呼び出し側のシェーダーを書き換えずに済む。
//
// NOTE: Lighting.hlsli の末尾から include される。LightAttenuation / SpotConeWeight /
//       SafeNormalize の定義より後でなければならない (HLSL は前方宣言を持たないため)。
#ifndef CLUSTERED_LIGHTS_HLSLI
#define CLUSTERED_LIGHTS_HLSLI

#include "Common/ClusterConstants.hlsli"

// ピクセルシェーダーが読むライトデータ。
// 束縛されていないフレームでは clusterLightMode == LEGACY になるため、これらは読まれない。
StructuredBuffer<PunctualLight> gPunctualLights : register(SB_PUNCTUAL_LIGHTS);
StructuredBuffer<uint>          gClusterIndices : register(SB_CLUSTER_INDICES);

// 1 本ぶんの評価結果。intensity には距離減衰とスポットコーンを織り込み済み。
struct PunctualSample
{
    float3 L;
    float3 color;
    float  intensity;
};

// -------------------------------------------------------------------------
// 走査範囲の決定
//   count : このピクセルで評価するライト本数
//   base  : gClusterIndices 内の読み出し開始位置 (CLUSTERED 以外では未使用)
// -------------------------------------------------------------------------
void FBZZ_PunctualRange(float2 svXY, float3 worldPos, out uint count, out uint base)
{
    count = 0;
    base  = 0;

    if (clusterLightMode == FBZZ_LIGHT_MODE_CLUSTERED)
    {
        // ビュー空間深度 → Z スライス。view 行列は b0 (CameraConstants)。
        const float viewZ = mul(float4(worldPos, 1.0f), view).z;
        const uint3 coord = uint3(
            (uint)clamp((int)(svXY.x / max(clusterTilePx.x, 1e-4f)), 0, FBZZ_CLUSTER_GRID_X - 1),
            (uint)clamp((int)(svXY.y / max(clusterTilePx.y, 1e-4f)), 0, FBZZ_CLUSTER_GRID_Y - 1),
            FBZZ_ClusterSliceFromViewZ(viewZ));

        base  = FBZZ_ClusterIndex(coord) * FBZZ_CLUSTER_STRIDE;
        count = min(gClusterIndices[base], (uint)FBZZ_MAX_LIGHTS_PER_CLUSTER);
        base += 1; // 先頭要素は個数なので、ライト番号はその次から
    }
    else if (clusterLightMode == FBZZ_LIGHT_MODE_LINEAR)
    {
        count = min(punctualLightCount, (uint)FBZZ_MAX_PUNCTUAL_LIGHTS);
    }
    else // FBZZ_LIGHT_MODE_LEGACY
    {
        // 点光源のあとにスポットが続く 1 本のインデックス空間として扱う。
        // WHY 連結するか: 呼び出し側のループを 1 重で書けるようにするため。
        //     加算の順序は従来 (点 → スポット) と同じなので、丸め誤差まで一致する。
        count = (uint)max(pointLightCount, 0) + (uint)max(spotLightCount, 0);
    }
}

// 走査位置 i のライトを取り出す。
PunctualLight FBZZ_PunctualAt(uint base, uint i)
{
    PunctualLight lt;

    if (clusterLightMode == FBZZ_LIGHT_MODE_CLUSTERED)
    {
        lt = gPunctualLights[gClusterIndices[base + i]];
    }
    else if (clusterLightMode == FBZZ_LIGHT_MODE_LINEAR)
    {
        lt = gPunctualLights[i];
    }
    else // FBZZ_LIGHT_MODE_LEGACY — b3 の固定長配列から詰め替える
    {
        const uint pointCount = (uint)max(pointLightCount, 0);
        if (i < pointCount)
        {
            lt.position  = pointLights[i].position;
            lt.range     = pointLights[i].range;
            lt.color     = pointLights[i].color;
            lt.intensity = pointLights[i].intensity;
            lt.direction = float3(0.0f, -1.0f, 0.0f);
            lt.innerCos  = 0.0f;
            lt.outerCos  = 0.0f;
            lt.type      = FBZZ_LIGHT_TYPE_POINT;
        }
        else
        {
            const uint s = i - pointCount;
            lt.position  = spotLights[s].position;
            lt.range     = spotLights[s].range;
            lt.color     = spotLights[s].color;
            lt.intensity = spotLights[s].intensity;
            lt.direction = spotLights[s].direction;
            lt.innerCos  = spotLights[s].innerCos;
            lt.outerCos  = spotLights[s].outerCos;
            lt.type      = FBZZ_LIGHT_TYPE_SPOT;
        }
        lt._lightPad = float2(0.0f, 0.0f);
    }
    return lt;
}

// 距離減衰とスポットコーンを解く。
// WHY 減衰式をここで再実装しないか: Lighting.hlsli の LightAttenuation / SpotConeWeight を
//     そのまま呼ぶことが、「レガシー経路と見た目が完全一致する」ことの根拠そのものになる。
//     式を書き写すと、片方だけ直したときに静かにズレる。
PunctualSample FBZZ_EvalPunctual(PunctualLight lt, float3 worldPos, float3 N)
{
    const float3 toLight = lt.position - worldPos;
    const float  dist    = length(toLight);

    PunctualSample s;
    s.L         = SafeNormalize(toLight, N);
    s.color     = lt.color;
    s.intensity = lt.intensity * LightAttenuation(dist, lt.range);
    if (lt.type == FBZZ_LIGHT_TYPE_SPOT)
        s.intensity *= SpotConeWeight(s.L, lt.direction, lt.innerCos, lt.outerCos);
    return s;
}

// -------------------------------------------------------------------------
// 呼び出し側のイテレータ
//
//   FBZZ_PUNCTUAL_BEGIN(worldPos, svPosition.xy, N)
//       result += Lighting_PBR_Direct(N, V, ps.L, col, met, rough, ps.color, ps.intensity);
//   FBZZ_PUNCTUAL_END
//
// ループ変数はマクロ内で完結させ、呼び出し側の名前と衝突しないよう _fbzz 接頭辞を付ける。
// デバッグヒートマップもここで処理し、Forward / Deferred の両方で同じ可視化結果にする。
// -------------------------------------------------------------------------
#define FBZZ_PUNCTUAL_BEGIN(_worldPos, _svXY, _N)                                \
    {                                                                            \
        const float3 _fbzzWorldPos = (_worldPos);                                \
        const float2 _fbzzSvXY     = (_svXY);                                    \
        uint _fbzzCount, _fbzzBase;                                              \
        FBZZ_PunctualRange(_fbzzSvXY, _fbzzWorldPos, _fbzzCount, _fbzzBase);      \
        [loop] for (uint _fbzzIdx = 0; _fbzzIdx < _fbzzCount; ++_fbzzIdx)        \
        {                                                                        \
            PunctualSample ps = FBZZ_EvalPunctual(                               \
                FBZZ_PunctualAt(_fbzzBase, _fbzzIdx), _fbzzWorldPos, (_N));

#define FBZZ_PUNCTUAL_END                                                        \
        }                                                                        \
        if (clusterDebugMode != 0)                                                \
            return float4(FBZZ_ClusterDebugColor(_fbzzSvXY, _fbzzWorldPos), 1.0f); \
    }

// クラスタ占有のヒートマップ色。clusterDebugMode != 0 のとき共通イテレータが最終色へ差し替える。
// 緑 (空いている) → 黄 → 赤 (上限で切り捨てが起きている)。
float3 FBZZ_ClusterDebugColor(float2 svXY, float3 worldPos)
{
    uint count, base;
    FBZZ_PunctualRange(svXY, worldPos, count, base);
    const float t = saturate((float)count / (float)FBZZ_MAX_LIGHTS_PER_CLUSTER);
    return lerp(float3(0.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), t)
         + float3(0.0f, 0.0f, count >= FBZZ_MAX_LIGHTS_PER_CLUSTER ? 1.0f : 0.0f);
}

#endif // CLUSTERED_LIGHTS_HLSLI
