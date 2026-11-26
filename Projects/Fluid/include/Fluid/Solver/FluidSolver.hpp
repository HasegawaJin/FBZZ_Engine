/// @file    FluidSolver.hpp
/// @brief   レシピを解く 2 つのソルバー — 格子の気体 (Stable Fluids) と粒子の液体 (PBF)
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note どちらも決定論的な CPU 実装 (同じレシピ・同じ seed なら同じ結果)。今の主な客は
/// @note エディターの «焼き» だが、依存は Math だけなので実行時に回しても同じものが動く。
/// @note 入力はレシピの設定型だけで、.fluid というファイルのことは知らない
/// @note (読み書きは Engine の FluidRecipeCodec)。
/// @see Docs/design/fluid-library.md
#pragma once
#include <Fluid/Api.hpp>

#include <Fluid/Recipe/FluidRecipe.hpp>
#include <Fluid/Operators/FluidSourceMask.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::fluid {

/// @brief セル中心に速度とスカラー量を持つ気体ソルバー。nz = 1 で 2D、nz > 1 で 3D。
/// @note 速度の単位は領域単位/秒。最も長い軸を [-1,1] とし、セル幅は全軸で共通。
class FluidGasSolver {
public:
    FBZZ_FLUID_API void Reset(const FluidRecipe& recipe, int nx, int ny, int nz);
    /// @brief 場・時刻はそのままに、部品 (発生源・力・障害物) とその見た目に効かない設定だけを差し替える。
    /// @brief 解き直しの途中から «変えた部品で続ける» ために使う (Fluid Editor のプレビュー)。
    /// @brief 格子の意味が変わるレシピ (kind・resolution・seed など) では false を返す — 呼び手は Reset し直すこと。
    [[nodiscard]] FBZZ_FLUID_API bool ReplaceOperators(const FluidRecipe& recipe);
    /// @brief dt 秒ぶん進める。1 刻みで 2 セル以上動かないよう内部で刻みを割る (CFL)。
    FBZZ_FLUID_API void Advance(float dt);
    /// @brief 刻みを割らずに 1 回だけ進める。
    FBZZ_FLUID_API void Step(float dt);
    /// @brief 圧力投影で速度場の発散を目標値へ近づける。
    /// @param[in] expansion セルごとの目標発散 [1/秒]。燃焼による膨張を残すために使い、nullptr なら 0。
    /// @pre expansion を指定する場合は速度場と同じ並びで全セル分を与える。
    FBZZ_FLUID_API void Project(const std::vector<float>* expansion = nullptr);

    [[nodiscard]] int   SizeX() const { return m_cellsX; }
    [[nodiscard]] int   SizeY() const { return m_cellsY; }
    [[nodiscard]] int   SizeZ() const { return m_cellsZ; }
    [[nodiscard]] float CellSize() const { return m_cellWidth; }
    [[nodiscard]] float Time() const { return m_time; }
    [[nodiscard]] std::size_t Index(int x, int y, int z) const
    {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(m_cellsY) + static_cast<std::size_t>(y))
            * static_cast<std::size_t>(m_cellsX) + static_cast<std::size_t>(x);
    }

    /// @brief 領域座標での値 (範囲外は縁の値)。2D では z を無視する。
    [[nodiscard]] FBZZ_FLUID_API float SampleDensity(float x, float y, float z = 0.0f) const;
    [[nodiscard]] FBZZ_FLUID_API float SampleTemperature(float x, float y, float z = 0.0f) const;
    FBZZ_FLUID_API void SampleVelocity(float x, float y, float z, float& outX, float& outY, float& outZ) const;
    /// @brief 色の鍵 [0,1] (FluidSource::colorKey を煙の量で重み付けした平均)。煙がほぼ無い所は 0。
    /// @note 鍵そのものでなく「鍵 × 密度」を運ぶ理由: 鍵を直接補間すると、煙の無いセルの 0 と混ざって
    /// @note 薄い縁ほど鍵が 0 へ引っ張られる。量で重み付けすれば 2 つの煙が混ざったときの平均にもなる。
    [[nodiscard]] FBZZ_FLUID_API float SampleColorKey(float x, float y, float z = 0.0f) const;

    /// @brief 中心差分で測った発散の最大値 [1/秒]。投影の効き具合の確認用。障害物の中のセルは数えない。
    [[nodiscard]] FBZZ_FLUID_API float MaxDivergence() const;
    [[nodiscard]] FBZZ_FLUID_API float TotalDensity() const;

    [[nodiscard]] const std::vector<float>& Density() const { return m_density; }
    [[nodiscard]] const std::vector<float>& Temperature() const { return m_temperature; }
    [[nodiscard]] const std::vector<float>& VelocityX() const { return m_vx; }
    [[nodiscard]] const std::vector<float>& VelocityY() const { return m_vy; }
    [[nodiscard]] const std::vector<float>& VelocityZ() const { return m_vz; }
    /// @brief セルごとの色の鍵 [0,1] (刻みの終わりに求め直す)。鍵の付いた発生源が無ければ全部 0。
    [[nodiscard]] const std::vector<float>& ColorKey() const { return m_colorKey; }
    std::vector<float>& Density() { return m_density; }
    std::vector<float>& Temperature() { return m_temperature; }
    std::vector<float>& VelocityX() { return m_vx; }
    std::vector<float>& VelocityY() { return m_vy; }
    std::vector<float>& VelocityZ() { return m_vz; }

    /// @name 流れに乗せた細部 (2D のときだけ持つ)
    /// @{
    /// @note 格子より細かい起伏は解かずにノイズで描く。ノイズを「座標ごと流れで運ぶ」と煙と一緒に動くが、
    /// @note 運び続けると引き伸ばされて筋になる。2 層を半周期ずらして初期位置へ戻し、戻す瞬間の層の
    /// @note 重みを 0 にして混ぜると、流れに沿ったまま入れ替わる (Neyret 2003, Advected Textures)。

    [[nodiscard]] bool HasDetail() const { return !m_detailU[0].empty(); }
    /// @brief layer (0/1) の座標 [領域単位] を領域座標 (x, y) で引く。
    FBZZ_FLUID_API void SampleDetailCoordinate(int layer, float x, float y, float& outU, float& outV) const;
    /// @brief 2 層の重み。和は常に 1 で、層を初期位置へ戻す瞬間はその層が 0。
    FBZZ_FLUID_API void DetailWeights(float& outLayer0, float& outLayer1) const;

private:
    [[nodiscard]] FBZZ_FLUID_API float CellCenter(int index, int count) const;
    [[nodiscard]] FBZZ_FLUID_API float ToGrid(float position, int count) const;
    [[nodiscard]] FBZZ_FLUID_API float SampleGrid(const std::vector<float>& field, float gx, float gy, float gz) const;
    FBZZ_FLUID_API void MinMaxAround(const std::vector<float>& field, float gx, float gy, float gz,
                      float& outMin, float& outMax) const;
    [[nodiscard]] FBZZ_FLUID_API float Derivative(const std::vector<float>& field, int x, int y, int z, int axis) const;
    [[nodiscard]] FBZZ_FLUID_API float Divergence(int x, int y, int z) const;

    FBZZ_FLUID_API void AdoptOperators(const FluidRecipe& recipe);
    FBZZ_FLUID_API void BuildSolids();
    FBZZ_FLUID_API void EnforceSolids(bool clearScalars);
    FBZZ_FLUID_API void Inject(float dt);
    FBZZ_FLUID_API void Burn(float dt);
    FBZZ_FLUID_API void ApplyForces(float dt);
    FBZZ_FLUID_API void ApplyVorticityConfinement(float dt);
    void BuildPressureRhs(const std::vector<float>* expansion);
    void SolvePressure();
    void SubtractPressureGradient();
    FBZZ_FLUID_API void Advect(float dt);
    FBZZ_FLUID_API void AdvectScalar(std::vector<float>& field, float dt);
    FBZZ_FLUID_API void Dissipate(float dt);
    FBZZ_FLUID_API void UpdateColorKey();
    FBZZ_FLUID_API void ResetDetailLayer(int layer);
    FBZZ_FLUID_API void AdvectDetail(float dt);
    FBZZ_FLUID_API void UpdateDetailEpochs();

    FluidGasSettings            m_settings; ///< @note 圧力反復回数・散逸率など、各ステップで参照する気体の設定。
    std::vector<FluidSource>    m_sources; ///< 有効な部品だけを先頭から上限数まで持つ。ノイズの切り出し位置は「有効な部品の中での添字」で 決まり、GPU (有効な部品だけを詰める) と同じ数え方になる。
    std::vector<FluidForce>     m_forces; ///< @note この格子の速度に作用する外力。
    std::vector<FluidCollider>  m_colliders; ///< @note セルのうち流れを遮る領域を決める障害物。
    std::vector<FluidSourceMask> m_sourceMasks; ///< m_sources と同じ並び。Texture 以外と読めなかった画像は空 (= 1 として引かれ、板の形に湧く)。
    math::Vector3 m_noiseOffset = { 0.0f, 0.0f, 0.0f }; ///< 同じ種から生成し、ノイズの評価座標へ加えるオフセット。
    /// @note ReplaceOperators が「同じ格子の続き」と言えるかを判じるための、Reset したレシピの控え
    /// @note (seed = ノイズの切り出し位置、要求解像度 = 2D は ResolveGasResolution・3D は vectorFieldResolution)。
    uint32_t m_seed = 1; ///< 気体のノイズを再現するために保持する現在のレシピの種。
    int   m_requestedResolution = 0; ///< @note 格子の再利用が可能かをレシピ変更時に判定するための要求解像度。
    int   m_cellsX = 1; ///< @note 横方向のセル数。X が配列上で隣り合う。
    int   m_cellsY = 1; ///< @note 縦方向のセル数。隣の行への添字計算に使う。
    int   m_cellsZ = 1; ///< @note 奥行き方向のセル数。2 次元では 1 層だけ使う。
    float m_cellWidth = 1.0f; ///< @note 隣り合うセル中心間の距離。速度や圧力の差を領域座標での変化率に換算する。
    float m_time = 0.0f; ///< @note 発生源や動く障害物の位置を決めるシミュレーション時刻（秒）。

    std::vector<float> m_density; ///< @note セルごとの煙の濃さ。速度場に乗せて運ぶ。
    std::vector<float> m_temperature; ///< @note 浮力や燃焼に使うセルごとの温度。
    std::vector<float> m_fuel; ///< @note 燃焼によって熱や煙に変わるセルごとの残存燃料。
    /// @note 色の量 (密度 × 鍵)。密度と同じ移流・散逸で運ぶ。鍵の付いた発生源が 1 つも無いレシピは
    /// @note m_hasColor = false で運ぶのを飛ばす (鍵は全部 0 のまま。密度には元から触らない)。
    std::vector<float> m_colorMass; ///< セルごとの煙密度と色キーの積。煙とともに移流する。
    /// @note 燃料の色の量 (燃料 × 鍵)。燃料と同じに扱う (注入・移流・障害物で 0。散逸はしない)。
    /// @note 煙と別に持つ理由: 煙を注がない発生源 (Fire プリセット = density 0・fuel > 0) では、
    /// @note 燃えて生まれる煤が「その場の煙の鍵」しか受け取れず、色の付いた炎が作れなかった。
    std::vector<float> m_fuelColorMass; ///< セルごとの燃料密度と色キーの積。着火時に煙の色へ移す。
    std::vector<float> m_colorKey; ///< @note 描画で色を選ぶため、色の量を煙の密度で割り戻した値。
    bool m_hasColor = false; ///< 色キー付きの発生源が存在し、色質量を追跡するかどうか。
    std::vector<float> m_vx; ///< セル中心の流速を X・Y・Z 成分別に保持する。圧力で補正した流速を使って煙と熱を運ぶ。
    std::vector<float> m_vy; ///< 現在のセル速度の Y 成分。
    std::vector<float> m_vz; ///< 現在のセル速度の Z 成分。
    std::vector<float> m_pressure; ///< @note 余分な膨張を打ち消す圧力。前の刻みの結果を次の反復の初期値に使う。
    std::vector<float> m_excessDivergence; ///< @note セルから出る流れの広がりから、燃焼で残したい膨張を引いた値。
    std::vector<float> m_expansion; ///< @note 燃焼によって許す局所的な膨張率。圧力計算の目標にする。
    std::vector<float> m_prevVx; ///< 移流・渦度の作業領域。刻みごとに確保し直さない。
    std::vector<float> m_prevVy; ///< 速度の移流に使う、移流前の Y 成分。
    std::vector<float> m_prevVz; ///< 速度の移流に使う、移流前の Z 成分。
    std::vector<float> m_scratchA; ///< 移流に使う 3 枚の作業格子。スカラーの移流では順に片道の結果・往復した結果・補正後の結果を置く。
    std::vector<float> m_scratchB; ///< 移流や細部の計算で使い回す 2 枚目のセル一時配列。
    std::vector<float> m_scratchC; ///< 移流や細部の計算で使い回す 3 枚目のセル一時配列。
    std::vector<float> m_curlX; ///< セルごとの速度の回転の X 成分。
    std::vector<float> m_curlY; ///< セルごとの速度の回転の Y 成分。
    std::vector<float> m_curlZ; ///< セルごとの速度の回転の Z 成分。
    std::vector<float> m_vorticityMagnitude; ///< @note 渦の強さ。隣のセルとの強さの差から、細かな渦を補う力の向きを決める。
    /// @note 障害物のセル (1 = 固体) と、そのセルの速度 (障害物の動きの速度。重なったら先の障害物)。
    /// @note 刻みの始めに BuildSolids が作り直す。効いている障害物が無い刻みは m_hasSolid = false で
    /// @note 全部飛ばす (障害物の無いレシピの結果を 1 ビットも変えない)。
    std::vector<uint8_t> m_solid; ///< 障害物に覆われているセルを非 0 で示すマスク。
    std::vector<float> m_solidVx; ///< 固体セルで流体速度に書き戻す障害物速度の X 成分。
    std::vector<float> m_solidVy; ///< 固体セルで流体速度に書き戻す障害物速度の Y 成分。
    std::vector<float> m_solidVz; ///< 固体セルで流体速度に書き戻す障害物速度の Z 成分。
    bool m_hasSolid = false; ///< 今の刻みで固体セルの処理が必要かどうか。
    std::vector<float> m_detailU[2]; ///< 流れに乗せた細部の座標 (2 層)。3D では空のまま。
    std::vector<float> m_detailV[2]; ///< 移流する細部用テクスチャ座標の V 成分。添字は 2 層から選ぶ。
    int m_detailEpoch[2] = { 0, 0 }; ///< @note 各層の座標を初期位置へ戻す周期の番号。同じ周期での二重リセットを防ぐ。
};

/// @brief 液体の正規化座標で取得した障害物表面との接触情報。
/// @note distance は固体の外側で正。normal は外側を向く長さ 1 のベクトル。
struct FluidLiquidContact {
    math::Vector3 normal = math::Vector3::UP; ///< 境界の外側へ向く局所法線。
    float distance = 0.0f; ///< 境界までの符号付き距離。負なら内側。
    float restitution = 0.0f; ///< 境界から離れる速度を決める反発係数。
};

struct FluidLiquidBoundary {
    const void* context = nullptr; ///< 境界の問い合わせに渡す非所有の呼び出し元データ。
    bool (*sample)(const void*, const math::Vector3&, float, FluidLiquidContact&) = nullptr; ///< 粒子の予測位置と半径から境界との接触を問い合わせる。
    bool (*drain)(const void*, const math::Vector3&, const math::Vector3&) = nullptr; ///< 粒子の位置と速度から、領域から取り除くかを返す。
};

/// @brief 密度を位置の拘束として解く粒子液体ソルバー (Position Based Fluids)。
/// @see https://mmacklin.com/pbf_sig_preprint.pdf Macklin & Müller, Position Based Fluids (2013), Algorithm 1.
class FluidLiquidSolver {
public:
    struct Particle {
        float x  = 0.0f; ///< 液体粒子の局所 X 座標。
        float y  = 0.0f; ///< 液体粒子の局所 Y 座標。
        float vx = 0.0f; ///< 液体粒子の局所速度の X 成分。
        float vy = 0.0f; ///< 液体粒子の局所速度の Y 成分。
        float age = 0.0f; ///< 液体粒子を放出してからの経過時間 [秒]。
        float z  = 0.0f; ///< 奥行き。2D では常に 0。
        float vz = 0.0f; ///< 液体粒子の局所速度の Z 成分。
        float colorKey = 0.0f; ///< 撃ち出した発生源の FluidSource::colorKey ([0,1] に丸めたもの)。粒子の一生で変わらない。
    };

    /// @brief volumetric = true で 3D (Volume Flipbook Baker 用)。false の 2D は奥行きを持たない (z と vz は 0 のまま)。
    FBZZ_FLUID_API void Reset(const FluidRecipe& recipe, bool volumetric = false);
    /// @brief 粒子はそのままに、部品 (発生源・力・障害物) だけを差し替える (FluidGasSolver::ReplaceOperators と同じ意味)。
    /// @brief 既に撃ち出した数は発生源ごとに引き継ぐ (数が減ったら余りは捨てる)。kind・粒子半径が変わるなら false。
    [[nodiscard]] FBZZ_FLUID_API bool ReplaceOperators(const FluidRecipe& recipe);
    [[nodiscard]] bool IsVolumetric() const { return m_volumetric; }
    /// @brief 見積もった移動距離に応じて時刻を分割し、液体を進める。
    /// @param[in] dt 進める時間 [秒]。0 以下では処理しない。
    FBZZ_FLUID_API void Advance(float dt);
    /// @brief 境界の表面と排出条件を問い合わせながら液体を進める。
    /// @param[in] dt 進める時間 [秒]。0 以下では処理しない。
    /// @param[in] boundary 接触は粒子の予測位置で、排出は補正後の位置と速度で問い合わせる。コールバックと context は呼び出し中だけ借りる。
    FBZZ_FLUID_API void Advance(float dt, const FluidLiquidBoundary& boundary);
    /// @brief 時間を分割せず、粒子の放出から位置・速度の確定まで 1 刻み進める。
    /// @param[in] dt 刻みの長さ [秒]。0 以下では処理しない。
    FBZZ_FLUID_API void Step(float dt);

    [[nodiscard]] const std::vector<Particle>& Particles() const { return m_particles; }
    [[nodiscard]] float Time() const { return m_time; }
    [[nodiscard]] float ParticleRadius() const { return m_particleRadius; }
    [[nodiscard]] float RestDensity() const { return m_restDensity; }
    /// @brief 粒子 i の近傍で測った密度 (Step の最後の反復時点)。
    [[nodiscard]] FBZZ_FLUID_API float MeasuredDensity(std::size_t index) const;

private:
    void Step(float dt, const FluidLiquidBoundary& boundary);
    [[nodiscard]] FBZZ_FLUID_API float Poly6(float distanceSquared) const;
    [[nodiscard]] FBZZ_FLUID_API float SpikyGradientScale(float distance) const;
    [[nodiscard]] FBZZ_FLUID_API float NextRandom();
    [[nodiscard]] FBZZ_FLUID_API int   CellOf(float x, float y, float z) const;

    FBZZ_FLUID_API void AdoptOperators(const FluidRecipe& recipe);
    FBZZ_FLUID_API void Emit();
    FBZZ_FLUID_API void BuildNeighbors();
    FBZZ_FLUID_API void SolveDensity();
    void ComputeDensityMultipliers(float inverseRestDensity, float cohesion);
    void ComputeDensityCorrections(float inverseRestDensity);
    void ApplyDensityCorrections();
    void CommitParticlePositions(float dt, const FluidLiquidBoundary& boundary);
    FBZZ_FLUID_API void ApplyViscosity();

    /// @note 刻みの始めの時刻で居る障害物と、その時刻の中心・速度。
    struct ActiveCollider {
        std::size_t   index = 0; ///< ソルバーが保持する障害物設定配列の添字。
        math::Vector3 center = { 0.0f, 0.0f, 0.0f }; ///< この刻みで評価した障害物の局所中心位置。
        math::Vector3 velocity = { 0.0f, 0.0f, 0.0f }; ///< この刻みで流体へ伝える障害物の局所速度。
    };

    FluidLiquidSettings             m_settings; ///< @note 反復回数・粘性・粒子寿命など、各ステップで使う液体の設定。
    std::vector<FluidSource>        m_sources; ///< 気体と同じく有効な部品だけ。液体の発生源は粒子半径未満の寸法を粒子半径まで広げてある。
    std::vector<FluidForce>         m_forces; ///< 液体粒子へ適用する外力のレシピ設定のコピー。
    std::vector<FluidCollider>      m_colliders; ///< 液体粒子との接触を評価する障害物のレシピ設定のコピー。
    std::vector<ActiveCollider>     m_activeColliders; ///< @note 今回の刻みで作用する障害物と、その時刻での中心・速度。
    std::vector<FluidSourceMask>    m_sourceMasks; ///< m_sources と同じ並び (気体と同じ)。
    std::vector<int>                m_emittedPerSource; ///< @note 発生源ごとにこれまで放出した粒子数。設定を編集しても既に出した分は数え直さない。
    std::vector<Particle>           m_particles; ///< @note 位置・速度・年齢を確定した粒子。次の刻みの出発点になる。
    uint32_t m_rngState = 1; ///< @note 同じ種から粒子を同じ順序で放出するための乱数状態。
    float m_time = 0.0f; ///< @note 発生量と障害物の位置を評価する経過秒数。

    float m_particleRadius = 0.01f; ///< @note 境界との接触や静止時の粒子間隔を決める粒子の半径。
    float m_kernelRadius = 0.04f; ///< @note 周囲の粒子が密度に影響する距離。近傍検索のセル幅にも使う。
    float m_kernelRadiusSquared = 0.0016f; ///< @note 平方根を取らずに近傍の範囲内か判定するための半径の二乗。
    float m_densityKernelScale = 0.0f; ///< @note 近い粒子ほど密度への寄与を大きくする Poly6 の係数。2 次元と 3 次元で異なる。
    float m_gradientKernelScale = 0.0f; ///< @note 粒子間の距離から位置補正の勾配を求める Spiky カーネルの係数。
    float m_restDensity = 1.0f; ///< @note 粒子を直径間隔で並べたときの密度。現在の密度が過密かどうかの比較基準。
    float m_densityRegularization = 1.0e-6f; ///< @note 周囲に粒子が少ないときも密度補正の分母が小さくなり過ぎるのを防ぐ。
    float m_tensileReference = 1.0f; ///< @note 粒子が密集するときだけ強くなる反発を計算するための基準密度カーネル値。
    float m_tensileScale = 0.0f; ///< @note 近距離の粒子同士が固まり過ぎないように加える補正の倍率。

    int   m_cellsX = 1; ///< 一様格子と計数ソート用。セル幅はカーネルの有効半径 h と同じ。
    int   m_cellsY = 1; ///< 近傍検索格子の Y 方向のセル数。
    int   m_cellsZ = 1; ///< 近傍検索格子の Z 方向のセル数。
    bool  m_volumetric = false; ///< 粒子と近傍格子を三次元で更新するかどうか。
    std::vector<int> m_cellStart; ///< @note 各セルに入った粒子番号の区間の先頭。次のセルの先頭が区間の終端になる。
    std::vector<int> m_cellParticles; ///< @note 同じセルの粒子番号をまとめた配列。周辺セルだけから近傍を探す。
    std::vector<int> m_cellOfParticle; ///< @note 粒子番号から所属セルを引く逆引き表。
    std::vector<int> m_neighborStart; ///< 粒子 i の近傍は m_neighbors の [m_neighborStart[i], m_neighborStart[i+1]) に並ぶ。
    std::vector<int> m_neighbors; ///< 各粒子の近傍範囲から参照する粒子インデックスの平坦配列。

    std::vector<float> m_predictedX; ///< X・Y・Z は位置を仮に時間発展させた値。密度・衝突の補正が済んでから粒子の位置と速度を確定する。
    std::vector<float> m_predictedY; ///< 位置補正を蓄積する予測位置の Y 成分。添字は粒子番号。
    std::vector<float> m_predictedZ; ///< 位置補正を蓄積する予測位置の Z 成分。添字は粒子番号。
    std::vector<float> m_densityMultiplier; ///< @note 密度のずれを各粒子の位置補正に換算する重み（数式の λ）。
    std::vector<float> m_density; ///< @note 近傍粒子から最後に測った密度。基準密度と比較して補正量を決める。
    std::vector<float> m_deltaX; ///< X・Y・Z に全粒子の補正を一旦ためる。隣を調べている途中で書き換えず、位置補正と粘性の段階で使い回す。
    std::vector<float> m_deltaY; ///< 今回の密度拘束反復で足す位置補正の Y 成分。
    std::vector<float> m_deltaZ; ///< 今回の密度拘束反復で足す位置補正の Z 成分。
};

}
