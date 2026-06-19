// FBZZ Engine
// Detail/DetailGrass.hlsl | VS + PS
// Terrain Detail System — Grass レイヤー専用プロシージャルブレードシェーダー
//
// 頂点バッファなし。VS が SV_VertexID / SV_InstanceID から
// ブレードの形状を手続き的に生成する。
//
//   DrawCall: vertexCount = bladeSegments * 6  (三角形リスト、セグメントあたり 2 三角形)
//             instanceCount = 草インスタンス数
//             instanceBuffer = StructuredBuffer<GrassInstance> (VS t0)
//
// インスタンスバッファ (VS t0 = DrawCall.instanceBuffer):
//   struct GrassInstance { float3 pos; float rotY; float scale; float windPhase; };
//
// 定数バッファ:
//   b0 = CameraConstants
//   b2 = DetailGrassCB (風・ブレードパラメータ)
//
// ライティング:
//   abs(dot(N, L)) を用いた簡易両面ライティング + 環境光
//   WHY: 草は裏面も同じ明るさで描画する必要がある。

#include "Common/Binding.hlsli"
#include "Platform/DX11.hlsli"

// ============================================================
// 定数バッファ
// ============================================================

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float3   _camPad;
};

cbuffer LightConstants : register(CB_LIGHT)
{
    float3 lightDir;
    float  _lPad0;
    float3 lightColor;
    float  lightIntensity;
    // 残りフィールドは使わない
};

// b2 = DetailGrassCB
#define FBZZ_MATERIAL_CONSTANTS
cbuffer DetailGrassCB : register(CB_MATERIAL)
{
    float3 windDir;       // 正規化 XZ 風向きベクトル (Y=0)
    float  gTime;         // 累積時間 (sin に渡す)
    float  windStrength;  // グローバル風速
    float  windFrequency; // sin 周波数
    float  bladeHeight;   // ブレード高さ [m]
    float  bladeWidth;    // ブレード根元幅 [m]
    int    bladeSegments; // 分割数
    float  alphaCutoff;   // アルファテスト閾値
    int    hasAlbedoTex;  // 1 = アルベドテクスチャあり
    float  _gPad;
};

// ============================================================
// インスタンスバッファ (VS t0 = DrawCall.instanceBuffer)
// ============================================================

struct GrassInstance
{
    float3 pos;
    float  rotY;
    float  scale;
    float  windPhase;
};
StructuredBuffer<GrassInstance> g_Instances : register(t0);

// ============================================================
// テクスチャ
// ============================================================

Texture2D    gAlbedo  : register(TEX_ALBEDO);
SamplerState gSampler : register(SAMPLER_DEFAULT);

// ============================================================
// 入出力構造体
// ============================================================

struct PsIn
{
    float4 svPos   : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float  bladetT : TEXCOORD1; // [0=根元, 1=先端]
    float3 normal  : NORMAL;
};

// ============================================================
// ブレード頂点レイアウト (セグメントあたり 6 頂点)
// ============================================================

// 三角形リスト: v0(BL), v1(BR), v2(TL), v3(BR), v4(TR), v5(TL)
static const float kSideU[6] = { -0.5f,  0.5f, -0.5f,  0.5f,  0.5f, -0.5f };
static const float kIsTop[6] = {  0.0f,  0.0f,  1.0f,  0.0f,  1.0f,  1.0f };

// ============================================================
// VS
// ============================================================

PsIn VSMain(uint vertId : SV_VertexID, uint instId : SV_InstanceID)
{
    GrassInstance inst = g_Instances[instId];

    int   segIdx  = (int)(vertId / 6);
    int   localV  = (int)(vertId % 6);
    float sideU   = kSideU[localV];  // -0.5 (左) or +0.5 (右)
    float isTop   = kIsTop[localV];  // 0=下端 1=上端

    // セグメント内での正規化高さ [0, 1] (blade 全体)
    float t = (segIdx + isTop) / (float)bladeSegments;

    // カメラ向き Cylindrical Billboard の右方向。
    // WHY: SceneView は EditorCamera、GameView/PlayMode は CameraComponent で RenderSystem が
    //      b0 を更新するため、shader 側は cameraPos を参照すればビューごとの正面向きに追従できる。
    float3 viewDir = cameraPos - inst.pos;
    viewDir.y = 0.0f;
    float viewLenSq = dot(viewDir, viewDir);
    viewDir = (viewLenSq > 1e-6f)
        ? viewDir * rsqrt(viewLenSq)
        : float3(0.0f, 0.0f, 1.0f);
    float3 bladeRight   = normalize(cross(float3(0.0f, 1.0f, 0.0f), viewDir));
    float3 bladeForward = normalize(cross(bladeRight, float3(0.0f, 1.0f, 0.0f)));

    // 根元から先端に向かって幅を細くする (先端は 20% の幅)
    float width = bladeWidth * inst.scale * (1.0f - t * 0.8f);

    // 風の変位: sin 波で揺れ、高さの二乗で根元ほど動かない
    float windAmount = sin(gTime * windFrequency + inst.windPhase)
                     * windStrength * inst.scale * t * t;
    float3 windOffset = float3(windDir.x, 0.0f, windDir.z) * windAmount;

    // ワールド座標
    float3 basePos  = inst.pos;
    float3 worldPos = basePos
                    + bladeRight * sideU * width
                    + float3(0.0f, bladeHeight * inst.scale * t, 0.0f)
                    + windOffset;

    // ブレード表面法線: カメラ方向を基準にし、先端ほど少し上向きへ寄せる。
    float3 normal       = normalize(lerp(bladeForward,
                                         float3(0.0f, 1.0f, 0.0f),
                                         t * 0.5f));  // 先端は上向きへ

    // UV: u=左右[0,1], v=根元→先端[1,0] (テクスチャ上部が草の先端)
    float2 uv = float2(sideU + 0.5f, 1.0f - t);

    PsIn o;
    o.svPos   = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv      = uv;
    o.bladetT = t;
    o.normal  = normal;
    return o;
}

// ============================================================
// PS
// ============================================================

float4 PSMain(PsIn p) : SV_Target0
{
    float4 col;

    if (hasAlbedoTex)
    {
        col = gAlbedo.Sample(gSampler, p.uv);
    }
    else
    {
        // プロシージャルグリーングラデーション
        float3 baseColor = float3(0.10f, 0.30f, 0.04f); // 暗い緑 (根元)
        float3 tipColor  = float3(0.30f, 0.52f, 0.08f); // 明るい黄緑 (先端)
        col = float4(lerp(baseColor, tipColor, p.bladetT), 1.0f);
    }

    // アルファテスト
    if (alphaCutoff > 0.0f)
        clip(col.a - alphaCutoff);

    // 簡易両面ライティング: abs(dot(N, L)) + 環境光
    float3 N = normalize(p.normal);
    float  NdotL    = abs(dot(N, normalize(-lightDir)));
    float  ambient  = 0.35f;
    float  diffuse  = NdotL * lightIntensity * 0.65f;
    col.rgb *= (ambient + diffuse) * lightColor;

    return col;
}
