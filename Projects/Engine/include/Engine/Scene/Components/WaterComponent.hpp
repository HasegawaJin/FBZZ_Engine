/// @file    WaterComponent.hpp
/// @brief   水面コンポーネント（ジオメトリ・水の種類 (.mat) の参照・浮力・着水）。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// @brief 水の «種類» ── 色・さざ波・Gerstner 波・風への反応・水流 ── は materialPath が指す
/// @brief .mat が丸ごと持つ。コンポーネントに残すのは «この 1 枚» ごとに違う量だけ。
/// @note 波までコンポーネント側に残すと、色だけ .mat にあり波が別にある食い違いが起きる
///       (Ocean.mat を差しても湖の波のまま等)。水の種類は 1 ファイルで決まるべき。
#pragma once

#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Script.hpp>
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
#include <string>
#include <vector>

namespace fbzz::scene {

/// @brief GerstnerWave — 水面を構成する 1 本の波のパラメータ。
/// @note 水面全体をシミュレーションせず、少数の解析波を合成して低コストに大きなうねりを表現する。
struct GerstnerWave {
    math::Vector2 direction  = { 1.0f, 0.0f }; ///< @brief 進行方向。正規化は評価側で行う。
    float         amplitude  = 0.30f;          ///< @brief 波高 A [m]
    float         wavelength = 10.0f;          ///< @brief 波長 lambda [m]
    float         steepness  = 0.50f;          ///< @brief 急峻度 Q [0,1]。1 を超えると波面が交差する。
};

/// @brief 水面 1 枚が «形と質感» として受け取る流れの本数。WaterCB の枠数と揃えること。
inline constexpr int kWaterSurfaceFlowCount = 8;

/// @brief 流れが水面の形へ出せる変位の上限 [m]。穴にも盛り上がりにも同じ値で効く。
/// @note 見た目の歯止めで物理的な意味は無い。Bernoulli をそのまま採ると speed 10 m/s が
///       5 m の穴を掘り、水面が裏返って «底» が見える。
inline constexpr float kMaxWaterSurfaceDisplacement = 3.0f;

/// @brief 渦の軸が «鉛直» と認める、軸と上向きの内積の下限。
/// @note 45 度 (0.707) をわずかに下回るところ。これより寝た渦は水面を掘らず撫でるだけで、
///       高さの解析項として書けない。Inspector の注意書きも同じ値を見る。
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
///          スロットへ新しい実体を差し込むため、古いポインタは黙って別の場を指す。
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
///          見えている輪と浮力が別の高さになる。
inline constexpr float kWaterRippleHeightScale = 0.5f;

/// @brief 生きている着水の輪 1 つ。**ワールド座標で持つ** (保存しない)。
///
/// @note 正本を WaterComponent へ置く理由: 描画パスの静的キャッシュに置くと SceneView と
///       GameView で寿命が 2 回進み、さらに CPU 側の GetSurfaceHeightAt が読めない。
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
    /// @brief チャンク分割数。水面を chunkCount×chunkCount のサブメッシュに分割し、
    /// @brief チャンク単位でフラスタムカリングする。
    uint32_t chunkCount = 4;
    /// @}

    /// @name 波 (個体ごとの補正。波そのものは .mat が持つ)
    /// @{
    bool  enableGerstnerWaves = true;
    /// @brief .mat の振幅に掛ける倍率。同じ Ocean.mat を «凪の入り江» と «外洋» に使い分けるため。
    float waveAmplitudeScale  = 1.0f;
    /// @}

    /// @name 物理
    /// @{
    bool  buoyancyEnabled = true;
    /// @brief 完全に沈んだときの上向き加速度 [m/s^2]。重力 (9.8) を超えると浮く。
    float buoyancy        = 15.0f;
    /// @brief 水中での速度減衰 [1/s]。水流があるときは «水に対する» 速度に掛かる。
    float waterDrag       = 2.0f;
    /// @brief 浮力が届く水面からの深さ [m]。
    /// @note 水面は厚みを持たない板なので、上限が無いと «水面の真下にある洞窟» の中まで浮力が届く。
    float buoyancyDepth   = 10.0f;
    /// @brief 剛体が水面を通過したときに波紋としぶきを出す。
    bool  splashEnabled   = true;
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
    /// @brief WaterSystem が毎フレーム «.mat の波 × 倍率 × 環境風» から組み立てた実効波。
    /// @brief 描画・浮力・水中判定・スクリプトはすべてこれを読む。
    std::array<GerstnerWave, 4> waves = {};
    /// @brief 水流の速度 [m/s] (ワールド XZ)。.mat の flowDirection × currentSpeed。
    math::Vector2 current = math::Vector2::ZERO;
    /// @brief WaterSystem が毎フレーム書く «頂点グリッド 1 セルのワールド実寸» [m]。
    /// @note 刻めない波長の Gerstner 波を寝かせる判断は、描画 (GPU) と浮力 (CPU) が同じ値を
    ///       使う必要がある。片方だけ寝かせると平らな水面の上で物が揺れる。
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
    physics::VolumeHandle volumeHandle;

    const char* GetTypeName() const { return "Water"; }

    /// @brief 頂点グリッドで «刻めない» 波を寝かせる係数 [0,1]。Water.hlsl の WaveMeshFade と同じ式。
    /// @note Gerstner 波は頂点でしか評価されないため、1 波長あたり数セルしか取れない波は山と谷が
    ///       セル境界で入れ替わり «別の波» に化ける。刻めない波は消し、細かさは手続きさざ波に任せる。
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
    ///       斜めにずらすと包絡が波頭に沿っても変化し、うねりが塊へ割れる。
    static constexpr float WAVE_GROUP_COS = 0.852525f;
    static constexpr float WAVE_GROUP_SIN = 0.522687f;

    /// @brief 波の «群» の包絡 [1-depth, 1+depth]。振幅にそのまま掛ける。
    /// @note 正弦を 4 本足しただけでは全波頭が同じ高さ・形になる。実海面は近い周波数の «うなり»
    ///       で波が群れ、大きい塊と凪の区間が交互に来る。
    /// @param index 波の番号 [0,3]。群の波長と傾ける向きをここから決める (CB を増やさないため)。
    /// @note 群の角周波数を r*omega/2 にするのは、深水波の群速度が位相速度の 1/2 だから。
    ///       これで «群はゆっくり進み、個々の波頭がその中を追い越していく» 見え方になる。
    /// @warning Water.hlsl の WaveGroupEnvelope と同じ式であること。片方だけ変えると、
    ///          見えている波と浮力が別の水面になる。
    static float WaveGroupEnvelope(int index, math::Vector2 direction, float k, float omega,
                                   float worldX, float worldZ, float time, float depth)
    {
        if (depth <= 0.0001f) return 1.0f;
        const float ratio = 0.09f + 0.035f * static_cast<float>(index);
        const float sign  = (index & 1) ? -1.0f : 1.0f;
        const math::Vector2 groupDir = {
            direction.x * WAVE_GROUP_COS - direction.y * sign * WAVE_GROUP_SIN,
            direction.x * sign * WAVE_GROUP_SIN + direction.y * WAVE_GROUP_COS
        };
        const float groupK = ratio * k;
        const float phase = groupK * (groupDir.x * worldX + groupDir.y * worldZ)
                          - ratio * omega * 0.5f * time;
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
    ///       重ねると、波頭が有限の長さに切れる (short-crested sea)。
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
    ///        逆写像の対象ではなく、入れると Gerstner の位相まで穴のぶんずれる。
    /// @note 形を Gaussian にするのは C¹ 連続だから。(1 - r/R)^p は r = R で折れ、頂点法線が
    ///       1 セルだけ跳ねて縁に輪が出る。
    /// @warning Water.hlsl の WaterRadialDisplacement と同じ式であること。片方だけ変えると、
    ///          見えている形と浮力・水中判定が別の水面になる。
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
    ///       淀みの深さとしては二重に数えることになる。
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
    ///          片方だけ変えると、見えている輪の上で物が別の高さに浮く。
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

    /// @brief 輪 1 つが頂点へ乗せる高さ [m]。刻めない輪は meshFade が 0 にする。
    static float WaterRippleHeight(const WaterRipple& ripple, float worldX, float worldZ)
    {
        if (ripple.meshFade <= 0.0f || ripple.amplitude <= 0.0f) return 0.0f;
        const float dx = worldX - ripple.center.x;
        const float dz = worldZ - ripple.center.y;
        return WaterRippleWave(ripple, std::sqrt(dx * dx + dz * dz))
             * ripple.amplitude * ripple.meshFade * kWaterRippleHeightAmplitude;
    }

    /// @brief 逆写像の反復回数。
    /// @note 減衰を波の急峻度から決めるので、収束する設定 (Σ Q·k·A ≤ 1) では素の不動点反復に
    ///       なり、4 回で残差が mm 台へ入る。
    static constexpr int SURFACE_SOLVE_ITERATIONS = 4;

    struct ResolvedWave {
        math::Vector2 direction;
        float k = 0.0f;
        float omega = 0.0f;
        float amplitude = 0.0f;
        float horizontalAmplitude = 0.0f;
        int index = 0;
    };

    /// @note 保存しない。公開入力の直接編集も検出し、同一設定では超越関数を再評価しない。
    void PrepareWaveCache() const
    {
        bool same = m_waveCacheValid && m_cachedCell.x == cellSize.x && m_cachedCell.y == cellSize.y
                 && m_cachedSpread == waveSpread && m_cachedGrouping == waveGrouping;
        for (size_t i = 0; same && i < waves.size(); ++i) {
            const auto& a = waves[i];
            const auto& b = m_cachedWaves[i];
            same = a.direction.x == b.direction.x && a.direction.y == b.direction.y
                && a.amplitude == b.amplitude && a.wavelength == b.wavelength && a.steepness == b.steepness;
        }
        if (same) return;
        m_cachedWaves = waves;
        m_cachedCell = cellSize;
        m_cachedSpread = waveSpread;
        m_cachedGrouping = waveGrouping;
        m_resolvedCount = 0;
        m_contraction = 0.0f;
        m_waveHeightBound = 0.0f;
        const float peak = (1.0f + math::Clamp01(waveGrouping)) * WaveSpreadAmplitudeSum(waveSpread);
        for (int i = 0; i < 4; ++i) {
            const auto& wave = waves[static_cast<size_t>(i)];
            if (wave.amplitude < 0.0001f || wave.wavelength <= 0.0001f) continue;
            const float fade = WaveMeshFade(wave.wavelength, cellSize);
            const float k = math::TWO_PI / wave.wavelength;
            m_contraction += math::Clamp01(wave.steepness) * wave.amplitude * fade * peak * k;
            if (fade <= 0.0f) continue;
            const float omega = std::sqrt(9.8f * k);
            const math::Vector2 direction = wave.direction.Normalized();
            for (int component = 0; component < 2; ++component) {
                const auto part = WaveSpreadComponent(i, component, direction, waveSpread);
                if (part.amplitudeScale <= 0.001f) continue;
                auto& resolved = m_resolvedWaves[m_resolvedCount++];
                resolved = { part.direction, k, omega, wave.amplitude * fade * part.amplitudeScale,
                    math::Clamp01(wave.steepness) * wave.amplitude * fade * part.amplitudeScale, i };
                m_waveHeightBound += std::abs(resolved.amplitude) * (1.0f + std::abs(waveGrouping));
            }
        }
        m_waveCacheValid = true;
    }

    /// @return 基準面からの絶対変位の保守的上限 [m]。時刻に依存しない。
    float SurfaceHeightBound() const
    {
        PrepareWaveCache();
        float bound = enableGerstnerWaves ? m_waveHeightBound : 0.0f;
        for (int i = 0; i < (std::min)(surfaceFlowCount, kWaterSurfaceFlowCount); ++i)
            bound += std::abs(surfaceFlows[static_cast<size_t>(i)].height);
        for (const auto& ripple : ripples)
            if (ripple.meshFade > 0.0f && ripple.amplitude > 0.0f)
                bound += ripple.amplitude * ripple.meshFade * kWaterRippleHeightAmplitude * 1.58f;
        /// @note 加算と三角関数の丸めが境界で早期判定を反転させないよう外側へ広げる。
        return bound + (std::max)(1.0e-5f, bound * 1.0e-5f);
    }

private:
    mutable bool m_waveCacheValid = false;
    mutable std::array<GerstnerWave, 4> m_cachedWaves{};
    mutable math::Vector2 m_cachedCell{};
    mutable float m_cachedSpread = 0.0f;
    mutable float m_cachedGrouping = 0.0f;
    mutable std::array<ResolvedWave, 8> m_resolvedWaves{};
    mutable size_t m_resolvedCount = 0;
    mutable float m_contraction = 0.0f;
    mutable float m_waveHeightBound = 0.0f;

    math::Vector2 ResolvedHorizontalDisplacementAt(math::Vector2 point, float time) const
    {
        math::Vector2 offset = math::Vector2::ZERO;
        for (size_t i = 0; i < m_resolvedCount; ++i) {
            const auto& wave = m_resolvedWaves[i];
            const auto dir = wave.direction;
            const float envelope = WaveGroupEnvelope(wave.index, dir, wave.k, wave.omega,
                                                      point.x, point.y, time, waveGrouping);
            const float phase = wave.k * (dir.x * point.x + dir.y * point.y) - wave.omega * time;
            const float push = wave.horizontalAmplitude * envelope * std::cos(phase);
            offset.x += dir.x * push;
            offset.y += dir.y * push;
        }
        return offset;
    }

public:

    /// @brief Gerstner 波の水平変位 [m] (ワールド XZ)。
    /// @param undisplaced 変位«前»の水平位置。波の位相はここで取る。
    math::Vector2 HorizontalDisplacementAt(math::Vector2 undisplaced, float time) const
    {
        if (!enableGerstnerWaves) return math::Vector2::ZERO;
        PrepareWaveCache();
        return ResolvedHorizontalDisplacementAt(undisplaced, time);
    }

    /// @brief ワールド XZ に «水面が来ている» とき、その点の変位前の水平位置を解く。
    /// @note Gerstner 波は «変位前の位置» を位相の入力に取る。ワールド XZ をそのまま使うと水平変位分
    ///       だけ別の場所の高さを読み、急な斜面では Q·A の合計 (Ocean.mat で 1〜2m) ずれる。
    /// @warning 折り畳んだ波では逆写像が一意でない (同じ XZ に複数の水面)。反復はその枝の 1 つへ
    ///          落ちる。初期値依存で連続性は保証しない。
    math::Vector2 SolveUndisplacedXZ(float worldX, float worldZ, float time) const
    {
        const math::Vector2 target = { worldX, worldZ };
        if (!enableGerstnerWaves) return target;

        /// @note 不動点反復の縮小率の上界 Σ Q·k·A。三角関数を 1 度も回さずに出せる。
        ///       群の包絡は最大 (1 + waveGrouping) 倍、方向広がりは主 + 伴走の和まで振幅を持ち上げる。
        PrepareWaveCache();
        const float contraction = m_contraction;
        /// @note 水平変位を持たない水面 (湖・プール・急峻度 0) は解く必要がない。
        if (contraction <= 0.0001f) return target;

        /// @note Σ Q·k·A ≤ 1 なら素の反復がそのまま縮小写像。超える («波が折り畳む») 設定では
        ///       その比で減衰させ、有限回で暴れないところへ抑える。
        const float relax = 1.0f / (std::max)(contraction, 1.0f);

        math::Vector2 guess = target;
        for (int iteration = 0; iteration < SURFACE_SOLVE_ITERATIONS; ++iteration) {
            const math::Vector2 offset = ResolvedHorizontalDisplacementAt(guess, time);
            guess.x += (target.x - (guess.x + offset.x)) * relax;
            guess.y += (target.y - (guess.y + offset.y)) * relax;
        }
        return guess;
    }

    /// @brief 水面の基準面からの高さ [m] を CPU で評価する。
    /// @param worldX, worldZ ワールド座標 (変位«後»、つまり画面に出ている位置)。
    /// @note シェーダーは波の位相をワールド XZ で取るため、原点からの相対座標で評価すると
    ///       水面を原点以外へ置いた瞬間に浮力・水中判定と描画の波がずれる。
    /// @note 流れの場が作る形と着水の輪も足す。中心型はワールド XZ、Baked と輪は変位前の XZ で
    ///       引く (VS が波紋テクスチャを «波を乗せる前の» 頂点 UV で引いているため)。
    /// @see SolveUndisplacedXZ
    float GetSurfaceHeightAt(float worldX, float worldZ, float time) const
    {
        float height = 0.0f;
        math::Vector2 origin = { worldX, worldZ };

        if (enableGerstnerWaves) {
            origin = SolveUndisplacedXZ(worldX, worldZ, time);
            for (size_t i = 0; i < m_resolvedCount; ++i) {
                const auto& wave = m_resolvedWaves[i];
                const auto dir = wave.direction;
                const float envelope = WaveGroupEnvelope(wave.index, dir, wave.k, wave.omega,
                                                         origin.x, origin.y, time, waveGrouping);
                const float phase = wave.k * (dir.x * origin.x + dir.y * origin.y) - wave.omega * time;
                height += wave.amplitude * envelope * std::sin(phase);
            }
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
        const float oldExtentX = extentX;
        const float oldExtentZ = extentZ;
        const std::string oldMaterialPath = materialPath;

        /// @note IReflector は uint32_t 非対応のため int 経由で編集・保存する。
        int resX = static_cast<int>((std::clamp)(resolutionX, 1u, 512u));
        int resZ = static_cast<int>((std::clamp)(resolutionZ, 1u, 512u));
        int chunks = static_cast<int>((std::clamp)(chunkCount, 1u, 64u));
        r.Field("resolutionX", resX);
        r.Field("resolutionZ", resZ);
        r.Field("chunkCount", chunks);
        r.Field("extentX", extentX);
        r.Field("extentZ", extentZ);
        r.Field("materialPath", materialPath);
        r.Field("enableGerstnerWaves", enableGerstnerWaves);
        r.Field("waveAmplitudeScale", waveAmplitudeScale);
        r.Field("buoyancyEnabled", buoyancyEnabled);
        r.Field("buoyancy", buoyancy);
        r.Field("waterDrag", waterDrag);
        r.Field("buoyancyDepth", buoyancyDepth);
        r.Field("splashEnabled", splashEnabled);

        resolutionX = static_cast<uint32_t>((std::clamp)(resX, 1, 512));
        resolutionZ = static_cast<uint32_t>((std::clamp)(resZ, 1, 512));
        chunkCount  = static_cast<uint32_t>((std::clamp)(chunks, 1, 64));
        extentX = (std::clamp)(extentX, 0.1f, 10000.0f);
        extentZ = (std::clamp)(extentZ, 0.1f, 10000.0f);
        waveAmplitudeScale = (std::max)(waveAmplitudeScale, 0.0f);
        buoyancyDepth      = (std::max)(buoyancyDepth, 0.1f);
        if (resolutionX != oldResolutionX || resolutionZ != oldResolutionZ ||
            chunkCount  != oldChunkCount  || extentX      != oldExtentX      ||
            extentZ     != oldExtentZ) {
            meshDirty = true;
            foamDirty = true;
        }
        if (materialPath != oldMaterialPath) {
            texDirty = true;
            foamDirty = true;
        }
    }
    /// @}
};

} // namespace fbzz::scene
