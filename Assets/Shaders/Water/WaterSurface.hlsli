/// @file    WaterSurface.hlsli
/// @brief   水面の共有入力、頂点評価、選択と材質から使う水面ヘルパーの入口。
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
/// @note C++ の WATER_RIVER_WAVE_ROCK_COUNT と同期する。
#define WATER_RIVER_WAVE_ROCK_COUNT 8
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

cbuffer WaterCB : register(FBZZ_REGISTER(b, CB_WATER_SLOT))
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
    float4   g_riverWaveParams; ///< @note x=河川の基本波の周期 [s]、y=岩の数、zw=波速の評価に使う基本波・短波の代表波長 [m]。
    float4   g_riverWaveRocks[2 * WATER_RIVER_WAVE_ROCK_COUNT]; ///< @note 偶数行=中心 XYZ と半径、奇数行 x=鉛直半径 [m]。
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
FBZZ_VS_SBUFFER(WaterPatchInstance, g_waterPatchInstances, VS_SB_INSTANCE_SLOT);

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
FBZZ_TEX2D(g_foamMask, SRV_WATER_FOAM_SLOT); ///< @brief 岸沿いの泡マスク (地形高さから CPU 生成)
FBZZ_TEX2D(g_detailNoise, SRV_WATER_DETAIL_NOISE_SLOT); ///< @brief さざ波タイル (起動時に CPU で焼く。RG=勾配, B=高さ)
FBZZ_TEX2D(g_sceneDepth, SRV_WATER_SCENE_DEPTH_SLOT); ///< @brief Water 描画直前の深度コピー
FBZZ_TEX2D(g_sceneColor, SRV_WATER_SCENE_COLOR_SLOT); ///< @brief Water 描画直前の HDR コピー
FBZZ_TEX2D(g_rippleTex, SRV_WATER_RIPPLE_SLOT); ///< @brief 着水波紋 (CPU 生成)
FBZZ_TEX2D_T(float, g_shadowMap, SRV_WATER_SHADOW_SLOT);
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
    float2 riverTravelTimes : TEXCOORD3; ///< @brief 基本波・短波の符号付き移動時間 [s]。
};

struct WaterPSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float2 uv         : TEXCOORD1;
    float3 normal     : TEXCOORD2;
    float3 tangent    : TEXCOORD3; ///< @brief スプラインでは正規化前の ∂P/∂x。
    float3 binormal   : TEXCOORD4; ///< @brief スプラインでは正規化前の ∂P/∂z。
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
    float2 splineLocalVelocity : TEXCOORD23; ///< @brief スプラインの基流へ加える Flow Field の速度 [world XZ m/s]。
    float splineSmallWaveHeight : TEXCOORD24; ///< @brief スプラインの着水波紋と河川波の Y 変位 [m]。
    float4 riverWave : TEXCOORD25; ///< @brief x=岩を回り込む移動時間 [s]、yzw=PS が補う 3 帯域の振幅 [m]。
    float riverShortTravelTime : TEXCOORD26; ///< @brief 短波帯の移動時間 [s]。
    float4 riverStandingWave : TEXCOORD27; ///< @brief 固定波の強さ、強さで重み付けした断面位相、泡の供給量と供給量で重み付けした流下距離 [m]。
    float riverStandingAmplitude : TEXCOORD28; ///< @brief 強さを適用した固定波の高さの尺度 [m]。
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

#include "Water/WaterSurfaceTerrain.hlsli"
#include "Water/WaterSurfaceWaves.hlsli"
#include "Water/WaterSurfaceFlow.hlsli"

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

#include "Water/WaterRiverWaves.hlsli"

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

    const bool riverWaves = splineSurface && g_riverWaveParams.x > 0.0f;
    WaterWaveDepthState depthState = WaterDepthState(basePos.xz, riverWaves);
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
    WaterFlowSample flowSample = SampleWaterSurfaceFlow(worldPos.xz, basePos.xz);
    float4 riverWave = 0.0f;
    float2 riverTravelTimes = 0.0f;
    float2 standingWave = 0.0f;
    float2 standingFoam = 0.0f;
    float4 standingSection = 0.0f;
    [branch] if (riverWaves) {
        const float2 transportVelocity = v.splineData.yz + flowSample.localVelocity;
        riverWave = WaterRiverWaveVertex(v, basePos, transportVelocity,
            depthState, meshCellSize, time, seamWeight, riverTravelTimes);
        standingWave = WaterRiverStandingWave(basePos.xz, v.splineData.yz, transportVelocity, v.splineData.x,
            depthState.terrainDepth, meshCellSize, seamWeight, true, standingFoam, standingSection);
        /// @note Keep the crest readable while retaining the displacement limit shared with culling and the medium atlas.
        riverWave *= 1.0f - max(0.76f * standingWave.y, abs(standingWave.x) / RIVER_DISPLACEMENT_LIMIT);
    }
    const float smallWaveHeight = rippleHeight + riverWave.x + standingWave.x;
    worldPos.y += smallWaveHeight;
    relativeSurfaceY += smallWaveHeight;

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

    /// @note tangent = ∂P/∂x, binormal = ∂P/∂z。法線は cross(binormal, tangent) で +Y を向く。
    /// @warning 逆順 cross(tangent, binormal) は平坦な水面で (0,-1,0) を返す。以前はこれで法線が
    /// @note 真下を向き、NdotV が常に 0 → フレネル飽和・スペキュラ消失・影の反転を起こしていた。
    /// @note スプラインは PS で基準面と波紋の勾配を加えるため、微分の長さも保持する。
    if (!splineSurface) {
        tangent = normalize(tangent);
        binormal = normalize(binormal);
    }
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
    o.splineLocalVelocity = splineSurface ? flowSample.localVelocity : 0.0f;
    o.splineSmallWaveHeight = splineSurface ? smallWaveHeight - standingWave.x : 0.0f;
    o.riverWave = float4(riverTravelTimes.x, riverWave.yzw);
    o.riverShortTravelTime = riverTravelTimes.y;
    o.riverStandingWave = float4(standingSection.x, standingSection.x * standingSection.w / max(standingSection.y, 0.55f), standingFoam);
    o.riverStandingAmplitude = standingSection.z;
    return o;
}

/// @brief 既存の indexed Water 経路から共有評価を呼ぶ互換 wrapper。
/// @note SV_InstanceID は StartInstanceLocation を含まないため、候補の先頭は root constant から明示的に加算する。
/// @note The backend resets b15 to zero for direct draws and writes the candidate offset in 24-byte indexed-indirect commands.
/// @see https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/ SV_InstanceID excludes StartInstanceLocation.
cbuffer WaterGeometryDrawConstants : register(FBZZ_REGISTER(b, CB_INSTANCE_DRAW_OFFSET_SLOT))
{
    uint g_waterGeometryInstanceOffset;
};

WaterPSInput VSMain(WaterVSInput v, uint instanceId : SV_InstanceID)
{
    return EvaluateWaterVertex(v, instanceId + g_waterGeometryInstanceOffset);
}

#include "Water/WaterSurfaceDetail.hlsli"

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
