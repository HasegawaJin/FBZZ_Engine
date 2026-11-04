/// @file    WaterComponent.hpp
/// @brief   水面コンポーネント（ジオメトリ・水の種類 (.mat) の参照・浮力・着水）。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// @brief 水の色・質感・風と Rectangle の基流は materialPath が指す .mat が持つ。
/// @note wavesAssetRef が設定されている場合は、波の基波・群・方向分布だけを共有 .fzdata で上書きする。
#pragma once

#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/DataAssetRef.hpp>
#include <Engine/Scene/WaterDepthField.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterZoneSnapshot.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Reflection.hpp>
#include <Fluid/GerstnerWaves.hpp>
#include <Fluid/VectorFieldAsset.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Physics/BodyHandle.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

struct WaterExclusionSnapshot;

inline void WaterSplinePoint::Reflect(IReflector& reflector)
{
    const std::uint32_t originalID = id;
    const int initialProxy = static_cast<int>((std::min)(id,
        WATER_SPLINE_MAX_POINT_ID));
    int pointID = initialProxy;
    reflector.Field("id", pointID);
    id = pointID == initialProxy ? originalID
        : static_cast<std::uint32_t>((std::max)(pointID, 0));
    reflector.Field("localPosition", localPosition);
    reflector.Field("width", width);
    reflector.Field("depth", depth);
    reflector.Field("flowSpeed", flowSpeed);
}

/// @brief GerstnerWave — 水面を構成する 1 本の波のパラメータ。
/// @note 水面全体をシミュレーションせず、少数の解析波を合成して低コストに大きなうねりを表現する。
using GerstnerWave = fluid::GerstnerWave;

/// @brief 水面 1 枚が «形と質感» として受け取る流れの本数。WaterCB の枠数と揃えること。
inline constexpr int kWaterSurfaceFlowCount = 8;

/// @brief 流れが水面の形へ出せる変位の上限 [m]。穴にも盛り上がりにも同じ値で効く。
/// @note 見た目の歯止めで物理的な意味は無い。Bernoulli をそのまま採ると speed 10 m/s が
/// @note 5 m の穴を掘り、水面が裏返って «底» が見える。
inline constexpr float kMaxWaterSurfaceDisplacement = 3.0f;

/// @brief 渦の軸が «鉛直» と認める、軸と上向きの内積の下限。
/// @note 45 度 (0.707) をわずかに下回るところ。これより寝た渦は水面を掘らず撫でるだけで、
/// @note 高さの解析項として書けない。Inspector の注意書きも同じ値を見る。
inline constexpr float kWaterVortexUprightDot = 0.7f;

/// @brief 形の Gaussian の実効半径を影響半径の何割にするか。
/// @note 0.4 なら r = radius で exp(-6.25) ≈ 0.2% = 実質 0。矩形の外へはみ出さない。
inline constexpr float kWaterSurfaceFlowShapeRatio = 0.4f;

/// @brief この流速 [m/s] でさざ波の向きと質感が «一様な流れ» と 1:1 で釣り合う。
/// @warning Water.hlsl の kWaterFlowReference と同じ値であること。
inline constexpr float kWaterSurfaceFlowReference = 4.0f;

/// @brief 水面の形・流れ・質感に出る «この水面に効く流れ» 1 本。
/// @brief WaterSystem が FlowFieldFrame から毎フレーム組み立てる (保存しない)。
///
/// @warning vectorField はフレームを跨いで持たない。速度場 PNG のホットリロードは AssetStore の
/// @note スロットへ新しい実体を差し込むため、古いポインタは黙って別の場を指す。
/// @see Docs/design/water-waves.md 「流れの場が水面に出る 3 つの道」
struct WaterSurfaceFlow {
    FlowFieldType kind   = FlowFieldType::Vortex; ///< @brief 型。CB へは float として渡る
    math::Vector2 center = math::Vector2::ZERO;   ///< @brief ワールド XZ [m] (Baked は場の中心)
    float         radius = 0.0f;                  ///< @brief 影響半径 [m]。0 なら形にも質感にも出ない
    /// @brief 形の変位 [m]。穴は負、盛り上がりは正、形を持たない型は 0。Baked では上限値 (負)。
    float         height = 0.0f;
    /// @brief 流速 [m/s]。Vortex は符号が回る向き (軸 y の符号)、Baked だけ焼いた値に掛ける無次元の倍率。
    float         speed  = 0.0f;
    /// @brief 距離減衰の指数。FlowFieldSettings::falloffPower をそのまま持つ。
    float         falloffPower = 2.0f;
    math::Vector2 direction = math::Vector2::ZERO; ///< @brief Uniform の XZ 向き (正規化)
    /// @brief さざ波と泡のムラへ回す強さ [0,1]。Uniform / Curl だけが持つ。
    float         chop = 0.0f;

    /// @name Baked 型のときだけ使う
    /// @{
    const fluid::VectorFieldAsset* vectorField = nullptr; ///< @brief 非所有。今フレームだけ有効
    math::Quaternion inverseRotation{};                   ///< @brief ワールド → 場のローカル
    math::Vector3    extents = { 1.0f, 1.0f, 1.0f };      ///< @brief 場を貼る箱の半径 [m]
    /// @brief 水面の基準面 Y − 場の中心 Y [m]。標本化する高さを «波を乗せる前» に固定するため。
    float            planeOffsetY = 0.0f;
    /// @}
};

/// @brief 同時に生かす着水の輪の上限。
/// @note 波紋テクスチャは «テクセル数 × 輪の数» を毎フレーム CPU で焼き直す。
inline constexpr size_t kMaxWaterRipples = 24;

/// @brief 輪が頂点へ乗るときの高さの振幅 [m]。輪の強さ [0,1] に掛ける。
inline constexpr float kWaterRippleHeightAmplitude = 0.15f;

/// @brief 波紋テクスチャの B チャンネルが表せる高さの範囲 [m] (±)。
/// @warning Water.hlsl の kRippleHeightScale と同じ値であること。片方だけ変えると、
/// @note 見えている輪と浮力が別の高さになる。
inline constexpr float kWaterRippleHeightScale = 0.5f;

/// @brief 生きている着水の輪 1 つ。**ワールド座標で持つ** (保存しない)。
///
/// @note 正本を WaterComponent へ置く理由: 描画パスの静的キャッシュに置くと SceneView と
/// @note GameView で寿命が 2 回進み、さらに CPU 側の GetSurfaceHeightAt が読めない。
/// @see Docs/design/water-waves.md 「波紋の帯分け」
struct WaterRipple {
    math::Vector2 center    = math::Vector2::ZERO; ///< @brief ワールド XZ [m]
    float         radius    = 0.0f;                ///< @brief 今の輪の半径 [m]
    float         width     = 0.25f;               ///< @brief 輪の太さ [m]
    float         amplitude = 0.0f;                ///< @brief 強さ [0,1]
    float         speed     = 1.2f;                ///< @brief 広がる速さ [m/s]
    float         decayRate = 1.3f;                ///< @brief 減衰 [1/s]
    /// @brief 頂点グリッドで刻めるか [0,1]。高さにだけ掛ける (法線は全部書く)。
    float         meshFade  = 0.0f;
    /// @brief この半径を超えたら捨てる [m]。水面の広さから WaterSystem が決める。
    float         maxRadius = 0.0f;
};

/// @brief WaterComponent — シーン上の水面 1 面分。
struct WaterComponent {
    /// @name ジオメトリ
    /// @{
    uint32_t resolutionX = 64;
    uint32_t resolutionZ = 64;
    float    extentX     = 100.0f;
    float    extentZ     = 100.0f;
    /// @brief Footprint geometry shared by rendering, physical queries, and water membership.
    WaterSurfaceShape surfaceShape = WaterSurfaceShape::RECTANGLE;
    /// @brief Stable-ID local control points for lake outlines and river centerlines.
    std::vector<WaterSplinePoint> splinePoints;
    /// @brief Select inline numeric points or ordered TargetPoint bindings as the position source.
    ControlPointSourceMode pointSourceMode = ControlPointSourceMode::INLINE;
    /// @brief Select live TargetPoint resolution or the last explicit owner-local numeric bake.
    ControlPointRuntimeMode pointRuntimeMode = ControlPointRuntimeMode::LIVE;
    /// @brief Ordered TargetPoint rows; IDs and Water attributes remain owner-authored.
    std::vector<WaterControlPointBinding> pointBindings;
    /// @brief Owner-local Y required by POINT_OBJECTS lake outlines.
    float lakePlaneHeight = 0.0f;
    /// @brief Last explicitly baked owner-local water control points.
    std::vector<WaterSplinePoint> bakedSplinePoints;
    /// @brief Source signature captured by the last explicit static bake.
    std::uint64_t bakedInputSignature = 0;
    /// @brief Source mode used to create the last explicit static bake.
    ControlPointSourceMode bakedSourceMode = ControlPointSourceMode::INLINE;
    /// @brief Water interpretation captured with the last explicit static bake.
    WaterSurfaceShape bakedSurfaceShape = WaterSurfaceShape::RECTANGLE;
    /// @brief Lake plane captured with the last explicit static bake.
    float bakedLakePlaneHeight = 0.0f;
    /// @brief Adapter version used to interpret the last explicit static bake.
    std::uint32_t bakedAdapterVersion = 0;
    /// @brief Whether bakedSplinePoints contains an explicit numeric snapshot.
    bool bakeValid = false;
    /// @brief Catmull-Rom samples per segment, bounded by the numeric snapshot builder.
    std::uint32_t splineSamplesPerSegment = 8;
    /// @brief Target maximum world edge length for the shared refined spline mesh [m].
    float splineMeshCellSize = 2.0f;
    /// @brief チャンク分割数。水面を chunkCount×chunkCount のサブメッシュに分割し、
    /// @brief チャンク単位でフラスタムカリングする。
    uint32_t chunkCount = 4;
    /// @brief 各ビューのカメラ付近へ頂点を寄せ、有限矩形の外周を維持する。
    bool cameraFocusedGrid = false;
    /// @brief カメラ付近の目標頂点間隔 [m]。ワールド単位で、均一格子より粗くはしない。
    float nearCellSize = 2.0f;
    /// @note 旧格子の設定を保持したまま、共有パッチのビュー別 LOD を選ぶ。
    bool adaptivePatchLod = false;
    uint32_t lodPatchResolution = 16;
    uint32_t lodMaxDepth = 9;
    uint32_t lodMaxPatches = 1024;
    /// @note 投影された格子セル幅の目標 [pixel]。幾何学的な高さ誤差の保証ではない。
    float lodTargetPixels = 8.0f;
    float lodNearDistance = 256.0f;
    float lodNearCellSize = 2.0f;
    bool lodMorphEnabled = false;
    float lodMorphTransitionWidth = 0.25f;
    /// @note Rectangle の水平水域を延長し、描画・浮力・水中判定を共有する。深さと除外は維持する。
    bool farOceanEnabled = false;
    /// @note active view の far clip を包む外周に掛ける余裕率。
    float farOceanCoverageScale = 1.05f;
    /// @note 有限水域の外で着水波紋の頂点変位を落とす距離 [world m]。
    float farOceanRippleFadeDistance = 4.0f;
    /// @}

    /// @name 波 (個体ごとの補正と、任意の共有波アセット)
    /// @{
    bool  enableGerstnerWaves = true;
    /// @brief .mat の振幅に掛ける倍率。同じ Ocean.mat を «凪の入り江» と «外洋» に使い分けるため。
    float waveAmplitudeScale  = 1.0f;
    /// @brief 設定時は .mat の波定義・群・広がりを上書きする。未設定なら従来の .mat 波を使う。
    DataAssetRef wavesAssetRef = { {}, "WaterWavesAsset" };
    /// @}

    /// @name 物理
    /// @{
    bool  buoyancyEnabled = true;
    /// @brief 完全に沈んだときの上向き加速度 [m/s^2]。重力 (9.8) を超えると浮く。
    float buoyancy        = 15.0f;
    /// @brief 水中での速度減衰 [1/s]。水流があるときは «水に対する» 速度に掛かる。
    float waterDrag       = 2.0f;
    /// @brief 水中判定と浮力領域が共有する水面からの深さ [m]。
    /// @note 水面は厚みを持たない板なので、上限が無いと «水面の真下にある洞窟» の中まで浮力が届く。
    float buoyancyDepth   = 10.0f;
    /// @brief 水深がこの値 [world m] に達するまで基波の振幅を浅瀬で滑らかに減衰する。0 で無効。
    float waveAttenuationDepth = 0.0f;
    /// @brief 剛体が水面を通過したときに波紋としぶきを出す。
    bool  splashEnabled   = true;
    /// @brief 水面接触・水中移動・地面接触から作る泡と局所濁りの設定。
    WaterInteractionSettings interactionSettings;
    /// @}

    /// @name 水の種類
    /// @{
    std::string materialPath;
    /// @}

    /// @name 状態フラグ
    /// @{
    bool enabled   = true;
    bool meshDirty = true;
    bool foamDirty = true;
    bool texDirty  = true;
    /// @}

    /// @name ランタイム (保存しない)
    /// @{
    /// @brief WaterSystem が毎フレーム、波アセットまたは .mat と倍率・環境風から組み立てた基波。
    /// @brief 描画・浮力・水中判定・スクリプトはすべてこれを読む。
    std::array<GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> waves = {};
    /// @brief 有効な基波の数。0〜16。本体の反復と GPU の波数はこの範囲だけを使う。
    std::uint32_t waveCount = 4;
    /// @brief Rectangle の基流速度 [m/s] (ワールド XZ)。Spline は制御点の flowSpeed を使う。
    math::Vector2 current = math::Vector2::ZERO;
    /// @brief WaterSystem が毎フレーム書く最密区間の頂点間隔 [m]。
    /// @note 描画の帯域制限だけに使う。浮力・水中判定の波高はこの間隔に依存しない。
    math::Vector2 cellSize = math::Vector2::ZERO;
    /// @brief 波の «群» の深さ [0,1]。.mat の waveGrouping をそのまま持つ。
    float waveGrouping = 0.0f;
    /// @brief 方向広がり [0,1]。.mat の waveSpread をそのまま持つ。0 で «1 波 1 方向»。
    float waveSpread = 0.0f;
    /// @brief WaterSystem が FlowFieldFrame から毎フレーム選び直す «この水面に効く流れ»。
    std::array<WaterSurfaceFlow, kWaterSurfaceFlowCount> surfaceFlows = {};
    int surfaceFlowCount = 0;
    /// @brief 生きている着水の輪。WaterSystem が寿命を進め、描画と GetSurfaceHeightAt が読む。
    std::vector<WaterRipple> ripples;
    /// @brief LiveQuery surface 用に Terrain から作る不変の数値 snapshot。
    /// @note Simulation は別 pointer と減衰閾値を捕捉し、LiveQuery の再構築で固定stepの評価を変えない。
    std::shared_ptr<const WaterDepthField> depthField;
    /// @brief Last fixed-step terrain depth mapping used by Simulation and rendering.
    std::shared_ptr<const WaterDepthField> simulationDepthField;
    /// @brief Wave attenuation threshold captured with the Simulation depth mapping.
    float simulationWaveAttenuationDepth = 0.0f;
    /// @brief Scene 変換と制御点から作る不変の数値水面。保存対象ではない。
    std::shared_ptr<const WaterSplineSnapshot> splineSnapshot;
    /// @brief Zone ownerが共有する不変の和集合水面。保存対象ではない。
    std::shared_ptr<const WaterZoneSnapshot> zoneSnapshot;
    /// @brief Last fixed-step water-zone publication used by simulation readers.
    std::shared_ptr<const WaterZoneSnapshot> simulationZoneSnapshot;
    /// @brief Exclusion boxes shared by LiveQuery consumers and the renderer.
    std::shared_ptr<const WaterExclusionSnapshot> exclusionSnapshot;
    /// @brief Fixed-step exclusion boxes captured with the simulation surface.
    std::shared_ptr<const WaterExclusionSnapshot> simulationExclusionSnapshot;
    /// @brief Pending water contact events, active plumes and their reusable emitters.
    WaterInteractionRuntime interactionRuntime;
    /// @brief Input key for reusing the immutable live exclusion snapshot.
    std::uint64_t exclusionSnapshotAttemptSignature = 0;
    bool exclusionSnapshotAttempted = false;
    /// @brief Last fixed-step standalone spline publication used by simulation readers.
    std::shared_ptr<const WaterSplineSnapshot> simulationSplineSnapshot;
    /// @brief Owner pose captured with the last fixed-step surface publication.
    Transform simulationTransform{};
    /// @brief Surface shape and Zone selection captured with the simulation geometry.
    WaterSurfaceShape simulationSurfaceShape = WaterSurfaceShape::RECTANGLE;
    bool simulationZoneSurfaceSelected = false;
    /// @brief True while an enabled Zone owns this Water surface, including an invalid Zone result.
    bool zoneSurfaceSelected = false;
    /// @brief True after the first simulation publication, including an invalid/empty result.
    bool simulationSurfacePublished = false;
    /// @brief Whether simulation readers should consume the captured surface instead of LiveQuery values.
    bool simulationReadEnabled = false;
    /// @brief Zone owner captured with the last fixed-step surface publication.
    EntityID simulationZoneOwner{};
    /// @brief Zone memberが参照するownerのEntityID。owner自身と単独水域は無効値。
    EntityID zoneOwner{};
    /// @brief 同じ入力の有効・無効な生成結果を毎フレーム再評価しないための runtime key。
    std::uint64_t splineSnapshotAttemptSignature = 0;
    bool splineSnapshotAttempted = false;
    physics::VolumeHandle volumeHandle;

    /// @brief Select the immutable depth field for the requested water read boundary.
    [[nodiscard]] const WaterDepthField* EffectiveDepthField(WaterSurfaceReadView view) const
    {
        if (view == WaterSurfaceReadView::SIMULATION && simulationReadEnabled
            && simulationSurfacePublished) return simulationDepthField.get();
        return depthField.get();
    }

    /// @brief Select the attenuation threshold paired with the requested depth field.
    [[nodiscard]] float EffectiveWaveAttenuationDepth(WaterSurfaceReadView view) const
    {
        if (view == WaterSurfaceReadView::SIMULATION && simulationReadEnabled
            && simulationSurfacePublished) return simulationWaveAttenuationDepth;
        return waveAttenuationDepth;
    }

    /// @brief Clear non-authoring geometry and handles after copying this component into another Scene.
    void ResetSurfaceRuntimeState()
    {
        depthField.reset();
        simulationDepthField.reset();
        simulationWaveAttenuationDepth = 0.0f;
        splineSnapshot.reset();
        zoneSnapshot.reset();
        simulationZoneSnapshot.reset();
        exclusionSnapshot.reset();
        simulationExclusionSnapshot.reset();
        exclusionSnapshotAttemptSignature = 0;
        exclusionSnapshotAttempted = false;
        simulationSplineSnapshot.reset();
        simulationTransform = Transform{};
        simulationSurfaceShape = WaterSurfaceShape::RECTANGLE;
        simulationZoneSurfaceSelected = false;
        zoneSurfaceSelected = false;
        simulationSurfacePublished = false;
        simulationReadEnabled = false;
        simulationZoneOwner = {};
        zoneOwner = {};
        splineSnapshotAttemptSignature = 0;
        splineSnapshotAttempted = false;
        m_waveCacheValid = false;
        volumeHandle = {};
    }

    const char* GetTypeName() const { return "Water"; }

    /// @brief 頂点グリッドで «刻めない» 波を寝かせる係数 [0,1]。Water.hlsl の WaveMeshFade と同じ式。
    /// @note Gerstner 波は頂点でしか評価されないため、1 波長あたり数セルしか取れない波は山と谷が
    /// @note セル境界で入れ替わり «別の波» に化ける。刻めない波は消し、細かさは手続きさざ波に任せる。
    /// @param cell 頂点グリッド 1 セルのワールド実寸 [m]。0 のときはフェードしない。
    static float WaveMeshFade(float wavelength, math::Vector2 cell)
    {
        const float c = (std::max)(cell.x, cell.y);
        if (c <= 0.0f) return 1.0f;
        /// @note smoothstep(2, 3.5, 1 波長あたりのセル数)。下限 2.0 は Nyquist。
        const float t = math::Clamp01((wavelength / c - 2.0f) / 1.5f);
        return t * t * (3.0f - 2.0f * t);
    }

    /// @brief 群の向きを波からどれだけ傾けるか (rad 0.55 の cos / sin)。Water.hlsl と同じ値。
    /// @note 群が波と同じ向きに進むだけだと波頭が «高さの揃った無限に長い直線» のままになる。
    /// @note 斜めにずらすと包絡が波頭に沿っても変化し、うねりが塊へ割れる。
    static constexpr float WAVE_GROUP_COS = 0.852525f;
    static constexpr float WAVE_GROUP_SIN = 0.522687f;

    /// @brief 波の «群» の包絡 [1-depth, 1+depth]。振幅にそのまま掛ける。
    /// @note 正弦を 4 本足しただけでは全波頭が同じ高さ・形になる。実海面は近い周波数の «うなり»
    /// @note で波が群れ、大きい塊と凪の区間が交互に来る。
    /// @param index 波の番号 [0,15]。群の波長帯と傾ける向きをここから決める。
    /// @note 群の角周波数を r*omega/2 にするのは、深水波の群速度が位相速度の 1/2 だから。
    /// @note これで «群はゆっくり進み、個々の波頭がその中を追い越していく» 見え方になる。
    /// @warning Water.hlsl の WaveGroupEnvelope と同じ式であること。片方だけ変えると、
    /// @note 見えている波と浮力が別の水面になる。
    static float WaveGroupEnvelope(int index, math::Vector2 direction, float k, float omega,
                                   float worldX, float worldZ, float time, float depth,
                                   float phaseOffsetRadians = 0.0f)
    {
        if (depth <= 0.0001f) return 1.0f;
        const int band = (std::max)(index, 0) % 4;
        const float ratio = 0.09f + 0.035f * static_cast<float>(band);
        const float sign  = (index & 1) ? -1.0f : 1.0f;
        const math::Vector2 groupDir = {
            direction.x * WAVE_GROUP_COS - direction.y * sign * WAVE_GROUP_SIN,
            direction.x * sign * WAVE_GROUP_SIN + direction.y * WAVE_GROUP_COS
        };
        const float groupK = ratio * k;
        const float phase = groupK * (groupDir.x * worldX + groupDir.y * worldZ)
                          - ratio * omega * 0.5f * time + phaseOffsetRadians;
        return 1.0f + depth * std::sin(phase);
    }

    /// @brief 方向広がりの成分 1 つぶんの «向きと振幅倍率»。
    struct WaveSpreadPart {
        math::Vector2 direction;
        float         amplitudeScale = 1.0f;
    };

    /// @brief 方向広がりの成分を割り出す。
    ///
    /// @param component 0 = 主成分, 1 = 伴走成分。
    /// @note 実海面の波は 1 方向へ揃わず狭い方向スペクトルを持つ。同じ波数で向きだけ違う波を
    /// @note 重ねると、波頭が有限の長さに切れる (short-crested sea)。
    /// @note 振幅は main^2 + comp^2 = 1 で分ける。分けないと spread を上げるだけで海が高くなる。
    /// @warning Water.hlsl の ApplyWaveSpread / ResolveWaveSpread と同じ式であること。
    static WaveSpreadPart WaveSpreadComponent(int index, int component, math::Vector2 direction,
                                              float spread)
    {
        const float clamped = math::Clamp01(spread);
        const float energy = math::Clamp01(0.40f * clamped);
        WaveSpreadPart part;
        part.direction = direction;
        if (component == 0) {
            part.amplitudeScale = std::sqrt(1.0f - energy);
            return part;
        }
        part.amplitudeScale = std::sqrt(energy);
        const float angle = clamped * 0.9f * ((index & 1) ? -1.0f : 1.0f);
        const float ca = std::cos(angle);
        const float sa = std::sin(angle);
        part.direction = { direction.x * ca - direction.y * sa,
                           direction.x * sa + direction.y * ca };
        return part;
    }

    /// @brief 主成分と伴走成分の振幅倍率の «和»。AABB のマージンと波高の基準に使う。
    static float WaveSpreadAmplitudeSum(float spread)
    {
        const float energy = math::Clamp01(0.40f * math::Clamp01(spread));
        return std::sqrt(1.0f - energy) + std::sqrt(energy);
    }

    /// @brief 中心を持つ流れ 1 本が作る変位 [m] (穴は負、盛り上がりは正)。
    ///
    /// @param worldX,worldZ ワールド XZ。**変位前へ戻さない** — 形は水平変位を持たないので
    /// @note 逆写像の対象ではなく、入れると Gerstner の位相まで穴のぶんずれる。
    /// @note 形を Gaussian にするのは C¹ 連続だから。(1 - r/R)^p は r = R で折れ、頂点法線が
    /// @note 1 セルだけ跳ねて縁に輪が出る。
    /// @warning Water.hlsl の WaterRadialDisplacement と同じ式であること。片方だけ変えると、
    /// @note 見えている形と浮力・水中判定が別の水面になる。
    static float WaterRadialDisplacement(const WaterSurfaceFlow& flow, float worldX, float worldZ)
    {
        if (std::abs(flow.height) <= 0.0001f || flow.radius <= 0.0001f) return 0.0f;
        const float scale = flow.radius * kWaterSurfaceFlowShapeRatio;
        const float dx = worldX - flow.center.x;
        const float dz = worldZ - flow.center.y;
        return flow.height * std::exp(-(dx * dx + dz * dz) / (scale * scale));
    }

    /// @brief 焼いた場 1 枚が作るへこみ [m] (0 以下)。
    ///
    /// @param baseX,baseZ **変位前の** ワールド XZ。高さは planeOffsetY で基準面に固定する。
    /// @note 高さを «波を乗せた後の Y» で引くと «高さを高さで引く» 循環になる。
    /// @note 流速の XZ 成分だけを Bernoulli へ通す。鉛直成分は水面を «押し上げる» 話で、
    /// @note 淀みの深さとしては二重に数えることになる。
    /// @warning Water.hlsl の WaterBakedDisplacement と同じ式であること。
    static float WaterBakedDisplacement(const WaterSurfaceFlow& flow, float baseX, float baseZ)
    {
        if (flow.vectorField == nullptr || flow.vectorField->Empty()) return 0.0f;
        if (std::abs(flow.height) <= 0.0001f) return 0.0f;
        const math::Vector3 toPoint = { baseX - flow.center.x, flow.planeOffsetY,
                                        baseZ - flow.center.y };
        const math::Vector3 local = flow.inverseRotation * toPoint;
        const math::Vector3 uvw = { local.x / flow.extents.x * 0.5f + 0.5f,
                                    local.y / flow.extents.y * 0.5f + 0.5f,
                                    local.z / flow.extents.z * 0.5f + 0.5f };
        if (uvw.x < 0.0f || uvw.x > 1.0f || uvw.y < 0.0f || uvw.y > 1.0f
            || uvw.z < 0.0f || uvw.z > 1.0f) return 0.0f;

        const math::Vector3& assetMin = flow.vectorField->boundsMin;
        const math::Vector3& assetMax = flow.vectorField->boundsMax;
        const math::Vector3 samplePoint = {
            assetMin.x + (assetMax.x - assetMin.x) * uvw.x,
            assetMin.y + (assetMax.y - assetMin.y) * uvw.y,
            assetMin.z + (assetMax.z - assetMin.z) * uvw.z };
        const math::Vector3 world =
            (flow.inverseRotation.Conjugate() * flow.vectorField->SampleLocal(samplePoint))
            * flow.speed;
        const float speedSq = world.x * world.x + world.z * world.z;
        return -(std::min)(speedSq / 19.6f, std::abs(flow.height));
    }

    /// @brief 流れ 1 本が作る変位 [m]。型で中心型と Baked を振り分ける。
    /// @param worldX,worldZ 変位«後» のワールド XZ (中心型が使う)。
    /// @param baseX,baseZ   変位«前» のワールド XZ (Baked が使う)。
    static float WaterSurfaceDisplacement(const WaterSurfaceFlow& flow,
                                          float worldX, float worldZ, float baseX, float baseZ)
    {
        return flow.kind == FlowFieldType::Baked
            ? WaterBakedDisplacement(flow, baseX, baseZ)
            : WaterRadialDisplacement(flow, worldX, worldZ);
    }

    /// @brief 波紋の輪 1 本ぶんの断面 [-1,1]。
    ///
    /// @param distance  輪の中心からの距離 [m]。
    /// @param ringRadius 今の輪の半径 [m]。
    /// @param width     輪の太さ [m]。断面の «波長» は 2·width。
    /// @warning 波紋テクスチャを焼く側と GetSurfaceHeightAt が**この 1 本だけ**を呼ぶこと。
    /// @note 片方だけ変えると、見えている輪の上で物が別の高さに浮く。
    static float WaterRippleProfile(float distance, float ringRadius, float width)
    {
        const float w = (std::max)(width, 1.0e-4f);
        const float ring = distance - ringRadius;
        return std::exp(-(ring * ring) / (w * w)) * std::sin(ring / w * math::PI);
    }

    /// @brief 輪 1 つの合成断面 (本体 + 後続 2 本)。法線にも高さにも同じ形を使う。
    /// @note 後続の輪は «石を落とした跡に残る第 2・第 3 の波»。1 本だけだと輪が薄い線に見える。
    static float WaterRippleWave(const WaterRipple& ripple, float distance)
    {
        const float w = (std::max)(ripple.width, 1.0e-4f);
        return WaterRippleProfile(distance, ripple.radius,            w) * 1.00f
             + WaterRippleProfile(distance, ripple.radius - w * 3.0f, w) * 0.42f
             + WaterRippleProfile(distance, ripple.radius - w * 6.0f, w) * 0.16f;
    }

    /// @brief 描画解像度から独立した波紋の水面高さ [m]。
    static float WaterRippleHeight(const WaterRipple& ripple, float worldX, float worldZ)
    {
        if (ripple.amplitude <= 0.0f) return 0.0f;
        const float dx = worldX - ripple.center.x;
        const float dz = worldZ - ripple.center.y;
        return WaterRippleWave(ripple, std::sqrt(dx * dx + dz * dz))
             * ripple.amplitude * kWaterRippleHeightAmplitude;
    }

    /// @brief 逆写像の反復回数。
    /// @note 減衰を波の急峻度から決めるので、収束する設定 (Σ Q·k·A ≤ 1) では素の不動点反復に
    /// @note なり、4 回で残差が mm 台へ入る。
    static constexpr int SURFACE_SOLVE_ITERATIONS = 4;

    struct ResolvedWave {
        math::Vector2 direction;
        float k = 0.0f;
        float omega = 0.0f;
        float amplitude = 0.0f;
        float horizontalAmplitude = 0.0f;
        float phaseOffsetRadians = 0.0f;
        int index = 0;
    };

    /// @note 保存しない。公開入力の直接編集も検出し、同一設定では超越関数を再評価しない。
    void PrepareWaveCache(WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY) const
    {
        const WaterDepthField* depth = EffectiveDepthField(view);
        const float attenuationDepth = EffectiveWaveAttenuationDepth(view);
        const uint64_t depthSignature = depth ? depth->signature : 0;
        bool same = m_waveCacheValid && m_cachedWaveCount == waveCount
                 && m_cachedSpread == waveSpread && m_cachedGrouping == waveGrouping
                 && m_cachedDepthSignature == depthSignature
                 && m_cachedAttenuationDepth == attenuationDepth;
        const size_t activeCount = (std::min)(static_cast<size_t>(waveCount), waves.size());
        for (size_t i = 0; same && i < activeCount; ++i) {
            const auto& a = waves[i];
            const auto& b = m_cachedWaves[i];
            same = a.direction.x == b.direction.x && a.direction.y == b.direction.y
                && a.amplitude == b.amplitude && a.wavelength == b.wavelength && a.steepness == b.steepness
                && a.phaseOffsetRadians == b.phaseOffsetRadians;
        }
        if (same) return;
        m_cachedWaves = waves;
        m_cachedWaveCount = waveCount;
        m_cachedSpread = waveSpread;
        m_cachedGrouping = waveGrouping;
        m_cachedDepthSignature = depthSignature;
        m_cachedAttenuationDepth = attenuationDepth;
        m_resolvedCount = 0;
        m_contraction = 0.0f;
        m_horizontalDisplacementBound = 0.0f;
        m_horizontalWaveScale = 1.0f;
        m_horizontalContraction = 0.0f;
        m_waveHeightBound = 0.0f;
        const float grouping = math::Clamp01(waveGrouping);
        /// @note Float-rounded direction normalization and rotations get a small upper-bound margin.
        const float peak = (1.0f + grouping + grouping * 0.195f) * WaveSpreadAmplitudeSum(waveSpread)
            * (1.0f + 32.0f * std::numeric_limits<float>::epsilon());
        for (size_t i = 0; i < activeCount; ++i) {
            const auto& wave = waves[i];
            if (wave.amplitude < 0.0001f || wave.wavelength <= 0.0001f
                || !std::isfinite(wave.phaseOffsetRadians)) continue;
            const float directionLengthSq = wave.direction.LengthSq();
            if (!std::isfinite(directionLengthSq) || directionLengthSq <= 1.0e-8f) continue;
            /// @note 物理と水域照会の波は描画の分割数・カメラ・LOD に依存させない。
            const float k = math::TWO_PI / wave.wavelength;
            m_contraction += math::Clamp01(wave.steepness) * wave.amplitude * peak * k;
            const float omega = std::sqrt(9.8f * k);
            const math::Vector2 direction = wave.direction.Normalized();
            for (int component = 0; component < 2; ++component) {
                const auto part = WaveSpreadComponent(static_cast<int>(i), component, direction, waveSpread);
                if (part.amplitudeScale <= 0.001f) continue;
                auto& resolved = m_resolvedWaves[m_resolvedCount++];
                resolved = { part.direction, k, omega, wave.amplitude * part.amplitudeScale,
                    math::Clamp01(wave.steepness) * wave.amplitude * part.amplitudeScale,
                    wave.phaseOffsetRadians, static_cast<int>(i) };
                m_horizontalDisplacementBound += std::abs(resolved.horizontalAmplitude) * (1.0f + grouping);
                m_waveHeightBound += std::abs(resolved.amplitude) * (1.0f + std::abs(waveGrouping));
            }
        }
        float attenuationGradient = 0.0f;
        if (attenuationDepth > 0.0f && depth
            && depth->attenuationDepth == attenuationDepth
            && std::isfinite(depth->maximumAttenuationGradient)
            && depth->maximumAttenuationGradient >= 0.0f) {
            attenuationGradient = depth->maximumAttenuationGradient;
            const float contractionBound = m_contraction + m_horizontalDisplacementBound * attenuationGradient;
            if (attenuationGradient > 0.0f && std::isfinite(contractionBound) && contractionBound > 0.849f)
                m_horizontalWaveScale = 0.849f / contractionBound;
        }
        m_horizontalContraction = m_horizontalWaveScale
            * (m_contraction + m_horizontalDisplacementBound * attenuationGradient);
        m_waveCacheValid = true;
    }

    /// @return 基準面からの絶対変位の保守的上限 [m]。時刻に依存しない。
    float SurfaceHeightBound(WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY) const
    {
        PrepareWaveCache(view);
        float bound = enableGerstnerWaves ? m_waveHeightBound : 0.0f;
        if (const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(*this, view)) {
            bound += (std::max)(std::abs(geometry->minimumBaseHeightOffset),
                std::abs(geometry->maximumBaseHeightOffset));
        }
        for (int i = 0; i < (std::min)(surfaceFlowCount, kWaterSurfaceFlowCount); ++i)
            bound += std::abs(surfaceFlows[static_cast<size_t>(i)].height);
        for (const auto& ripple : ripples)
            if (ripple.amplitude > 0.0f)
                bound += ripple.amplitude * kWaterRippleHeightAmplitude * 1.58f;
        /// @note 加算と三角関数の丸めが境界で早期判定を反転させないよう外側へ広げる。
        return bound + (std::max)(1.0e-5f, bound * 1.0e-5f);
    }

private:
    mutable bool m_waveCacheValid = false;
    mutable std::array<GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> m_cachedWaves{};
    mutable std::uint32_t m_cachedWaveCount = 0;
    mutable float m_cachedSpread = 0.0f;
    mutable float m_cachedGrouping = 0.0f;
    mutable uint64_t m_cachedDepthSignature = 0;
    mutable float m_cachedAttenuationDepth = 0.0f;
    mutable std::array<ResolvedWave, fluid::MAX_BASE_GERSTNER_WAVES * 2> m_resolvedWaves{};
    mutable size_t m_resolvedCount = 0;
    mutable float m_contraction = 0.0f;
    mutable float m_horizontalDisplacementBound = 0.0f;
    mutable float m_horizontalWaveScale = 1.0f;
    mutable float m_horizontalContraction = 0.0f;
    mutable float m_waveHeightBound = 0.0f;

    float AttenuationFactorAt(math::Vector2 point, WaterSurfaceReadView view) const
    {
        const WaterDepthField* depth = EffectiveDepthField(view);
        const float attenuationDepth = EffectiveWaveAttenuationDepth(view);
        if (!depth || attenuationDepth <= 0.0f
            || depth->attenuationDepth != attenuationDepth) return 1.0f;
        WaterDepthSample sample;
        return TrySampleWaterDepthField(*depth, point, sample) ? sample.factor : 1.0f;
    }

    math::Vector2 ResolvedHorizontalDisplacementAt(math::Vector2 point, float time,
        WaterSurfaceReadView view) const
    {
        math::Vector2 offset = math::Vector2::ZERO;
        const float attenuation = AttenuationFactorAt(point, view) * m_horizontalWaveScale;
        for (size_t i = 0; i < m_resolvedCount; ++i) {
            const auto& wave = m_resolvedWaves[i];
            const auto dir = wave.direction;
            const float envelope = WaveGroupEnvelope(wave.index, dir, wave.k, wave.omega,
                                                      point.x, point.y, time, waveGrouping,
                                                      wave.phaseOffsetRadians);
            const float phase = wave.k * (dir.x * point.x + dir.y * point.y) - wave.omega * time
                              + wave.phaseOffsetRadians;
            const float push = wave.horizontalAmplitude * envelope * std::cos(phase) * attenuation;
            offset.x += dir.x * push;
            offset.y += dir.y * push;
        }
        return offset;
    }

public:

    /// @brief Gerstner 波の水平変位 [m] (ワールド XZ)。
    /// @param undisplaced 変位«前»の水平位置。波の位相はここで取る。
    math::Vector2 HorizontalDisplacementAt(math::Vector2 undisplaced, float time,
        WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY) const
    {
        if (!enableGerstnerWaves) return math::Vector2::ZERO;
        PrepareWaveCache(view);
        return ResolvedHorizontalDisplacementAt(undisplaced, time, view);
    }

    /// @return CPU/GPU で共有する水平 Gerstner push の安全倍率 [0,1]。
    [[nodiscard]] float GetHorizontalWaveScale(
        WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY) const
    {
        PrepareWaveCache(view);
        return m_horizontalWaveScale;
    }

    /// @brief ワールド XZ に «水面が来ている» とき、その点の変位前の水平位置を解く。
    /// @note Gerstner 波は «変位前の位置» を位相の入力に取る。ワールド XZ をそのまま使うと水平変位分
    /// @note だけ別の場所の高さを読み、急な斜面では Q·A の合計 (Ocean.mat で 1〜2m) ずれる。
    /// @warning 折り畳んだ波では逆写像が一意でない (同じ XZ に複数の水面)。反復はその枝の 1 つへ
    /// @note 落ちる。初期値依存で連続性は保証しない。
    math::Vector2 SolveUndisplacedXZ(float worldX, float worldZ, float time,
        WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY) const
    {
        const math::Vector2 target = { worldX, worldZ };
        if (!enableGerstnerWaves) return target;

        /// @note 不動点反復の縮小率の上界 Σ Q·k·A。三角関数を 1 度も回さずに出せる。
        /// @note 群の包絡は最大 (1 + waveGrouping) 倍、方向広がりは主 + 伴走の和まで振幅を持ち上げる。
        PrepareWaveCache(view);
        const float contraction = m_horizontalContraction;
        /// @note 水平変位を持たない水面 (湖・プール・急峻度 0) は解く必要がない。
        if (contraction <= 0.0001f) return target;

        /// @note Σ Q·k·A ≤ 1 なら素の反復がそのまま縮小写像。超える («波が折り畳む») 設定では
        /// @note その比で減衰させ、有限回で暴れないところへ抑える。
        const float relax = 1.0f / (std::max)(contraction, 1.0f);

        math::Vector2 guess = target;
        for (int iteration = 0; iteration < SURFACE_SOLVE_ITERATIONS; ++iteration) {
            const math::Vector2 offset = ResolvedHorizontalDisplacementAt(guess, time, view);
            guess.x += (target.x - (guess.x + offset.x)) * relax;
            guess.y += (target.y - (guess.y + offset.y)) * relax;
        }
        return guess;
    }

    /// @brief 水面の基準面からの変位 [m] を CPU で評価する。
    /// @param worldX, worldZ ワールド座標 (変位«後»、つまり画面に出ている位置)。
    /// @note シェーダーは波の位相をワールド XZ で取るため、原点からの相対座標で評価すると
    /// @note 水面を原点以外へ置いた瞬間に浮力・水中判定と描画の波がずれる。
    /// @note Spline の基準高さ、流れの場が作る形、着水の輪も足す。中心型はワールド XZ、Baked と輪は変位前の XZ で
    /// @note 引く (VS が波紋テクスチャを «波を乗せる前の» 頂点 UV で引いているため)。
    /// @see SolveUndisplacedXZ
    float GetSurfaceHeightAt(float worldX, float worldZ, float time,
        WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY) const
    {
        float height = 0.0f;
        math::Vector2 origin = { worldX, worldZ };

        if (enableGerstnerWaves) {
            origin = SolveUndisplacedXZ(worldX, worldZ, time, view);
            const float attenuation = AttenuationFactorAt(origin, view);
            for (size_t i = 0; i < m_resolvedCount; ++i) {
                const auto& wave = m_resolvedWaves[i];
                const auto dir = wave.direction;
                const float envelope = WaveGroupEnvelope(wave.index, dir, wave.k, wave.omega,
                                                          origin.x, origin.y, time, waveGrouping,
                                                          wave.phaseOffsetRadians);
                const float phase = wave.k * (dir.x * origin.x + dir.y * origin.y) - wave.omega * time
                                  + wave.phaseOffsetRadians;
                height += wave.amplitude * envelope * std::sin(phase) * attenuation;
            }
        }

        if (const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(*this, view)) {
            WaterSplineSample base;
            if (TrySampleWaterSpline(*geometry, origin.x, origin.y, base))
                height += base.baseHeightOffset;
        }

        const int liveFlows = (std::min)(surfaceFlowCount, kWaterSurfaceFlowCount);
        for (int i = 0; i < liveFlows; ++i) {
            height += WaterSurfaceDisplacement(surfaceFlows[static_cast<size_t>(i)],
                                               worldX, worldZ, origin.x, origin.y);
        }

        for (const WaterRipple& ripple : ripples)
            height += WaterRippleHeight(ripple, origin.x, origin.y);

        return height;
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        const uint32_t oldResolutionX = resolutionX;
        const uint32_t oldResolutionZ = resolutionZ;
        const uint32_t oldChunkCount  = chunkCount;
        const bool oldAdaptivePatchLod = adaptivePatchLod;
        const bool oldFarOceanEnabled = farOceanEnabled;
        const uint32_t oldPatchResolution = lodPatchResolution;
        const float oldExtentX = extentX;
        const float oldExtentZ = extentZ;
        const WaterSurfaceShape oldSurfaceShape = surfaceShape;
        const std::uint32_t oldSplineSamplesPerSegment = splineSamplesPerSegment;
        const float oldSplineMeshCellSize = splineMeshCellSize;
        const std::vector<WaterSplinePoint> oldSplinePoints = splinePoints;
        const ControlPointSourceMode oldPointSourceMode = pointSourceMode;
        const ControlPointRuntimeMode oldPointRuntimeMode = pointRuntimeMode;
        const std::vector<WaterControlPointBinding> oldPointBindings = pointBindings;
        const float oldLakePlaneHeight = lakePlaneHeight;
        const std::vector<WaterSplinePoint> oldBakedSplinePoints = bakedSplinePoints;
        const std::uint64_t oldBakedInputSignature = bakedInputSignature;
        const ControlPointSourceMode oldBakedSourceMode = bakedSourceMode;
        const WaterSurfaceShape oldBakedSurfaceShape = bakedSurfaceShape;
        const float oldBakedLakePlaneHeight = bakedLakePlaneHeight;
        const std::uint32_t oldBakedAdapterVersion = bakedAdapterVersion;
        const bool oldBakeValid = bakeValid;
        const std::string oldMaterialPath = materialPath;

        /// @note IReflector は uint32_t 非対応のため int 経由で編集・保存する。
        int resX = static_cast<int>((std::clamp)(resolutionX, 1u, 512u));
        int resZ = static_cast<int>((std::clamp)(resolutionZ, 1u, 512u));
        int chunks = static_cast<int>((std::clamp)(chunkCount, 1u, 64u));
        int patchResolution = static_cast<int>((std::clamp)(lodPatchResolution, 2u, 64u));
        int maxDepth = static_cast<int>((std::clamp)(lodMaxDepth, 0u, 10u));
        int maxPatches = static_cast<int>((std::clamp)(lodMaxPatches, 1u, 4096u));
        r.Field("resolutionX", resX);
        r.Field("resolutionZ", resZ);
        r.Field("chunkCount", chunks);
        r.Field("cameraFocusedGrid", cameraFocusedGrid);
        r.Field("nearCellSize", nearCellSize);
        r.Field("adaptivePatchLod", adaptivePatchLod);
        r.Field("lodPatchResolution", patchResolution);
        r.Field("lodMaxDepth", maxDepth);
        r.Field("lodMaxPatches", maxPatches);
        r.Field("lodTargetPixels", lodTargetPixels);
        r.Field("lodNearDistance", lodNearDistance);
        r.Field("lodNearCellSize", lodNearCellSize);
        r.Field("lodMorphEnabled", lodMorphEnabled);
        r.Field("lodMorphTransitionWidth", lodMorphTransitionWidth);
        r.Field("farOceanEnabled", farOceanEnabled);
        r.Field("farOceanCoverageScale", farOceanCoverageScale);
        r.Field("farOceanRippleFadeDistance", farOceanRippleFadeDistance);
        r.Field("extentX", extentX);
        r.Field("extentZ", extentZ);
        int shapeValue = static_cast<int>(surfaceShape);
        static constexpr const char* SHAPES[] = { "Rectangle", "Spline Lake", "Spline River" };
        r.Enum("surfaceShape", shapeValue, SHAPES);
        surfaceShape = shapeValue < 0 || shapeValue > 2
            ? static_cast<WaterSurfaceShape>(0xffu) : static_cast<WaterSurfaceShape>(shapeValue);
        ReflectControlPointSourceMode(r, "pointSourceMode", pointSourceMode);
        ReflectControlPointRuntimeMode(r, "pointRuntimeMode", pointRuntimeMode);
        r.FloatRange("lakePlaneHeight", lakePlaneHeight, -1000000.0f, 1000000.0f);
        ReflectWaterControlPointBindings(r, pointBindings);
        ReflectWaterControlPointValues(r, "bakedSplinePoints", bakedSplinePoints);
        ReflectControlPointSignature(r, "bakedInputSignature", bakedInputSignature);
        ReflectControlPointSourceMode(r, "bakedSourceMode", bakedSourceMode);
        int bakedShapeValue = static_cast<int>(bakedSurfaceShape);
        static constexpr const char* BAKED_SHAPES[] = { "Rectangle", "Spline Lake", "Spline River" };
        r.Enum("bakedSurfaceShape", bakedShapeValue, BAKED_SHAPES);
        bakedSurfaceShape = bakedShapeValue < 0 || bakedShapeValue > 2
            ? static_cast<WaterSurfaceShape>(0xffu) : static_cast<WaterSurfaceShape>(bakedShapeValue);
        r.Field("bakedLakePlaneHeight", bakedLakePlaneHeight);
        ReflectControlPointVersion(r, "bakedAdapterVersion", bakedAdapterVersion);
        r.Field("bakeValid", bakeValid);
        if (pointSourceMode == ControlPointSourceMode::INLINE) {
            const std::size_t requestedSplinePointCount = r.BeginObjectList("splinePoints", splinePoints.size());
            if (requestedSplinePointCount <= WATER_SPLINE_MAX_CONTROL_POINTS
                || requestedSplinePointCount == splinePoints.size())
                splinePoints.resize(requestedSplinePointCount);
            std::uint32_t nextPointID = 1;
            for (std::size_t i = 0; i < splinePoints.size(); ++i) {
                auto& point = splinePoints[i];
                if (point.id == 0) {
                    while (nextPointID != 0 && std::any_of(splinePoints.begin(), splinePoints.end(),
                        [nextPointID](const WaterSplinePoint& candidate) { return candidate.id == nextPointID; }))
                        ++nextPointID;
                    if (nextPointID != 0) point.id = nextPointID++;
                }
                r.BeginObjectElement(i);
                point.Reflect(r);
                r.EndObjectElement();
            }
            const std::size_t removedSplinePoint = r.EndObjectList();
            if (removedSplinePoint < splinePoints.size())
                splinePoints.erase(splinePoints.begin() + static_cast<std::ptrdiff_t>(removedSplinePoint));
            std::size_t movedFrom = 0;
            std::size_t movedTo = 0;
            if (r.ObjectListMove(movedFrom, movedTo) && movedFrom < splinePoints.size()
                && movedTo < splinePoints.size() && movedFrom != movedTo) {
                WaterSplinePoint movedPoint = std::move(splinePoints[movedFrom]);
                splinePoints.erase(splinePoints.begin() + static_cast<std::ptrdiff_t>(movedFrom));
                splinePoints.insert(splinePoints.begin() + static_cast<std::ptrdiff_t>(movedTo), std::move(movedPoint));
            }
        }
        int samplesPerSegment = static_cast<int>((std::clamp)(splineSamplesPerSegment, 1u,
            WATER_SPLINE_MAX_SAMPLES_PER_SEGMENT));
        r.Field("splineSamplesPerSegment", samplesPerSegment);
        splineSamplesPerSegment = static_cast<std::uint32_t>((std::clamp)(samplesPerSegment, 1,
            static_cast<int>(WATER_SPLINE_MAX_SAMPLES_PER_SEGMENT)));
        r.FloatRange("splineMeshCellSize", splineMeshCellSize, 0.05f, 100.0f);
        r.Field("materialPath", materialPath);
        r.Field("wavesAssetRef", wavesAssetRef);
        r.Field("enableGerstnerWaves", enableGerstnerWaves);
        r.Field("waveAmplitudeScale", waveAmplitudeScale);
        r.Field("buoyancyEnabled", buoyancyEnabled);
        r.Field("buoyancy", buoyancy);
        r.Field("waterDrag", waterDrag);
        r.Field("buoyancyDepth", buoyancyDepth);
        r.Field("waveAttenuationDepth", waveAttenuationDepth);
        r.Field("splashEnabled", splashEnabled);
        r.BeginObject("interactionSettings");
        interactionSettings.Reflect(r);
        r.EndObject();

        resolutionX = static_cast<uint32_t>((std::clamp)(resX, 1, 512));
        resolutionZ = static_cast<uint32_t>((std::clamp)(resZ, 1, 512));
        chunkCount  = static_cast<uint32_t>((std::clamp)(chunks, 1, 64));
        lodPatchResolution = 2;
        while (lodPatchResolution < static_cast<uint32_t>((std::clamp)(patchResolution, 2, 64)))
            lodPatchResolution *= 2;
        lodMaxDepth = static_cast<uint32_t>((std::clamp)(maxDepth, 0, 10));
        lodMaxPatches = static_cast<uint32_t>((std::clamp)(maxPatches, 1, 4096));
        lodTargetPixels = (std::clamp)(lodTargetPixels, 1.0f, 64.0f);
        lodNearDistance = (std::clamp)(lodNearDistance, 0.0f, 10000.0f);
        lodNearCellSize = (std::clamp)(lodNearCellSize, 0.25f, 100.0f);
        lodMorphTransitionWidth = std::isfinite(lodMorphTransitionWidth)
            ? (std::clamp)(lodMorphTransitionWidth, 0.05f, 1.0f) : 0.25f;
        farOceanCoverageScale = std::isfinite(farOceanCoverageScale)
            ? (std::clamp)(farOceanCoverageScale, 1.0f, 2.0f) : 1.05f;
        farOceanRippleFadeDistance = std::isfinite(farOceanRippleFadeDistance)
            ? (std::clamp)(farOceanRippleFadeDistance, 0.1f, 1000.0f) : 4.0f;
        extentX = (std::clamp)(extentX, 0.1f, 10000.0f);
        extentZ = (std::clamp)(extentZ, 0.1f, 10000.0f);
        splineMeshCellSize = std::isfinite(splineMeshCellSize)
            ? (std::clamp)(splineMeshCellSize, 0.05f, 100.0f) : 2.0f;
        nearCellSize = (std::clamp)(nearCellSize, 0.25f, 10.0f);
        waveAmplitudeScale = (std::max)(waveAmplitudeScale, 0.0f);
        buoyancyDepth      = std::isfinite(buoyancyDepth) ? (std::max)(buoyancyDepth, 0.0f) : 0.0f;
        waveAttenuationDepth = std::isfinite(waveAttenuationDepth)
            ? (std::max)(waveAttenuationDepth, 0.0f) : 0.0f;
        if (resolutionX != oldResolutionX || resolutionZ != oldResolutionZ ||
            chunkCount  != oldChunkCount  || extentX      != oldExtentX      ||
            extentZ     != oldExtentZ || adaptivePatchLod != oldAdaptivePatchLod || farOceanEnabled != oldFarOceanEnabled ||
            lodPatchResolution != oldPatchResolution) {
            meshDirty = true;
            foamDirty = true;
        }
        if (materialPath != oldMaterialPath) {
            texDirty = true;
            foamDirty = true;
        }
        if (surfaceShape != oldSurfaceShape || splineSamplesPerSegment != oldSplineSamplesPerSegment
            || splineMeshCellSize != oldSplineMeshCellSize
            || splinePoints != oldSplinePoints || pointSourceMode != oldPointSourceMode
            || pointRuntimeMode != oldPointRuntimeMode || pointBindings != oldPointBindings
            || lakePlaneHeight != oldLakePlaneHeight || bakedSplinePoints != oldBakedSplinePoints
            || bakedInputSignature != oldBakedInputSignature || bakedSourceMode != oldBakedSourceMode
            || bakedSurfaceShape != oldBakedSurfaceShape || bakedLakePlaneHeight != oldBakedLakePlaneHeight
            || bakedAdapterVersion != oldBakedAdapterVersion || bakeValid != oldBakeValid) {
            meshDirty = true;
            foamDirty = true;
        }
    }
    /// @}
};

} /// @note namespace fbzz::scene
