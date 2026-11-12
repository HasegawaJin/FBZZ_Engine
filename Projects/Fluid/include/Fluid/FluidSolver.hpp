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

#include <Fluid/FluidRecipe.hpp>
#include <Fluid/FluidSourceMask.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::fluid {

/// @brief 格子の気体ソルバー。nz = 1 で 2D (フリップブック)、nz > 1 で 3D (速度場 PNG)。
/// @brief 1 刻みの順序: 注入 → 燃焼 → 外力 (浮力・風・乱流・渦度保存) → 障害物 → 圧力投影 → 移流 → 障害物 → 散逸。
/// @note 投影を移流の前に置く理由: 移流は発散ゼロの速度場で運ぶことが前提で、外力で乱れた場のまま
/// @note 運ぶと密度が湧いたり消えたりして量が合わなくなる。
/// @brief 障害物は刻みの始めの時刻でセルを固体と決め、固体セルの速度を障害物の動きの速度に、煙・熱・燃料を 0
/// @brief にする。圧力は固体の隣を床と同じ「通り抜けない壁」(∂p/∂n = 0) として解く。
/// @brief 速度は「領域単位/秒」(最も長い軸を [-1,1] とする)。セルは全軸で同じ幅。
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
    /// @brief 速度場から発散を取り除く。expansion は «湧き出し» [1/秒] (燃焼の膨張)。
    FBZZ_FLUID_API void Project(const std::vector<float>* expansion = nullptr);

    [[nodiscard]] int   SizeX() const { return m_nx; }
    [[nodiscard]] int   SizeY() const { return m_ny; }
    [[nodiscard]] int   SizeZ() const { return m_nz; }
    [[nodiscard]] float CellSize() const { return m_h; }
    [[nodiscard]] float Time() const { return m_time; }
    [[nodiscard]] std::size_t Index(int x, int y, int z) const
    {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(m_ny) + static_cast<std::size_t>(y))
            * static_cast<std::size_t>(m_nx) + static_cast<std::size_t>(x);
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
    FBZZ_FLUID_API void Advect(float dt);
    FBZZ_FLUID_API void AdvectScalar(std::vector<float>& field, float dt);
    FBZZ_FLUID_API void Dissipate(float dt);
    FBZZ_FLUID_API void UpdateColorKey();
    FBZZ_FLUID_API void ResetDetailLayer(int layer);
    FBZZ_FLUID_API void AdvectDetail(float dt);
    FBZZ_FLUID_API void UpdateDetailEpochs();

    FluidGasSettings            m_settings;
    /// @note 有効な部品だけを先頭から上限数まで持つ。ノイズの切り出し位置は「有効な部品の中での添字」で
    /// @note 決まり、GPU (有効な部品だけを詰める) と同じ数え方になる。
    std::vector<FluidSource>    m_sources;
    std::vector<FluidForce>     m_forces;
    std::vector<FluidCollider>  m_colliders;
    /// @note m_sources と同じ並び。Texture 以外と読めなかった画像は空 (= 1 として引かれ、板の形に湧く)。
    std::vector<FluidSourceMask> m_sourceMasks;
    math::Vector3 m_noiseOffset = { 0.0f, 0.0f, 0.0f };
    /// @note ReplaceOperators が「同じ格子の続き」と言えるかを判じるための、Reset したレシピの控え
    /// @note (seed = ノイズの切り出し位置、要求解像度 = 2D は ResolveGasResolution・3D は vectorFieldResolution)。
    uint32_t m_seed = 1;
    int   m_gridRequest = 0;
    int   m_nx = 1;
    int   m_ny = 1;
    int   m_nz = 1;
    float m_h = 1.0f;
    float m_time = 0.0f;

    std::vector<float> m_density;
    std::vector<float> m_temperature;
    std::vector<float> m_fuel;
    /// @note 色の量 (密度 × 鍵)。密度と同じ移流・散逸で運ぶ。鍵の付いた発生源が 1 つも無いレシピは
    /// @note m_hasColor = false で運ぶのを飛ばす (鍵は全部 0 のまま。密度には元から触らない)。
    std::vector<float> m_colorMass;
    /// @note 燃料の色の量 (燃料 × 鍵)。燃料と同じに扱う (注入・移流・障害物で 0。散逸はしない)。
    /// @note 煙と別に持つ理由: 煙を注がない発生源 (Fire プリセット = density 0・fuel > 0) では、
    /// @note 燃えて生まれる煤が「その場の煙の鍵」しか受け取れず、色の付いた炎が作れなかった。
    std::vector<float> m_fuelColorMass;
    std::vector<float> m_colorKey;
    bool m_hasColor = false;
    std::vector<float> m_vx;
    std::vector<float> m_vy;
    std::vector<float> m_vz;
    std::vector<float> m_pressure;
    std::vector<float> m_divergence;
    std::vector<float> m_expansion;
    /// @note 移流・渦度の作業領域。刻みごとに確保し直さない。
    std::vector<float> m_prevVx;
    std::vector<float> m_prevVy;
    std::vector<float> m_prevVz;
    std::vector<float> m_scratchA;
    std::vector<float> m_scratchB;
    std::vector<float> m_scratchC;
    std::vector<float> m_curlX;
    std::vector<float> m_curlY;
    std::vector<float> m_curlZ;
    std::vector<float> m_curlLength;
    /// @note 障害物のセル (1 = 固体) と、そのセルの速度 (障害物の動きの速度。重なったら先の障害物)。
    /// @note 刻みの始めに BuildSolids が作り直す。効いている障害物が無い刻みは m_hasSolid = false で
    /// @note 全部飛ばす (障害物の無いレシピの結果を 1 ビットも変えない)。
    std::vector<uint8_t> m_solid;
    std::vector<float> m_solidVx;
    std::vector<float> m_solidVy;
    std::vector<float> m_solidVz;
    bool m_hasSolid = false;
    /// @note 流れに乗せた細部の座標 (2 層)。3D では空のまま。
    std::vector<float> m_detailU[2];
    std::vector<float> m_detailV[2];
    int m_detailEpoch[2] = { 0, 0 };
};

/// @brief Collision field sampled in normalized liquid simulation coordinates.
/// @note distance is positive outside the solid and normal is a unit outward gradient.
struct FluidLiquidContact {
    math::Vector3 normal = math::Vector3::UP;
    float distance = 0.0f;
    float restitution = 0.0f;
};

struct FluidLiquidBoundary {
    const void* context = nullptr;
    bool (*sample)(const void*, const math::Vector3&, float, FluidLiquidContact&) = nullptr;
    bool (*drain)(const void*, const math::Vector3&, const math::Vector3&) = nullptr;
};

/// @brief 粒子液体ソルバー (Position Based Fluids, Macklin & Müller 2013)。

/// @note PBF を選ぶ理由: 力で押し返す SPH は硬い液体ほど刻みを細かくしないと爆発する。PBF は密度を
/// @note 「位置の拘束」として解くので、しぶきの初速が大きくても刻み数が読める。焼きはオフラインでも
/// @note 「押したら数秒で返ってくる」ことが大事なので、安定性を優先した。
class FluidLiquidSolver {
public:
    struct Particle {
        float x  = 0.0f;
        float y  = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float age = 0.0f;
        /// @brief 奥行き。2D では常に 0。
        float z  = 0.0f;
        float vz = 0.0f;
        /// @brief 撃ち出した発生源の FluidSource::colorKey ([0,1] に丸めたもの)。粒子の一生で変わらない。
        float colorKey = 0.0f;
    };

    /// @brief volumetric = true で 3D (Volume Flipbook Baker 用)。false の 2D は奥行きを持たない (z と vz は 0 のまま)。
    FBZZ_FLUID_API void Reset(const FluidRecipe& recipe, bool volumetric = false);
    /// @brief 粒子はそのままに、部品 (発生源・力・障害物) だけを差し替える (FluidGasSolver::ReplaceOperators と同じ意味)。
    /// @brief 既に撃ち出した数は発生源ごとに引き継ぐ (数が減ったら余りは捨てる)。kind・粒子半径が変わるなら false。
    [[nodiscard]] FBZZ_FLUID_API bool ReplaceOperators(const FluidRecipe& recipe);
    [[nodiscard]] bool IsVolumetric() const { return m_volumetric; }
    /// @brief dt 秒ぶん進める。1 刻みで粒子半径以上動かないよう内部で刻みを割る。
    FBZZ_FLUID_API void Advance(float dt);
    /// @note The boundary and its context are borrowed for this call only.
    FBZZ_FLUID_API void Advance(float dt, const FluidLiquidBoundary& boundary);
    FBZZ_FLUID_API void Step(float dt);

    [[nodiscard]] const std::vector<Particle>& Particles() const { return m_particles; }
    [[nodiscard]] float Time() const { return m_time; }
    [[nodiscard]] float ParticleRadius() const { return m_radius; }
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
    FBZZ_FLUID_API void ApplyViscosity();

    /// @note 刻みの始めの時刻で居る障害物と、その時刻の中心・速度。
    struct ActiveCollider {
        std::size_t   index = 0;
        math::Vector3 center = { 0.0f, 0.0f, 0.0f };
        math::Vector3 velocity = { 0.0f, 0.0f, 0.0f };
    };

    FluidLiquidSettings             m_settings;
    /// @note 気体と同じく有効な部品だけ。液体の発生源は粒子半径未満の寸法を粒子半径まで広げてある。
    std::vector<FluidSource>        m_sources;
    std::vector<FluidForce>         m_forces;
    std::vector<FluidCollider>      m_colliders;
    std::vector<ActiveCollider>     m_activeColliders;
    /// @note m_sources と同じ並び (気体と同じ)。
    std::vector<FluidSourceMask>    m_sourceMasks;
    std::vector<int>                m_emitted;
    std::vector<Particle>           m_particles;
    uint32_t m_rngState = 1;
    float m_time = 0.0f;

    float m_radius = 0.01f;
    float m_h = 0.04f;
    float m_h2 = 0.0016f;
    float m_poly6 = 0.0f;
    float m_spikyGradient = 0.0f;
    float m_restDensity = 1.0f;
    float m_relaxation = 1.0e-6f;
    float m_tensileReference = 1.0f;
    float m_tensileScale = 0.0f;

    /// @note 近傍探索 (一様格子 + 計数ソート)。
    int   m_cellsX = 1;
    int   m_cellsY = 1;
    int   m_cellsZ = 1;
    bool  m_volumetric = false;
    std::vector<int> m_cellStart;
    std::vector<int> m_cellParticles;
    std::vector<int> m_particleCell;
    std::vector<int> m_neighborStart;
    std::vector<int> m_neighbors;

    std::vector<float> m_predictedX;
    std::vector<float> m_predictedY;
    std::vector<float> m_predictedZ;
    std::vector<float> m_lambda;
    std::vector<float> m_density;
    std::vector<float> m_deltaX;
    std::vector<float> m_deltaY;
    std::vector<float> m_deltaZ;
};

} /// @note namespace fbzz::fluid
