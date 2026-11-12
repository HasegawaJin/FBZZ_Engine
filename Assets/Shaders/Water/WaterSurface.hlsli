/// @file    WaterSurface.hlsli
/// @brief   Gerstner 波・さざ波タイル・深度吸収・屈折・空反射・白波を合成する水面シェーダー。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note 水面は半透明でシーン深度と HDR カラーを読むため、通常マテリアルとは別のシェーダーに閉じる。
/// @note 法線マップを持たないのは、水面 1 枚ごとにタイリングとスクロール速度を詰め直す必要が
/// @note 生じ、大きさの違う水面を並べた瞬間にさざ波の粒度が揃わなくなるため。さざ波はエンジンが
/// @note 起動時に焼くタイラブル勾配タイルをワールド座標で引き、反射は空連動 IBL から取る。
/// @note オーサリング資産 0 個で、どの大きさの水面でも同じ細かさになる。
/// @see Docs/design/water-waves.md
#ifndef FBZZ_WATER_SURFACE_HLSLI
#define FBZZ_WATER_SURFACE_HLSLI
#include "Common/Binding.hlsli"
#include "Water/SurfacePatchMorph.hlsli"
#ifndef FBZZ_WATER_MEDIUM_CAPTURE
#include "Rendering/WaterMedium.hlsli"
#endif

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 4
/// @note Water SSR は PS 内で走るため、全画面 Compute SSR より低い上限にして面積負荷を抑える。
#define WATER_SSR_MAX_STEPS 16

/// @note 水面 1 枚が受け取る流れの枠数。C++ の kWaterSurfaceFlowCount と揃えること。
#define WATER_SURFACE_FLOW_COUNT 8
/// @note 流れの型。FlowFieldType と ParticleGpuSim.cs.hlsl の FF_* と同じ整数であること。
#define WFF_UNIFORM 0
#define WFF_SINK    1
#define WFF_SOURCE  2
#define WFF_VORTEX  3
#define WFF_CURL    4
#define WFF_BAKED   6
/// @note 速度場アトラスの 1 タイルの 1 辺。VectorFieldAsset.hpp の
/// @note kVelocityFieldTileResolution と一致させること。
#define WATER_VELOCITY_FIELD_TILE 32

struct PointLightData
{
    float3 position;
    float  range;
    float3 color;
    float  intensity;
};

struct SpotLightData
{
    float3 position;
    float  range;
    float3 direction;
    float  innerCos;
    float3 color;
    float  outerCos;
    float  intensity;
    float3 _pad;
};

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float    waterSsrEnabled;
    float    isOrthographic;
    float    waterWireframeMode; ///< @brief 0=通常、1=Wireframe Lit、2=Wireframe Unlit
};

/// @note Water は半透明 Forward 描画で GBuffer に法線を書かないため、通常の SSR Compute の
/// @note 反射元にはなれない。共通設定だけを受け取り、水面 PS 内でコピー済み深度を追跡する。
/// @warning 手書きの部分コピーは禁止。以前は先頭 8 フィールドだけを写しており、末尾へ足した
/// @note 画面空間 AO / 接触影が Water からは読めなかった。
#include "Common/AdvancedGraphicsConstants.hlsli"

/// @note C++ の WaterCB と 16 byte 単位で同期する。Vector4 パックにして暗黙パディング差をなくす。
struct WaterMaterialBlendTarget
{
    float4 shallowColorDepth;
    float4 deepColorDepth;
    float4 surface;
    float4 normalReflectionRefraction;
    float4 detail;
    float4 foam;
    float4 crestFoam;
    float4 foamColorRoughness;
    float4 absorptionPhase;
    float4 scatteringMode;
    float4 sss;
    float4 effects;
    float4 skyTint;
    float4 rippleColor;
};

cbuffer WaterCB : register(CB_OBJECT)
{
    float4x4 g_worldMatrix;
    float4x4 g_wvpMatrix;
    float4   g_shallowColorDepth;    ///< @brief xyz=浅瀬色, w=浅瀬深度
    float4   g_deepColorDepth;       ///< @brief xyz=深部色, w=深部深度
    float4   g_surfaceParams;        ///< @brief x=opacity, y=reflectivity, z=fresnelBias, w=fresnelPower
    float4   g_normalParams;         ///< @brief xy=水面のワールド実寸 [m], z=さざ波タイルの勾配復号係数, w=normalStrength
    float4   g_timeParams;           ///< @brief xy=頂点グリッド 1 セルの実寸 [m], z=1/タイルのセル数, w=time
    float4   g_foamParams;           ///< @brief x=threshold, y=fade, z=strength, w=foamNoiseScale
    float4   g_refractionFlowParams; ///< @brief x=refraction, y=flowSpeed, zw=外周フェード幅 (UV 単位, 0 で無効)
    float4   g_waveDir[16];          ///< @brief xy=direction, z=steepness, w=enabled
    float4   g_waveParams[16];       ///< @brief x=amplitude, y=wavelength, z=omega, w=k
    float4   g_detailParams;         ///< @brief x=detailScale, y=detailSpeed, z=detailStrength, w=smoothness
    float4   g_sssParams;            ///< @brief xyz=透過光の色, w=強度
    float4   g_reflectParams;        ///< @brief x=skyReflection(0で無効), y=波の群の深さ, z=水面基準Y, w=波高合計
    float4   g_flowParams;           ///< @brief xy=流れ方向(正規化), z=さざ波の異方比, w=うねり追従ワープ幅 [m]
    float4   g_waveShapeParams;      ///< @brief x=方向広がり [0,1], y=流れの本数, zw=ビューごとの格子中心 (ローカル XZ)
    /// @brief xy=中心のワールド XZ [m], z=影響半径 [m], w=形の変位 [m] (穴は負・山は正)
    float4   g_surfaceFlowA[WATER_SURFACE_FLOW_COUNT];
    /// @brief x=流速 [m/s] (渦だけ回る向きの符号つき), y=減衰の指数, z=型 (WFF_*), w=質感へ回す強さ [0,1]
    float4   g_surfaceFlowB[WATER_SURFACE_FLOW_COUNT];
    /// @brief xy=Uniform の XZ 向き (正規化), z=速度場アトラスのタイル番号 (-1 で無効), w=復号係数
    float4   g_surfaceFlowC[WATER_SURFACE_FLOW_COUNT];
    /// @brief Baked の逆回転クォータニオン (x, y, z, w)
    float4   g_surfaceFlowD[WATER_SURFACE_FLOW_COUNT];
    /// @brief xyz=Baked の箱の半径 [m], w=水面の基準面 Y − 場の中心 Y [m]
    float4   g_surfaceFlowE[WATER_SURFACE_FLOW_COUNT];
    float4   g_gridParams; ///< @brief x=カメラ近傍への頂点集中, y=近景セル幅 [m], zw=各軸の分割数
    float4   g_patchRect; ///< @note xy/zw=水域全体に対するパッチの UV 最小/最大
    float4   g_patchCellSize; ///< @note xy=world セル幅, z=共有パッチ有効, w=分割数
    float4   g_patchEdgeCellSize[4];
    float4   g_patchCornerCellSize[4];
    float4   g_crestFoamParams; ///< @note x/y=正規化波高の開始/幅, z=波頭泡の強度, w=履歴の減衰時間 [s]
    float4   g_foamColorRoughness; ///< @note xyz=泡の反射率, w=泡表面の粗さ
    float4   g_farOceanParams; ///< @note x=Horizon 有効, y=Horizon パッチ, z=局所波紋の減衰幅 [m], abs(w)=媒体 owner ID。正のとき除外を照会する。
    float4   g_terrainInfoParams; ///< @note x=水底情報有効, y=符号付き水深の範囲 [m], zw=情報テクスチャ寸法
    float4   g_wavePhasePacked[4];
    float4   g_waveControls; ///< @note x=有効基波数、yz=さざ波の方向上書き (0 で基流)、w=明示count。
    float4   g_absorptionCoefficients;
    float4   g_scatteringCoefficients;
    float4   g_opticalParams;
    float4   g_patchMorphView;
    float4   g_patchMorphForward;
    float4   g_patchMorphMetric;
    float4   g_patchMorphParams;
    float4   g_patchMorphCellSize;
    float4   g_waveDepthParams; ///< @note w=矩形水域の基流の符号付き速度 [m/s]。
    float4   g_horizonPatchDomain; ///< @note xy=local XZ 最小値, zw=local 寸法。0 寸法は従来の有限矩形。
    float4   g_terrainDomain; ///< @note xy=Terrain 域中心, zw=寸法。Water 軸へ投影した world m。
    float4   g_riverHorizonCaps[2]; ///< @note xy=world XZ, z=blend radius [m], w=sea level [m].
    float4   g_waterfallSeams[4]; ///< @note xy=world XZ, zw=flat/fade radii [m]. Negative first w pins the surface.
    float4   g_materialBlendMouth;
    float4   g_horizonWaveSeams[4];
    float4   g_horizonLinkedRiverMedia;
    WaterMaterialBlendTarget g_materialBlendTarget;
};

float WaterfallWaveWeight(float2 worldXZ)
{
    if (g_waterfallSeams[0].w < 0.0f) return 0.0f;
    float weight = 1.0f;
    [unroll] for (uint i = 0; i < 4; ++i) {
        const float4 seam = g_waterfallSeams[i];
        if (seam.w > 0.0f) weight = min(weight, smoothstep(seam.z, seam.w, length(worldXZ - seam.xy)));
        const float4 mouth = g_horizonWaveSeams[i];
        if (mouth.w > 0.0f) weight = min(weight, smoothstep(mouth.z, mouth.w, length(worldXZ - mouth.xy)));
    }
    return weight;
}

int WaterWaveCount()
{
    return g_waveControls.w > 0.5f ? (int)clamp(g_waveControls.x, 0.0f, 16.0f) : 4;
}

float WaterWavePhase(int index)
{
    return g_wavePhasePacked[index >> 2][index & 3];
}

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
/// @note 波紋テクスチャの B が表せる高さの範囲 [m] (±)。
/// @warning C++ の kWaterRippleHeightScale と同じ値であること。片方だけ変えると、
/// @note 見えている輪と浮力が別の高さになる。
static const float kRippleHeightScale = 0.5f;

cbuffer LightConstants : register(CB_LIGHT)
{
    float3         lightDir;       float _lightPad;
    float3         lightColor;     float lightIntensity;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];
    int            pointLightCount;
    int            spotLightCount;
    /// @note Common/Constants.hlsli と同じ並び。IBL が無いときの空反射の明るさに使う。
    float          skyDimmer;
    float          _lightPad2;
    float3         ambientColor;
    float          _ambientPad;
};

/// @note ShadowConstants (b4) はカスケード配列を含むためレイアウトを 1 か所で定義する。
#include "Common/ShadowConstants.hlsli"

#include "Rendering/Shadow.hlsli"
#include "Rendering/Lighting.hlsli"
#include "WaterOptics.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note WaterRenderPass.cppの192 byte layout。patchCellSize.z=2の束だけVS slot0を読む。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-input-assembler-stage-using System-generated InstanceID
struct WaterPatchInstance
{
    float4 rect;
    float4 cellSize;
    float4 edgeCellSize[4];
    float4 cornerCellSize[4];
    float4 morphParams;
    float4 morphCellSize;
};
FBZZ_VS_SBUFFER(WaterPatchInstance, g_waterPatchInstances, 0);

WaterPatchInstance ResolveWaterPatchInstance(uint instanceId)
{
    if (g_patchCellSize.z > 1.5f) return g_waterPatchInstances[instanceId];
    WaterPatchInstance patch;
    patch.rect = g_patchRect;
    patch.cellSize = g_patchCellSize;
    patch.morphParams = g_patchMorphParams;
    patch.morphCellSize = g_patchMorphCellSize;
    [unroll] for (uint edge = 0; edge < 4; ++edge) {
        patch.edgeCellSize[edge] = g_patchEdgeCellSize[edge];
        patch.cornerCellSize[edge] = g_patchCornerCellSize[edge];
    }
    return patch;
}

/// @note 水面が読むテクスチャは «エンジンが生成するもの» だけ。オーサリング資産は要らない。
FBZZ_TEX2D(g_foamMask, 3); ///< @brief 岸沿いの泡マスク (地形高さから CPU 生成)
FBZZ_TEX2D(g_detailNoise, 4); ///< @brief さざ波タイル (起動時に CPU で焼く。RG=勾配, B=高さ)
FBZZ_TEX2D(g_sceneDepth, 5); ///< @brief Water 描画直前の深度コピー
FBZZ_TEX2D(g_sceneColor, 6); ///< @brief Water 描画直前の HDR コピー
FBZZ_TEX2D(g_rippleTex, 8); ///< @brief 着水波紋 (CPU 生成)
FBZZ_TEX2D_T(float, g_shadowMap, 9);
FBZZ_TEXCUBE(g_skyReflection, TEX_IBL_PREFILTER_SLOT); ///< @brief 空連動 IBL の事前フィルタ済みキューブ
/// @note 常駐中の 速度場 PNG を積んだアトラス。場が 1 枚も無くても 1x1x1 が必ず束縛される。
FBZZ_TEX3D_T(float4, g_velocityAtlas, TEX_VELOCITY_FIELD_SLOT);

/// @note サンプラーのレジスタ割り当ては Binding.hlsli の SAMPLER_* に従う。
/// @warning DX12 は静的サンプラーを Root Signature へ焼き込むため、レジスタごとの意味は全シェーダーで
/// @note 一致していなければならない。Water だけ独自番号を使うと比較サンプラーの位置がずれて影が壊れる。
SamplerState g_sampler      : register(SAMPLER_DEFAULT);
SamplerState g_samplerClamp : register(SAMPLER_LINEAR_CLAMP); ///< @brief 環境反射のサンプルも兼ねる
/// @note さざ波タイルは «寝た視線で引き伸ばされる» のが常態なので異方フィルタを使う。
SamplerState g_samplerTile  : register(SAMPLER_WRAP_ANISO4);
SamplerComparisonState g_shadowSampler : register(SAMPLER_SHADOW);

struct WaterVSInput
{
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
    float4 splineData : TEXCOORD1; ///< @brief x=水深 [m], yz=ワールド XZ 流速 [m/s], w=スプライン頂点 flag
    float4 zoneMaterial : TEXCOORD2;
};

struct WaterPSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float2 uv         : TEXCOORD1;
    float3 normal     : TEXCOORD2;
    float3 tangent    : TEXCOORD3;
    float3 binormal   : TEXCOORD4;
    float4 screenPos  : TEXCOORD5;
    /// @brief x=正規化波高、y=白波の合成被覆率、z=Horizon の移流した残り泡、w=現在の波頭泡。
    float4 waveState  : TEXCOORD6;
    /// @brief xy = 流向、z = さざ波強度、w = 泡のムラ。頂点で評価して補間する。
    float4 flowDirectionDetail : TEXCOORD7;
    float2 flowFoam : TEXCOORD8;
    float2 meshCellSize : TEXCOORD9; ///< @brief この頂点の隣接セル幅 [m]。遠景で落とした変位を法線へ渡す。
    float2 baseWorldXZ : TEXCOORD10; ///< @brief 波変位前のワールド XZ。遠景の法線も頂点波と同じ位相で評価する。
    float4 splineSurface : TEXCOORD11; ///< @brief 水深と authored world flow。w が 1 のとき spline footprint。
    float baseWorldY : TEXCOORD12; ///< @brief 波を乗せる前の基準高さ。地形深度を同じ面から測る。
    float relativeSurfaceY : TEXCOORD13; ///< @brief WaterCB の world origin からの高さ。大きな座標で小さな波を失わない。
    float waveJacobian : TEXCOORD14; ///< @brief Gerstner 水平変位の実ヤコビアン。媒体と集光で共有する。
    float4 waveMeshFade[4] : TEXCOORD15; ///< @brief 頂点で保持した各基波の振幅比。残差と相補的に補間する。
    float4 shoreWaveHeights : TEXCOORD19; ///< @brief xyz=岸泡履歴の符号付き波高、w=現在の波高 [m]。
    nointerpolation uint2 zoneMaterialIndices : TEXCOORD20;
    float3 zoneReceiverWeights : TEXCOORD21;
    float3 zoneSourceWeights : TEXCOORD22;
};

/// @brief Give the connected river raster priority when both surfaces meet at the same elevation.
float WaterHorizonDepthTieOffset(float2 worldXZ)
{
    if (!any(g_horizonLinkedRiverMedia > 0.0f)) return 0.0f;
    float weight = 0.0f;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        const float4 mouth = g_horizonWaveSeams[i];
        if (g_horizonLinkedRiverMedia[i] > 0.0f)
            weight = max(weight, 1.0f - smoothstep(mouth.z, mouth.w, length(worldXZ - mouth.xy)));
    }
    return 0.03f * weight;
}

/// @brief 有限矩形の外周を保ったまま、軸の中央へ近景の頂点を集める。
/// @note Graphics/Effects/WaterGrid.hpp と同じ式。中心はセル単位で揃え、カメラ移動で近景の格子を滑らせない。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-18-using-vertex-texture-displacement GPU Gems 2 Chapter 18, camera-centered grid
float WaterGridAxisPosition(float coordinate, float extent, float resolution, float focus, float nearCellSize)
{
    float halfExtent = extent * 0.5f;
    if (coordinate <= 0.0f) return -halfExtent;
    if (coordinate >= 1.0f) return halfExtent;
    float q = coordinate * 2.0f - 1.0f;
    if (extent / resolution <= nearCellSize)
        return q * halfExtent;

    float nearHalfExtent = min(nearCellSize * resolution * 0.5f, halfExtent);
    float focusLimit = halfExtent - nearHalfExtent;
    focus = clamp(focus, -focusLimit, focusLimit);
    focus = floor(focus / nearCellSize + 0.5f) * nearCellSize;
    focus = clamp(focus, -focusLimit, focusLimit);
    float sign = q < 0.0f ? -1.0f : 1.0f;
    float t = abs(q);
    float side = halfExtent - sign * focus;
    float far = max(2.0f * t - 1.0f, 0.0f);
    return focus + sign * (nearHalfExtent * t + (side - nearHalfExtent) * far * far * far);
}

/// @brief 位置と両隣の最大セル幅を返す。境界も同じ写像を使うのでチャンク間に裂け目を作らない。
float2 ResolveWaterGridAxis(float coordinate, float extent, float resolution, float focus, float nearCellSize)
{
    float step = 1.0f / resolution;
    float position = WaterGridAxisPosition(coordinate, extent, resolution, focus, nearCellSize);
    float previous = WaterGridAxisPosition(max(coordinate - step, 0.0f), extent, resolution, focus, nearCellSize);
    float next = WaterGridAxisPosition(min(coordinate + step, 1.0f), extent, resolution, focus, nearCellSize);
    return float2(position, max(position - previous, next - position));
}

/// @brief 頂点グリッドで «刻めない» 波を寝かせる係数 [0,1]。C++ 側の WaveMeshFade と同じ式。
/// @param cellSize 頂点グリッド 1 セルのワールド実寸 [m]。0 のときはフェードしない。
/// @note Gerstner 波は頂点でしか評価されないので、1 波長あたり数セルしか取れない波は山と谷が
/// @note セル境界で入れ替わり «もっと長い別の波» に化ける。下限 2.0 は Nyquist、上限 3.5 は
/// @note «まだ波として読める» 側で、既存の水面から表現できている波を奪わない値。
/// @note 寝かせた波は消えるのではなく WaterResidualWaveSlope が法線だけ拾い直す。
float WaveMeshFade(float wavelength, float2 cellSize)
{
    float cell = max(cellSize.x, cellSize.y);
    if (cell <= 0.0f) return 1.0f;
    return smoothstep(2.0f, 3.5f, wavelength / cell);
}

float2 WaterTerrainUvAtWorldXZ(float2 worldXZ);
float2 SampleWaterTerrainDepth(float2 uv);

float2 WaterTerrainExtent()
{
    return all(g_terrainDomain.zw > 0.0f) ? g_terrainDomain.zw : g_normalParams.xy;
}

/// @note CPU WaterDepthField と同じflags/GB16bit/texel中心を使い、係数を復号してから補間する。
/// @see https://pubs.usgs.gov/publication/70030520 Stockdon et al. (2006), wave runup and swash elevation.
float2 WaterDepthTexelFactors(float4 texel, float landFadeDepth)
{
    uint flags = (uint)round(texel.a * 255.0f);
    if ((flags & 1u) == 0u) return 1.0f;
    const bool dry = (flags & 2u) != 0u;
    if (dry && landFadeDepth <= 0.0f) return 0.0f;
    uint word = (uint)round(texel.g * 255.0f) * 256u + (uint)round(texel.b * 255.0f);
    float depth = ((float)word / 65535.0f * 2.0f - 1.0f) * g_terrainInfoParams.y;
    if ((flags & 4u) != 0u) return !dry && depth > 0.0f ? 1.0f : 0.0f;
    if (dry) depth = min(depth, 0.0f);
    const float horizontal = smoothstep(0.0f, g_waveDepthParams.x, depth);
    if (landFadeDepth <= 0.0f) return horizontal;
    const float shoreHeightFactor = 0.65f;
    const float vertical = depth < 0.0f
        ? shoreHeightFactor * smoothstep(-landFadeDepth, 0.0f, depth)
        : lerp(shoreHeightFactor, 1.0f, horizontal);
    return float2(horizontal, vertical);
}

struct WaterWaveDepthState
{
    float3 horizontal;
    float3 vertical;
};

/// @return 各軸の x は振幅係数、yz は world XZ での解析勾配。
WaterWaveDepthState WaterDepthState(float2 baseWorldXZ)
{
    WaterWaveDepthState state = {float3(1.0f, 0.0f, 0.0f), float3(1.0f, 0.0f, 0.0f)};
    if (g_waveDepthParams.x <= 0.0f || g_waveDepthParams.y < 0.5f || g_terrainInfoParams.x < 0.5f)
        return state;
    float2 uv = WaterTerrainUvAtWorldXZ(baseWorldXZ);
    float2 clampedUv = saturate(uv);
    float2 terrainExtent = WaterTerrainExtent();
    float2 outside = (uv - clampedUv) * terrainExtent;
    float outsideDistance = length(outside);
    float outsideWidth = max(g_waveDepthParams.z, 0.001f);
    if (outsideDistance >= outsideWidth) return state;
    int2 dimensions = max((int2)g_terrainInfoParams.zw, int2(1, 1));
    float2 pixel = clampedUv * (float2)dimensions - 0.5f;
    int2 basePixel = (int2)floor(pixel);
    float2 blend = frac(pixel);
    const float landFadeDepth = all(g_terrainDomain.zw > 0.0f)
        ? min(g_terrainInfoParams.y, clamp(0.35f * g_waveDepthParams.x, 0.25f, 2.0f)) : 0.0f;
    float2 factors[4];
    [unroll]
    for (int z = 0; z < 2; ++z) {
        [unroll]
        for (int x = 0; x < 2; ++x) {
            int2 texel = clamp(basePixel + int2(x, z), int2(0, 0), dimensions - 1);
            factors[z * 2 + x] = WaterDepthTexelFactors(g_foamMask.Load(int3(texel, 0)), landFadeDepth);
        }
    }
    float2 lower = lerp(factors[0], factors[1], blend.x);
    float2 upper = lerp(factors[2], factors[3], blend.x);
    float2 du = lerp(factors[1] - factors[0], factors[3] - factors[2], blend.y) * dimensions.x;
    float2 dv = lerp(factors[2] - factors[0], factors[3] - factors[1], blend.x) * dimensions.y;
    if (outside.x != 0.0f) du = 0.0f;
    if (outside.y != 0.0f) dv = 0.0f;
    float2 axisU = normalize(mul(float4(1.0f, 0.0f, 0.0f, 0.0f), g_worldMatrix).xz);
    float2 axisV = normalize(mul(float4(0.0f, 0.0f, 1.0f, 0.0f), g_worldMatrix).xz);
    float2 gradientX = du * (axisU.x / terrainExtent.x) + dv * (axisV.x / terrainExtent.y);
    float2 gradientZ = du * (axisU.y / terrainExtent.x) + dv * (axisV.y / terrainExtent.y);
    float2 factor = saturate(lerp(lower, upper, blend.y));
    if (outsideDistance > 0.0f) {
        float t = saturate(outsideDistance / outsideWidth);
        float blendOut = t * t * (3.0f - 2.0f * t);
        const float2 distanceGradient = (axisU * outside.x + axisV * outside.y) / outsideDistance;
        const float2 fadeSlope = (1.0f - factor) * (6.0f * t * (1.0f - t) / outsideWidth);
        gradientX = (1.0f - blendOut) * gradientX + fadeSlope * distanceGradient.x;
        gradientZ = (1.0f - blendOut) * gradientZ + fadeSlope * distanceGradient.y;
        factor = lerp(factor, 1.0f, blendOut);
    }
    state.horizontal = float3(factor.x, gradientX.x, gradientZ.x);
    state.vertical = float3(factor.y, gradientX.y, gradientZ.y);
    return state;
}

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
/// @note **向きだけを曲げ、さざ波を «運ぶ» 距離は一様なまま。** 速度場で座標を積分すると、
/// @note 流速が場所で変わる渦ではさざ波が輪へ引き伸ばされる (直すには履歴バッファが要る)。
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

/// @note SurfacePatchLod::ResolveSurfacePatchCellSize と同じ辺・角の順で共有頂点の帯域を決める。
float2 ResolveWaterPatchCellSize(float2 unitUv, WaterPatchInstance patch)
{
    bool minX = unitUv.x <= 0.0f, maxX = unitUv.x >= 1.0f;
    bool minZ = unitUv.y <= 0.0f, maxZ = unitUv.y >= 1.0f;
    if ((minX || maxX) && (minZ || maxZ))
        return patch.cornerCellSize[(maxX ? 1 : 0) + (maxZ ? 2 : 0)].xy;
    if (minX) return patch.edgeCellSize[0].xy;
    if (maxX) return patch.edgeCellSize[1].xy;
    if (minZ) return patch.edgeCellSize[2].xy;
    if (maxZ) return patch.edgeCellSize[3].xy;
    return patch.cellSize.xy;
}

/// @brief 有限水域の外に出た局所波紋を world 距離で減衰させる。
/// @note 接続頂点は近景と同じ値を保ち、clamp sampler の端の波紋を遠景全体へ引き伸ばさない。
float WaterLocalDataWeight(float2 uv)
{
    if (g_farOceanParams.x < 0.5f) return 1.0f;
    float2 outside = max(max(-uv, uv - 1.0f), 0.0f) * g_normalParams.xy;
    float distance = length(outside);
    if (distance <= 0.0f) return 1.0f;
    return 1.0f - smoothstep(0.0f, max(g_farOceanParams.z, 0.001f), distance);
}

/// @note The ripple bake truncates zero to 127 and untouched texels use 128. Both codes represent still water.
float3 DecodeWaterRipple(float3 encoded)
{
    const float3 bytes = encoded * 255.0f;
    return (max(bytes - 128.0f, 0.0f) - max(127.0f - bytes, 0.0f)) / 127.0f;
}

/// @brief Indexed VS と Mesh Shader が共有する水面頂点評価。
/// @note Mesh Shader は同じパッチ instance index を渡し、既存の波・morph・flow・TAA 入力を維持する。
WaterPSInput EvaluateWaterVertex(WaterVSInput v, uint instanceId)
{
    WaterPatchInstance patch = ResolveWaterPatchInstance(instanceId);
    WaterPSInput o;
    float time = g_timeParams.w;
    float3 localPos = v.position;
    float2 uv = v.uv;
    float2 meshCellSize = g_timeParams.xy;
    bool splineSurface = v.splineData.w > 0.5f;
    [branch]
    if (!splineSurface && patch.cellSize.z > 0.5f)
    {
        float3 axisX = mul(float4(1.0f, 0.0f, 0.0f, 0.0f), g_worldMatrix).xyz;
        float3 axisZ = mul(float4(0.0f, 0.0f, 1.0f, 0.0f), g_worldMatrix).xyz;
        float2 axisScale = max(float2(length(axisX), length(axisZ)), 1.0e-6f);
        float2 authoredExtent = g_normalParams.xy / axisScale;
        bool horizonDomain = all(g_horizonPatchDomain.zw > 0.0f);
        float2 extent = horizonDomain ? g_horizonPatchDomain.zw : authoredExtent;
        float2 minimum = horizonDomain ? g_horizonPatchDomain.xy : -0.5f * authoredExtent;
        /// @note 二進分割 UV から同じ全域 extent を一度だけ掛け、隣接 LOD の共有位置を揃える。
        uv = lerp(patch.rect.xy, patch.rect.zw, v.uv);
        localPos.xz = minimum + uv * extent;
        meshCellSize = ResolveWaterPatchCellSize(v.uv, patch);
        if (patch.morphParams.x > 0.5f) {
            float3 worldPos = mul(float4(localPos, 1.0f), g_worldMatrix).xyz;
            SurfacePatchMorphSample morph = SurfacePatchMorphUv(uv, v.uv, worldPos,
                axisX * extent.x, axisZ * extent.y, patch.rect, patch.cellSize.w, patch.morphParams,
                g_patchMorphView, g_patchMorphForward, g_patchMorphMetric, patch.morphCellSize.xy, meshCellSize);
            uv = morph.uv;
            meshCellSize = morph.cellSize;
            localPos.xz = minimum + uv * extent;
        }
        /// @note 水面格子だけがカメラを追う。波紋とプロジェクト材質の UV は保存済み矩形に固定する。
        if (horizonDomain) uv = localPos.xz / authoredExtent + 0.5f;
    }
    else if (!splineSurface && g_gridParams.x > 0.5f)
    {
        float3 axisX = mul(float4(1.0f, 0.0f, 0.0f, 0.0f), g_worldMatrix).xyz;
        float3 axisZ = mul(float4(0.0f, 0.0f, 1.0f, 0.0f), g_worldMatrix).xyz;
        float2 axisScale = max(float2(length(axisX), length(axisZ)), 1.0e-6f);
        float2 extent = g_normalParams.xy / axisScale;
        float2 focus = g_waveShapeParams.zw;
        float2 nearCellSize = g_gridParams.yy / axisScale;
        float2 gridX = ResolveWaterGridAxis(v.uv.x, extent.x, g_gridParams.z, focus.x, nearCellSize.x);
        float2 gridZ = ResolveWaterGridAxis(v.uv.y, extent.y, g_gridParams.w, focus.y, nearCellSize.y);
        localPos.xz = float2(gridX.x, gridZ.x);
        meshCellSize = float2(gridX.y, gridZ.y) * axisScale;
        /// @note マスクと波紋は有限矩形に固定する。集中前の UV を渡すと岸の泡がカメラへ追従する。
        uv = saturate(localPos.xz / extent + 0.5f);
    }
    float3 worldPos = mul(float4(localPos, 1.0f), g_worldMatrix).xyz;
    float relativeSurfaceY = mul(float4(localPos, 0.0f), g_worldMatrix).y;
    float3 tangent = float3(1.0f, 0.0f, 0.0f);
    float3 binormal = float3(0.0f, 0.0f, 1.0f);
    float3 disp = float3(0.0f, 0.0f, 0.0f);

    /// @note 変位前の位置は «水の粒» の識別子。泡の航跡を引くのに使うので取っておく。
    float3 basePos = worldPos;
    o.baseWorldXZ = basePos.xz;
    o.baseWorldY = basePos.y;
    o.splineSurface = v.splineData;
    o.zoneMaterialIndices = (uint2)v.zoneMaterial.xy;
    const float3 cornerWeight = float3(v.zoneMaterial.z == 1.0f,
        v.zoneMaterial.z == 2.0f, v.zoneMaterial.z == 3.0f);
    o.zoneReceiverWeights = cornerWeight * (1.0f - v.zoneMaterial.w);
    o.zoneSourceWeights = cornerWeight * v.zoneMaterial.w;
    const float seamWeight = WaterfallWaveWeight(basePos.xz);

    float2 rotation, scales;
    ResolveWaveSpread(rotation, scales);

    WaterWaveDepthState depthState = WaterDepthState(basePos.xz);
    const bool crossingEnabled = !splineSurface && g_farOceanParams.x > 0.5f;
    WaterCrossingCrest crossing = (WaterCrossingCrest)0;
    [unroll]
    for (int band = 0; band < 4; ++band) o.waveMeshFade[band] = 0.0f;
    [loop]
    for (int i = 0; i < WaterWaveCount(); ++i)
    {
        float fade = WaveMeshFade(g_waveParams[i].y, meshCellSize);
        o.waveMeshFade[i >> 2][i & 3] = fade;
        [unroll]
        for (int part = 0; part < 2; ++part)
        {
            float2 D = g_waveDir[i].w > 0.0f ? normalize(g_waveDir[i].xy) : float2(1.0f, 0.0f);
            float  scale;
            ApplyWaveSpread(i, part, rotation, scales, D, scale);
            if (scale <= 0.001f) continue;

            float4 dirData = float4(D, g_waveDir[i].z, g_waveDir[i].w);
            float4 params  = g_waveParams[i];
            params.x *= scale;
            disp += GerstnerDisplace(i, dirData, params, worldPos, time, fade,
                depthState, tangent, binormal, crossingEnabled, crossing);
        }
    }

    if (crossingEnabled) {
        const float3 crest = WaterCrossingCrestDisplacement(crossing, depthState.vertical);
        disp.y += crest.x;
        tangent.y += crest.y;
        binormal.y += crest.z;
    }
    disp *= seamWeight;
    tangent = lerp(float3(1.0f, 0.0f, 0.0f), tangent, seamWeight);
    binormal = lerp(float3(0.0f, 0.0f, 1.0f), binormal, seamWeight);
    worldPos += disp;
    relativeSurfaceY += disp.y;

    /// @note 流れの場が出す形。中心型は変位«後»の位置で評価する — 形は水平変位を持たないので、
    /// @note CPU の GetSurfaceHeightAt がワールド XZ をそのまま使うのと揃える。
    /// @note Baked だけは変位«前» (basePos) で引く。焼いた場は 3D なので、波を乗せた後の高さで
    /// @note 標本化すると «高さを高さで引く» 循環になる。
    int flowCount = (int)g_waveShapeParams.y;
    float vortexFoam = 0.0f;
    [loop]
    for (int fi = 0; fi < WATER_SURFACE_FLOW_COUNT; ++fi)
    {
        if (fi >= flowCount) continue;
        const int flowKind = (int)g_surfaceFlowB[fi].z;
        if (flowKind == WFF_UNIFORM || (flowKind == WFF_VORTEX && g_surfaceFlowE[fi].x > 0.0f))
        {
            const float4 swell = flowKind == WFF_VORTEX
                ? WaterVortexSwellDisplacement(fi, worldPos.xz, time, meshCellSize)
                : float4(WaterSwellDisplacement(fi, worldPos.xz, time, meshCellSize), 0.0f);
            const float height = swell.x * depthState.vertical.x * seamWeight;
            const float2 slope = swell.yz * depthState.vertical.x * seamWeight;
            worldPos.y += height;
            relativeSurfaceY += height;
            /// @note The swell is evaluated after horizontal Gerstner displacement, while depth is sampled before it.
            tangent.y += dot(slope, tangent.xz) + swell.x * depthState.vertical.y * seamWeight;
            binormal.y += dot(slope, binormal.xz) + swell.x * depthState.vertical.z * seamWeight;
            vortexFoam = max(vortexFoam, swell.w * depthState.vertical.x * seamWeight);
            continue;
        }
        if (flowKind != WFF_BAKED)
        {
            float3 pit = WaterRadialDisplacement(fi, worldPos.xz) * seamWeight;
            worldPos.y += pit.x;
            relativeSurfaceY += pit.x;
            tangent.y  += pit.y;
            binormal.y += pit.z;
            continue;
        }
        /// @note 焼いた場に解析微分は無い。1 セルぶんの前進差分で勾配を取る。
        float  cell = max(max(meshCellSize.x, meshCellSize.y), 0.05f);
        float  h  = WaterBakedDisplacement(fi, basePos.xz);
        float  hx = WaterBakedDisplacement(fi, basePos.xz + float2(cell, 0.0f));
        float  hz = WaterBakedDisplacement(fi, basePos.xz + float2(0.0f, cell));
        worldPos.y += h * seamWeight;
        relativeSurfaceY += h * seamWeight;
        tangent.y  += (hx - h) / cell * seamWeight;
        binormal.y += (hz - h) / cell * seamWeight;
    }

    /// @note 着水の輪のうち «頂点で刻める» ぶん。CPU が B チャンネルへ meshFade 込みで焼いて
    /// @note いるので、ここは読んで足すだけ (water-waves.md の «波紋の帯分け»)。
    /// @note VS からのテクスチャ読みは SampleLevel。UV は波を乗せる前の頂点 UV で、CPU 側の
    /// @note GetSurfaceHeightAt も変位前の XZ で輪を引く。
    float rippleHeight = DecodeWaterRipple(g_rippleTex.SampleLevel(g_samplerClamp, uv, 0).rgb).b
                       * kRippleHeightScale * WaterLocalDataWeight(uv) * seamWeight;
    worldPos.y += rippleHeight;
    relativeSurfaceY += rippleHeight;

    /// @note 圧縮は正規化前の水平ヤコビアン、高さは Gerstner 変位そのものから取る。
    float jacobian = tangent.x * binormal.z - tangent.z * binormal.x;
    o.relativeSurfaceY = relativeSurfaceY;
    o.waveJacobian = jacobian;
    float fold = saturate(1.0f - jacobian);
    float crestHeight = saturate(disp.y / max(g_reflectParams.w, 0.01f));
    const bool crestFoamEnabled = g_crestFoamParams.z > 0.001f;
    const float currentCrestFoam = crestFoamEnabled ? WaterCrestFoamAmount(float2(crestHeight, fold)) : 0.0f;
    float crestFoam = currentCrestFoam;
    float crestTrail = 0.0f;
    WaterFlowSample flowSample = SampleWaterSurfaceFlow(worldPos.xz, basePos.xz);
    const float lifetime = g_crestFoamParams.w;
    bool shoreHistory = crossingEnabled && g_terrainInfoParams.x > 0.5f
        && g_foamParams.x > 0.0f && g_foamParams.z > 0.001f;
    o.shoreWaveHeights = disp.y;
    [branch]
    if (lifetime > 0.001f && (crestFoamEnabled || shoreHistory)) {
        if (shoreHistory) {
            /// @note The fixed shore column needs its own history only where past water can reach the foam band.
            const float2 terrain = SampleWaterTerrainDepth(WaterTerrainUvAtWorldXZ(worldPos.xz));
            const float maxRise = WaterMaximumWaveRise();
            shoreHistory = terrain.x > 0.0f && terrain.y >= -maxRise
                && terrain.y <= g_foamParams.x + max(g_foamParams.y, 0.0f) + maxRise;
        }
        if (crestFoamEnabled || shoreHistory) {
            const float2 drift = crossingEnabled && crestFoamEnabled ? WaterFoamDriftVelocity(flowSample) : 0.0f;
            const bool movingTrail = dot(drift, drift) > 1.0e-8f;
            [unroll]
            for (int tap = 0; tap < WATER_FOAM_TRAIL_TAPS; ++tap) {
                const float ageInLifetimes = 0.5f * (float)(tap + 1);
                const float age = lifetime * ageInLifetimes;
                WaterCrestSample past = (WaterCrestSample)0;
                if (shoreHistory || !movingTrail)
                    past = WaterWaveCrestAt(basePos, time - age, meshCellSize, depthState, crossingEnabled);
                if (shoreHistory) o.shoreWaveHeights[tap] = past.displacement.y;
                if (crestFoamEnabled) {
                    if (movingTrail) {
                        const float3 source = WaterFoamBacktrace(basePos, drift, age);
                        past = WaterWaveCrestAt(source, time - age, meshCellSize, WaterDepthState(source.xz), crossingEnabled);
                    }
                    const float2 pastCrest = float2(saturate(past.displacement.y / max(g_reflectParams.w, 0.01f)), past.compression);
                    /// @see https://doi.org/10.1029/2012JC008147 Callaghan et al. (2012), decay of residual whitecap foam.
                    const float coverage = WaterCrestFoamAmount(pastCrest) * exp(-ageInLifetimes);
                    if (crossingEnabled) crestTrail = max(crestTrail, coverage);
                    else crestFoam = max(crestFoam, coverage);
                }
            }
        }
    }

    tangent = normalize(tangent);
    binormal = normalize(binormal);
    /// @note tangent = ∂P/∂x, binormal = ∂P/∂z。法線は cross(binormal, tangent) で +Y を向く。
    /// @warning 逆順 cross(tangent, binormal) は平坦な水面で (0,-1,0) を返す。以前はこれで法線が
    /// @note 真下を向き、NdotV が常に 0 → フレネル飽和・スペキュラ消失・影の反転を起こしていた。
    float3 normal = normalize(cross(binormal, tangent));

    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    /// @note Bias only raster depth. Screen UVs, world height and the medium atlas retain the physical surface.
    const float depthTieOffset = WaterHorizonDepthTieOffset(basePos.xz);
    if (depthTieOffset > 0.0f && o.svPosition.w > 0.0f) {
        const float3 forward = float3(view[0][2], view[1][2], view[2][2]);
        const float4 deeper = mul(float4(worldPos + forward * depthTieOffset, 1.0f), viewProjection);
        if (deeper.w > 0.0f) o.svPosition.z = (deeper.z / deeper.w) * o.svPosition.w;
    }
    o.worldPos = worldPos;
    o.uv = uv;
    o.meshCellSize = meshCellSize;
    o.normal = normal;
    o.tangent = tangent;
    o.binormal = binormal;
    o.screenPos = o.svPosition;
    /// @note 変位そのものから山の高さを取る。親 Transform があっても基準面がずれない。
    o.waveState = float4(crestHeight, max(crestFoam, crestTrail), crestTrail, currentCrestFoam);
    float2 authoredFlow = v.splineData.yz;
    float authoredFlowSpeed = length(authoredFlow);
    if (splineSurface && authoredFlowSpeed > 1.0e-4f)
        flowSample.direction = authoredFlow / authoredFlowSpeed;
    o.flowDirectionDetail = float4(flowSample.direction, flowSample.detailGain, flowSample.chop);
    o.flowFoam = float2(flowSample.eye, max(flowSample.ring, vortexFoam));
    return o;
}

/// @brief 既存の indexed Water 経路から共有評価を呼ぶ互換 wrapper。
/// @note SV_InstanceID は StartInstanceLocation を含まないため、候補の先頭は root constant から明示的に加算する。
/// @note The backend resets b15 to zero for direct draws and writes the candidate offset in 24-byte indexed-indirect commands.
/// @see https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/ SV_InstanceID excludes StartInstanceLocation.
cbuffer WaterGeometryDrawConstants : register(b15)
{
    uint g_waterGeometryInstanceOffset;
};

WaterPSInput VSMain(WaterVSInput v, uint instanceId : SV_InstanceID)
{
    return EvaluateWaterVertex(v, instanceId + g_waterGeometryInstanceOffset);
}

/// @brief さざ波タイルから «1 ノイズセル単位の» 勾配と高さを引く。
/// @param q       ノイズセル単位の座標。
/// @param dqdx    q の画面 x 微分。異方フィルタの LOD 選択に使う。
/// @param dqdy    q の画面 y 微分。
/// @return xy = ∂h/∂q, z = 高さ [0,1]。
/// @note 呼び出し元は帯域外の tap を分岐で省く。非一様制御流れでも mip を選べるよう画面微分を明示する。
/// @note タイルの縮小は «勾配の平均» なので、遠景では自動的に平坦へ収束し、水面が鏡へ近づく。
float3 WaterNoiseTile(float2 q, float2 dqdx, float2 dqdy)
{
    float invCells = g_timeParams.z;
    float4 t = g_detailNoise.SampleGrad(g_samplerTile, q * invCells,
                                        dqdx * invCells, dqdy * invCells);
    return float3((t.rg * 2.0f - 1.0f) * g_normalParams.z, t.b);
}

struct WaterCurrentAdvection
{
    float2 firstOffset;
    float2 secondOffset;
    float firstWeight;
    float directionWeight;
};

/// @brief Trace the two noise samples upstream along the local world XZ velocity [m/s].
/// @note Each sample fades out as its phase wraps, bounding the distance traced through a curved velocity field.
WaterCurrentAdvection WaterCurrentAt(float2 velocity, float time)
{
    const float period = 1.6f;
    const float firstPhase = frac(time / period);
    const float secondPhase = frac(firstPhase + 0.5f);
    WaterCurrentAdvection current;
    current.firstOffset = -velocity * (period * (firstPhase - 0.5f));
    current.secondOffset = -velocity * (period * (secondPhase - 0.5f));
    current.firstWeight = smoothstep(0.0f, 1.0f, 1.0f - abs(2.0f * firstPhase - 1.0f));
    current.directionWeight = smoothstep(0.0f, 0.25f, length(velocity));
    return current;
}

/// @note オクターブごとの固有ドリフト。同じ向きに揃うと縞が流れて見えるため方向をばらす。
#define WATER_DETAIL_OCTAVES 5
static const float2 kWaterDrift[WATER_DETAIL_OCTAVES] = {
    float2( 0.31f,  0.17f), float2(-0.23f,  0.41f),
    float2( 0.47f, -0.29f), float2(-0.37f, -0.13f),
    float2( 0.11f,  0.53f)
};
/// @note 振幅 0.55^i (i = 0..4) の総和。途中で打ち切っても正規化がぶれないよう定数で持つ。
static const float kWaterDetailNorm = 2.110381f;
/// @note WaterNoiseBake.hpp の kGradientRms=0.5 の二乗。落とした勾配の分散を粗さへ渡す。
static const float kWaterDetailGradientVariance = 0.25f;
/// @note Nyquist cutoff 後の振幅分散比。0.55 amplitude × 2.07 frequency の二乗。
static const float kWaterDetailVarianceRatio = (0.55f * 2.07f) * (0.55f * 2.07f);
/// @note 完全に濾過された現在 octave から末尾までの幾何級数和。
static const float kWaterDetailVarianceTail[WATER_DETAIL_OCTAVES] = {
    1.0f + kWaterDetailVarianceRatio + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio
        + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio * kWaterDetailVarianceRatio
        + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio * kWaterDetailVarianceRatio * kWaterDetailVarianceRatio,
    1.0f + kWaterDetailVarianceRatio + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio
        + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio * kWaterDetailVarianceRatio,
    1.0f + kWaterDetailVarianceRatio + kWaterDetailVarianceRatio * kWaterDetailVarianceRatio,
    1.0f + kWaterDetailVarianceRatio,
    1.0f
};

/// @brief さざ波の高さ勾配 (∂h/∂x, ∂h/∂z) をワールド XZ で積む。
/// @param worldXZ   ワールド XZ [m]。UV で評価すると extent の違う水面どうしで粒度が揃わない。
/// @param flowDir   この点の流れの向き (正規化)。渦の中では接線へ曲がる。
/// @param footprint このピクセルが覆うワールド距離 [m]。遠いほど・浅い角度ほど大きい。
/// @param dWorldDx  worldXZ の画面 x 微分。タイルの異方フィルタへ渡す。
/// @param dWorldDy  worldXZ の画面 y 微分。
/// @return xy = 勾配, z = Nyquist で落とした傾きの 1 軸平均分散 (強度を掛ける前)。
/// @note 風向に直交して座標を縮め、さざ波の «うね» を進行方向と直交させる。等方ノイズのままだと
/// @note 粒の集まりにしか見えず、水というより «ブツブツした膜» になる。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/simulating-waves-using-the-water-waves-asset-in-unreal-engine Unreal Engine Water Waves, macro mesh waves and material normal-map micro detail.
float3 WaterDetailGradient(float2 worldXZ, float2 flowDir, float2 flowTravel, float time, float footprint,
                           float2 dWorldDx, float2 dWorldDy, bool followCurrent, WaterCurrentAdvection current)
{
    float scale = max(g_detailParams.x, 1.0e-4f);
    float speed = g_detailParams.y;
    float2 flow = flowTravel;

    float  stretch = max(g_flowParams.z, 1.0f);
    float2 alongFlow = flowDir;
    float2 crossFlow = float2(-alongFlow.y, alongFlow.x);
    float2x2 aniso = float2x2(alongFlow.x, alongFlow.y, crossFlow.x / stretch, crossFlow.y / stretch);

    /// @note オクターブごとに座標を回し、格子が縞として残らないようにする。異方スケールを
    /// @note 初期値に畳んでおけば、以降は回転を掛け足すだけで両方が同時に効く。
    /// @note Rotating absolute world coordinates by a varying flow direction shears the noise at bends.
    float2x2 m = followCurrent ? float2x2(1.0f, 0.0f, 0.0f, 1.0f) : aniso;
    const float2x2 step = float2x2(0.80f, -0.60f, 0.60f, 0.80f);

    float2 grad = float2(0.0f, 0.0f);
    float amp = 1.0f, freq = scale, lostVariance = 0.0f;
    const float crossGain = lerp(1.0f, rcp(stretch), current.directionWeight);
    const float varianceAxisScale = followCurrent ? crossGain : rcp(stretch);
    const float meanAxisScaleSq = 0.5f * (1.0f + varianceAxisScale * varianceAxisScale);
    const float2 firstDx = dWorldDx + ddx(current.firstOffset);
    const float2 firstDy = dWorldDy + ddy(current.firstOffset);
    const float2 secondDx = dWorldDx + ddx(current.secondOffset);
    const float2 secondDy = dWorldDy + ddy(current.secondOffset);
    const float driftSpeed = followCurrent ? 0.0f : speed;

    [loop]
    for (int i = 0; i < WATER_DETAIL_OCTAVES; ++i)
    {
        /// @note 1 ピクセルに 1 周期以上入るオクターブは、平均すれば «ざらつき» しか残らない。
        /// @note 残すとカメラが動くたびに遠景の水面が総毛立って明滅する。周期がピクセルの 2 倍を
        /// @note 切ったところから滑らかに寝かせ、«細部が消えて鏡に近づく» 見え方へ収束させる。
        /// @note 消えた後も分散だけは積む。振幅・周波数・異方スケールを含め、弱いさざ波を過剰にぼかさない。
        float fade = 1.0f - smoothstep(0.0f, 0.5f, footprint * freq);
        float slopeScale = amp * freq / kWaterDetailNorm;
        if (fade <= 0.0f)
        {
            lostVariance += kWaterDetailGradientVariance * meanAxisScaleSq
                          * slopeScale * slopeScale * kWaterDetailVarianceTail[i];
            break;
        }
        lostVariance += kWaterDetailGradientVariance * meanAxisScaleSq
                      * slopeScale * slopeScale * (1.0f - fade * fade);
        if (fade > 0.0f)
        {
            const float2 q = (mul(m, worldXZ) + flow + kWaterDrift[i] * (time * driftSpeed)) * freq;
            float3 n;
            if (followCurrent && current.directionWeight > 0.0f) {
                const float3 first = WaterNoiseTile(q + mul(m, current.firstOffset) * freq,
                    mul(m, firstDx) * freq, mul(m, firstDy) * freq);
                const float3 second = WaterNoiseTile(q + mul(m, current.secondOffset) * freq,
                    mul(m, secondDx) * freq, mul(m, secondDy) * freq);
                n = lerp(second, first, current.firstWeight);
            } else {
                n = WaterNoiseTile(q, mul(m, dWorldDx) * freq, mul(m, dWorldDy) * freq);
            }
            /// @note 勾配は変換後の座標系で出るので、mul(g, m) = m^T g で元の軸へ戻す (連鎖律)。
            grad += mul(n.xy, m) * (amp * freq * fade);
        }
        m     = mul(step, m);
        amp  *= 0.55f;
        freq *= 2.07f;
    }

    if (followCurrent)
        grad -= crossFlow * (dot(grad, crossFlow) * (1.0f - crossGain));
    const float invNorm = 1.0f / kWaterDetailNorm;
    return float3(grad * invNorm, lostVariance);
}

float3 WaterDetailGradient(float2 worldXZ, float2 flowDir, float2 flowTravel, float time, float footprint,
                           float2 dWorldDx, float2 dWorldDy)
{
    return WaterDetailGradient(worldXZ, flowDir, flowTravel, time, footprint, dWorldDx, dWorldDy,
        false, (WaterCurrentAdvection)0);
}

/// @brief 頂点グリッドで刻めなかった Gerstner 波を «法線だけ» 戻す。
/// @param dWorldDx,dWorldDy 波変位前のワールド XZ の画面微分 [m/pixel]。
/// @param[out] lostVariance ピクセルより細かくて捨てた傾きの 1 軸平均分散。粗さへ回す。
/// @return ワールド XZ の高さ勾配 (∂h/∂x, ∂h/∂z)。
/// @note WaveMeshFade は海サイズの水面で 4 本すべてを寝かせる。変位を諦めるのは正しいが、法線まで
/// @note 消すと «空を映すだけの板» になる。波長がピクセルより十分大きい間は傾きとして返せる。
/// @note 傾きの上限 1.2 (約 50 度) は安全弁。ak > 1 の波は本来砕けており、変位を伴わないここでは
/// @note そのままの傾きを出すと «壁» に見える。
/// @note 位相の画面周波数は dot(D, dWorldDxy)/波長。直交した伸びは位相を変えず、両画面軸が 0.5 周期/pixel 未満なら Nyquist を満たす。
/// @note 群の斜めの包絡も位相幅へ含め、基本波に直交した長い footprint で群だけが alias することを防ぐ。
/// @note セル幅の補間後に非線形fadeを再評価すると、頂点法線の保持率と残差の和が1を超える。各頂点のfadeを法線と同じ重みで補間する。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems Chapter 1, directional wave phase and surface derivatives
float2 WaterResidualWaveSlope(float2 worldXZ, float time, float2 dWorldDx, float2 dWorldDy,
                              float4 meshWaveFade[4], out float lostVariance)
{
    float2 slope = float2(0.0f, 0.0f);
    lostVariance = 0.0f;
    float2 rotation, scales;
    ResolveWaveSpread(rotation, scales);
    float grouping = saturate(g_reflectParams.y);
    float3 depthState = WaterDepthState(worldXZ).vertical;

    [loop]
    for (int i = 0; i < WaterWaveCount(); ++i)
    {
        if (g_waveDir[i].w <= 0.0f) continue;
        float wavelength = max(g_waveParams[i].y, 1.0e-4f);
        float meshFade = saturate(meshWaveFade[i >> 2][i & 3]);
        float residual = 1.0f - meshFade;
        if (residual <= 0.001f) continue;
        [unroll]
        for (int part = 0; part < 2; ++part)
        {
            float2 D = normalize(g_waveDir[i].xy);
            float amplitudeScale;
            ApplyWaveSpread(i, part, rotation, scales, D, amplitudeScale);
            if (amplitudeScale <= 0.001f) continue;
            float phaseFootprint = max(abs(dot(D, dWorldDx)), abs(dot(D, dWorldDy)));
            const float groupRatio = WaterWaveGroupRatio(i);
            if (grouping > 0.0001f) {
                const float2 groupDir = WaterWaveGroupDirection(i, D);
                phaseFootprint += groupRatio * max(abs(dot(groupDir, dWorldDx)), abs(dot(groupDir, dWorldDy)));
            }
            float pixelFade = saturate(1.0f - phaseFootprint * 2.0f / wavelength);
            float k = g_waveParams[i].w;
            float baseAmplitude = g_waveParams[i].x * amplitudeScale;
            float amplitude = baseAmplitude * depthState.x;
            const float3 group = WaveGroupEnvelope(i, D, k, g_waveParams[i].z, worldXZ, time);
            float phi = k * dot(D, worldXZ) - g_waveParams[i].z * time + WaterWavePhase(i);
            const float2 amplitudeGradient = group.x * depthState.yz + depthState.x * group.yz;
            const float2 heightGradient = amplitude * group.x * k * cos(phi) * D
                + baseAmplitude * sin(phi) * amplitudeGradient;
            slope += heightGradient * (residual * pixelFade);
            /// @note cos の平均二乗は 1/2。方向の trace を 2 軸へ均し、群の平均二乗と保持した振幅の二乗を含める。
            float retained = meshFade + residual * pixelFade;
            float amplitudeK = amplitude * k;
            const float groupEnergy = 1.0f + 0.5f * grouping * grouping * (1.0f + groupRatio * groupRatio);
            lostVariance += 0.25f * amplitudeK * amplitudeK * groupEnergy
                          * (1.0f - retained * retained);
        }
    }

    float len = length(slope);
    if (len > 1.2f) slope *= 1.2f / len;
    return slope;
}

/// @brief 接空間法線。tangent = +X(world), binormal = +Z(world) なので xy がそのまま world XZ に対応する。
/// @param geoNormal 頂点法線 (ワールド)。うねりの斜面を読み取り、さざ波の分布へ反映する。
/// @param detailGain 流れの場が持ち上げるさざ波の強度倍率 (1 起点)。
/// @return xyz = 接空間法線, w = 強度込みの落とした傾きの分散 (specular AA で粗さへ回す)。
/// @note うねりの勾配でさざ波の座標をずらす (領域ワープ)。波が «さざ波を運ぶ» ので、ワープが無いと
/// @note うねりとさざ波が別々の層として滑って見える。ワープ量は画面上でゆっくり変わるため、
/// @note 異方フィルタへ渡す微分には含めていない。
float4 SampleWaterNormal(float2 worldXZ, float2 uv, float2 flowDir, float2 flowTravel, float time, float footprint,
                          float2 dWorldDx, float2 dWorldDy, float3 geoNormal, float detailGain,
                          float4 meshWaveFade[4], float2 baseWorldXZ, float2 dBaseDx, float2 dBaseDy, bool splineSurface,
                          float normalStrength, float detailStrength, bool followCurrent, WaterCurrentAdvection current)
{
    float2 swellSlope = -geoNormal.xz / max(geoNormal.y, 0.2f);
    float2 warpOffset = swellSlope * g_flowParams.w;
    if (splineSurface) {
        /// @note 正のworld流速へ模様を運ぶため、回転・異方変換の前に逆向きへ座標を遡る。
        warpOffset -= flowTravel;
        flowTravel = 0.0f;
    }

    /// @note Use the warped coordinate's footprint for mip selection and keep the unwarped footprint as a lower bound.
    dWorldDx += ddx(warpOffset);
    dWorldDy += ddy(warpOffset);
    float detailFootprint = max(footprint, max(length(dWorldDx), length(dWorldDy)));
    float3 detail = WaterDetailGradient(worldXZ + warpOffset, flowDir, flowTravel, time, detailFootprint,
        dWorldDx, dWorldDy, followCurrent, current);
    /// @note 風上を向いた斜面ほどさざ波が立ち、風下は凪ぐ。一様に散らすとうねりが «ただの起伏» に見える。
    float windward = saturate(0.5f + dot(swellSlope, flowDir));
    float detailScale = max(detailStrength, 0.0f) * lerp(0.55f, 1.35f, windward) * max(detailGain, 0.0f);
    float2 grad = detail.xy * detailScale;

    float residualVariance = 0.0f;
    const float seam = WaterfallWaveWeight(baseWorldXZ);
    grad += WaterResidualWaveSlope(baseWorldXZ, time, dBaseDx, dBaseDy, meshWaveFade, residualVariance) * seam;
    residualVariance *= seam * seam;

    float2 ripple = DecodeWaterRipple(g_rippleTex.Sample(g_samplerClamp, uv).rgb).rg
                 * WaterLocalDataWeight(uv);
    /// @note A flat interaction normal contributes zero slope. Bound the conversion at grazing slopes.
    /// @see https://jcgt.org/published/0009/03/04/ Mikkelsen (2020), surface gradient composition.
    float rippleNormalZ = sqrt(saturate(1.0f - dot(ripple, ripple)));
    grad -= 0.5f * ripple / max(rippleNormalZ, 0.25f);
    float3 waveNormal = normalize(float3(-grad.x, -grad.y, 1.0f));
    return float4(normalize(lerp(float3(0.0f, 0.0f, 1.0f), waveNormal, normalStrength)),
                  (detail.z * detailScale * detailScale + residualVariance)
                      * normalStrength * normalStrength);
}

float4 SampleWaterNormal(float2 worldXZ, float2 uv, float2 flowDir, float2 flowTravel, float time, float footprint,
                          float2 dWorldDx, float2 dWorldDy, float3 geoNormal, float detailGain,
                          float4 meshWaveFade[4], float2 baseWorldXZ, float2 dBaseDx, float2 dBaseDy, bool splineSurface,
                          float normalStrength, float detailStrength)
{
    return SampleWaterNormal(worldXZ, uv, flowDir, flowTravel, time, footprint, dWorldDx, dWorldDy,
        geoNormal, detailGain, meshWaveFade, baseWorldXZ, dBaseDx, dBaseDy, splineSurface,
        normalStrength, detailStrength, false, (WaterCurrentAdvection)0);
}

float4 SampleWaterNormal(float2 worldXZ, float2 uv, float2 flowDir, float2 flowTravel, float time, float footprint,
                          float2 dWorldDx, float2 dWorldDy, float3 geoNormal, float detailGain,
                          float4 meshWaveFade[4], float2 baseWorldXZ, float2 dBaseDx, float2 dBaseDy, bool splineSurface)
{
    return SampleWaterNormal(worldXZ, uv, flowDir, flowTravel, time, footprint, dWorldDx, dWorldDy,
        geoNormal, detailGain, meshWaveFade, baseWorldXZ, dBaseDx, dBaseDy, splineSurface,
        g_normalParams.w, g_detailParams.z);
}

/// @brief 水面メッシュ外周のフェード係数。矩形の縁でアルファを落とす。
/// @param widthUV   CPU が «メートル / extent» で渡す帯幅 (UV 単位)。0 でフェード無効。
/// @param extent    水面のワールド実寸 [m]。帯をピクセル幅で下支えするのに使う。
/// @param footprint 1 ピクセルが覆うワールド距離 [m]。
/// @note 水面はゼロ厚のシートなので、矩形の縁では «板の切り口» のような直線でシーンが切り替わる。
/// @note 縁へ向かって消していけば «岸に向かって薄くなる水» として読める。
/// @note 帯の幅はメートル固定なので、海サイズでは縁が遠すぎて 1 ピクセルに収まる。画面上で数ピクセル
/// @note ぶんを下支えすれば、どんな大きさでも縁は溶けたまま消える。上限 0.05 / 0.15 は、寝た視線で
/// @note 遠景の水がまとめて消えるのと、小さな水面の中央まで薄まるのを防ぐ歯止め。
float WaterEdgeFade(float2 uv, float2 widthUV, float2 extent, float footprint)
{
    if (g_farOceanParams.x > 0.5f) return 1.0f;
    float2 minWidth = min(footprint * 4.0f / max(extent, 1.0e-4f), 0.05f);
    float2 w = min(max(widthUV, minWidth), 0.15f);
    float2 d = min(uv, 1.0f - uv); ///< @brief 最も近い縁までの距離 [0, 0.5]
    float fx = widthUV.x > 1.0e-5f ? smoothstep(0.0f, w.x, d.x) : 1.0f;
    float fy = widthUV.y > 1.0e-5f ? smoothstep(0.0f, w.y, d.y) : 1.0f;
    return fx * fy;
}

/// @brief 深度バッファの生値 (Reversed-Z: near → 1、far → 0) を視空間 Z へ直す。
/// @note 平行投影では深度が既に線形。透視用の逆数式を通すと水深フェード・屈折・水面 SSR が一斉にずれる。
/// @see Common/Space.hlsli の LinearizeDepth (同じ式)
float LinearizeDepth(float rawDepth)
{
    if (isOrthographic > 0.5f) return farZ - rawDepth * (farZ - nearZ);
    return (nearZ * farZ) / max(nearZ + rawDepth * (farZ - nearZ), 0.0001f);
}

/// @brief 水面専用 SSR。
/// @return rgb = 反射色, a = 信頼度 (0 でヒット無し)。
/// @note Water 描画直前の sceneDepth / sceneColor を使うため、現在の水面を読み戻す競合を起こさず、
/// @note 既に描画済みの不透明・半透明オブジェクトを反射できる。
float4 TraceWaterSSR(float3 worldPos, float3 normal)
{
    if (waterSsrEnabled < 0.5f || ssrIntensity <= 0.0f || ssrSteps <= 0)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    float3 incident = normalize(worldPos - cameraPos);
    float3 rayDirVS = normalize(mul(float4(reflect(incident, normal), 0.0f), view).xyz);
    float3 rayPosVS = mul(float4(worldPos, 1.0f), view).xyz + rayDirVS * max(ssrThickness, 0.02f);
    /// @note RenderSettings の SSR 品質をそのまま流すと、広い水面で step 数×ピクセル数の負荷が跳ねる。
    int waterSsrSteps = min(ssrSteps, WATER_SSR_MAX_STEPS);
    float stepLength = ssrMaxDistance / max((float)waterSsrSteps, 1.0f);

    [loop]
    for (int step = 0; step < waterSsrSteps; ++step)
    {
        rayPosVS += rayDirVS * stepLength;
        if (rayPosVS.z <= nearZ || rayPosVS.z >= farZ)
            break;

        float4 clip = mul(float4(rayPosVS, 1.0f), projection);
        if (clip.w <= 0.0f)
            break;

        float2 rayUV = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
        if (any(rayUV <= 0.0f) || any(rayUV >= 1.0f))
            break;

        float sceneRawDepth = g_sceneDepth.SampleLevel(g_samplerClamp, rayUV, 0).r;
        /// @note 空 (Reversed-Z で深度 0) は反射の当たり先にしない。
        if (sceneRawDepth <= 0.0f)
            continue;

        float sceneViewDepth = LinearizeDepth(sceneRawDepth);
        float depthDelta = rayPosVS.z - sceneViewDepth;
        float hitThickness = max(ssrThickness, stepLength * abs(rayDirVS.z));
        if (depthDelta >= 0.0f && depthDelta <= hitThickness)
        {
            /// @note 画面端では不安定なヒットを環境反射へ滑らかにフォールバックする。
            float2 edgeDistance = min(rayUV, 1.0f - rayUV);
            float confidence = saturate(min(edgeDistance.x, edgeDistance.y) * 12.0f);
            return float4(g_sceneColor.SampleLevel(g_samplerClamp, rayUV, 0).rgb,
                          confidence * saturate(ssrIntensity));
        }
    }
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}

/// @brief 4 テクセルの深度を byte 復号してから補間し、底の被覆率と基準水深を返す。
/// @note R の旧泡マスクと互換を保ち、GB の carry を線形フィルターによる byte 丸めで壊さない。
float2 SampleWaterTerrainDepth(float2 uv)
{
    if (any(uv < 0.0f) || any(uv > 1.0f)) return float2(0.0f, 0.0f);
    int2 dimensions = max((int2)g_terrainInfoParams.zw, int2(1, 1));
    float2 pixel = saturate(uv) * (float2)dimensions - 0.5f;
    int2 basePixel = (int2)floor(pixel);
    float2 blend = frac(pixel);
    float depthSum = 0.0f;
    float coverage = 0.0f;
    [unroll]
    for (int z = 0; z < 2; ++z) {
        [unroll]
        for (int x = 0; x < 2; ++x) {
            int2 texel = clamp(basePixel + int2(x, z), int2(0, 0), dimensions - 1);
            float4 value = g_foamMask.Load(int3(texel, 0));
            uint flags = (uint)round(value.a * 255.0f);
            bool dry = (flags & 2u) != 0u;
            /// @note Horizon の範囲外深度でも、地上の底は既定の外洋深度へ戻さない。
            if ((flags & 1u) == 0u || ((flags & 4u) != 0u && !(dry && all(g_terrainDomain.zw > 0.0f)))) continue;
            float weight = (x == 0 ? 1.0f - blend.x : blend.x)
                         * (z == 0 ? 1.0f - blend.y : blend.y);
            uint highByte = (uint)round(value.g * 255.0f);
            uint lowByte = (uint)round(value.b * 255.0f);
            float depth = ((float)(highByte * 256u + lowByte) / 65535.0f * 2.0f - 1.0f)
                        * g_terrainInfoParams.y;
            if (dry) depth = min(depth, 0.0f);
            coverage += weight;
            depthSum += weight * depth;
        }
    }
    return float2(coverage, depthSum / max(coverage, 1.0e-6f));
}

/// @brief 水平変位後の world XZ を地形情報の基準 UV へ戻す。
/// @note 水域は水平な直交 TRS。負 scale の向きも axis に残し、泡を Gerstner の水平変位に追従させない。
float2 WaterTerrainUvAtWorldXZ(float2 worldXZ)
{
    float2 origin = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), g_worldMatrix).xz;
    float2 axisX = normalize(mul(float4(1.0f, 0.0f, 0.0f, 0.0f), g_worldMatrix).xz);
    float2 axisZ = normalize(mul(float4(0.0f, 0.0f, 1.0f, 0.0f), g_worldMatrix).xz);
    float2 offset = worldXZ - origin;
    float2 terrainCenter = all(g_terrainDomain.zw > 0.0f) ? g_terrainDomain.xy : float2(0.0f, 0.0f);
    return 0.5f + (float2(dot(offset, axisX), dot(offset, axisZ)) - terrainCenter)
        / max(WaterTerrainExtent(), 1.0e-6f);
}

/// @brief Tests the current surface point against its resolved owner's exclusion boxes.
bool WaterSurfaceExcluded(WaterPSInput p)
{
#ifndef FBZZ_WATER_MEDIUM_CAPTURE
    if (g_farOceanParams.w > 0.5f)
    {
        FBZZWaterMediumQueryContext mediumContext;
        const uint encodedOwner = (uint)round(g_farOceanParams.w);
        if (!FBZZ_WaterMediumHeader(mediumContext) || encodedOwner == 0u
            || encodedOwner > mediumContext.bodyCount)
            return true;
        return FBZZ_WaterPointExcluded(encodedOwner - 1u, p.worldPos, mediumContext);
    }
#endif
    return false;
}


#endif
