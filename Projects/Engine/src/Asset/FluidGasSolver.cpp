/// @file    FluidGasSolver.cpp
/// @brief   格子の気体ソルバー (Stable Fluids + MacCormack 移流 + 渦度保存 + 燃焼)
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 速度は全成分をセル中心に置く (collocated)。スタッガード格子より投影が緩くなるが、
/// フリップブック用の絵としては差が見えず、2D と 3D を同じコードで扱える方を取った。
#include <Engine/Asset/FluidSolver.hpp>

#include <Engine/Asset/FluidOperatorEval.hpp>
#include <Engine/Core/CurlNoise.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::asset {
namespace {

// 注入を重ねても値が発散しないための上限。絵の上では opacity で飽和するので十分大きい。
constexpr float kMaxDensity     = 64.0f;
constexpr float kMaxTemperature = 64.0f;
constexpr int   kMaxAxisCells   = 512;
// 鍵 = 色の量 / max(密度, これ)。GPU (FluidGpuCommon の写し) と同じ値にしておかないと焼き分けで色が変わる。
constexpr float kColorEpsilon   = 1.0e-6f;

[[nodiscard]] float Lerp(float a, float b, float t) { return a + (b - a) * t; }
[[nodiscard]] float Saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }
[[nodiscard]] float ColorKeyOf(float mass, float density) { return Saturate(mass / (std::max)(density, kColorEpsilon)); }

// 注入で走査するセルの範囲 (形を外から包む箱)。FluidSourceWeight と同じく minSize まで広げた寸法で包む。
void SourceBounds(const FluidSource& source, const math::Vector3& center, float minSize,
                  math::Vector3& outLo, math::Vector3& outHi)
{
    const float sx = (std::max)(source.size.x, minSize);
    const float sy = (std::max)(source.size.y, minSize);
    const float sz = (std::max)(source.size.z, minSize);
    math::Vector3 extent = { sx, sx, sx };
    switch (source.shape) {
    case FluidSourceShape::Box:
        extent = { sx, sy, sz };
        break;
    case FluidSourceShape::Cone: {
        const math::Vector3& d = source.direction;
        const float lengthSq = d.x * d.x + d.y * d.y + d.z * d.z;
        const math::Vector3 axis = lengthSq < 1.0e-12f ? math::Vector3{ 0.0f, 1.0f, 0.0f }
                                                       : d * (1.0f / std::sqrt(lengthSq));
        // 頂点と底の中心を含む箱を、底の半径 (= 最も太い所) だけ全軸に膨らませる。
        const math::Vector3 tip = center + axis * sy;
        outLo = { (std::min)(center.x, tip.x) - sx, (std::min)(center.y, tip.y) - sx, (std::min)(center.z, tip.z) - sx };
        outHi = { (std::max)(center.x, tip.x) + sx, (std::max)(center.y, tip.y) + sx, (std::max)(center.z, tip.z) + sx };
        return;
    }
    case FluidSourceShape::Ring:
        extent = { sx + sy, sx + sy, sx + sy };
        break;
    case FluidSourceShape::Texture: {
        // 傾いた板 (箱) を包む軸並行の箱: 各軸へ 3 本の半軸を投影した長さの和。
        math::Vector3 right;
        math::Vector3 up;
        math::Vector3 normal;
        FluidTextureSourceBasis(source, right, up, normal);
        extent = { std::fabs(right.x) * sx + std::fabs(up.x) * sy + std::fabs(normal.x) * sz,
                   std::fabs(right.y) * sx + std::fabs(up.y) * sy + std::fabs(normal.y) * sz,
                   std::fabs(right.z) * sx + std::fabs(up.z) * sy + std::fabs(normal.z) * sz };
        break;
    }
    case FluidSourceShape::Capsule:
    case FluidSourceShape::Cylinder: {
        const math::Vector3& d = source.direction;
        const float lengthSq = d.x * d.x + d.y * d.y + d.z * d.z;
        const math::Vector3 axis = lengthSq < 1.0e-12f ? math::Vector3{ 0.0f, 1.0f, 0.0f }
                                                       : d * (1.0f / std::sqrt(lengthSq));
        // 芯の線分を包む箱を半径だけ全軸に膨らませる (円柱はこのカプセルの中に収まる)。
        extent = { std::fabs(axis.x) * sy + sx, std::fabs(axis.y) * sy + sx, std::fabs(axis.z) * sy + sx };
        break;
    }
    case FluidSourceShape::Sphere:
    default:
        break;
    }
    outLo = center - extent;
    outHi = center + extent;
}

void LoadSourceMasks(const std::vector<FluidSource>& sources, std::vector<FluidSourceMask>& outMasks)
{
    outMasks.assign(sources.size(), FluidSourceMask{});
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].shape != FluidSourceShape::Texture || sources[i].texture.empty()) continue;
        if (!LoadFluidSourceMask(sources[i].texture, outMasks[i])) outMasks[i].values.clear();
    }
}

// このレシピが要求する格子の 1 辺。呼び手が Reset に渡す nx を導いた値で、これが変われば別の格子になる。
[[nodiscard]] int GasGridRequest(const FluidRecipe& recipe, bool volumetric)
{
    return volumetric ? recipe.output.vectorFieldResolution : ResolveGasResolution(recipe);
}

} // namespace

// 部品 (発生源・力・障害物) とその画像だけを取り込む。Reset と ReplaceOperators で共通。
void FluidGasSolver::AdoptOperators(const FluidRecipe& recipe)
{
    m_sources.clear();
    for (const FluidSource& source : recipe.sources)
        if (source.enabled && m_sources.size() < static_cast<std::size_t>(kMaxFluidSources))
            m_sources.push_back(source);
    m_hasColor = false;
    for (FluidSource& source : m_sources) {
        source.colorKey = Saturate(source.colorKey);
        m_hasColor = m_hasColor || source.colorKey > 0.0f;
    }
    m_forces.clear();
    for (const FluidForce& force : recipe.forces)
        if (force.enabled && m_forces.size() < static_cast<std::size_t>(kMaxFluidForces))
            m_forces.push_back(force);
    m_colliders.clear();
    for (const FluidCollider& collider : recipe.colliders)
        if (collider.enabled && m_colliders.size() < static_cast<std::size_t>(kMaxFluidColliders))
            m_colliders.push_back(collider);
    LoadSourceMasks(m_sources, m_sourceMasks);
}

void FluidGasSolver::Reset(const FluidRecipe& recipe, int nx, int ny, int nz)
{
    m_settings = recipe.gas;
    AdoptOperators(recipe);
    m_solid.clear();
    m_hasSolid = false;
    m_nx = std::clamp(nx, 1, kMaxAxisCells);
    m_ny = std::clamp(ny, 1, kMaxAxisCells);
    m_nz = std::clamp(nz, 1, kMaxAxisCells);
    m_h  = 2.0f / static_cast<float>((std::max)(m_nx, (std::max)(m_ny, m_nz)));
    m_time = 0.0f;
    m_seed = recipe.seed;
    m_gridRequest = GasGridRequest(recipe, m_nz > 1);

    // seed は «ノイズ格子のどこを切り出すか» を決める。値を変えるのではなく座標をずらす。
    const uint32_t hash = core::PcgHash(recipe.seed * 2654435761u + 1u);
    m_noiseOffset = { static_cast<float>(hash % 997u) * 0.731f,
                      static_cast<float>((hash >> 10) % 997u) * 0.517f,
                      static_cast<float>((hash >> 20) % 997u) * 0.379f };

    const std::size_t count = static_cast<std::size_t>(m_nx) * m_ny * m_nz;
    for (std::vector<float>* field : { &m_density, &m_temperature, &m_fuel,
                                       &m_colorMass, &m_fuelColorMass, &m_colorKey,
                                       &m_vx, &m_vy, &m_vz,
                                       &m_pressure, &m_divergence, &m_expansion,
                                       &m_prevVx, &m_prevVy, &m_prevVz,
                                       &m_scratchA, &m_scratchB, &m_scratchC,
                                       &m_curlX, &m_curlY, &m_curlZ, &m_curlLength })
        field->assign(count, 0.0f);

    // 細部の座標は 2D (フリップブック) でだけ使う。3D (.vfield) は速度しか焼かないので持たない。
    for (int layer = 0; layer < 2; ++layer) {
        if (m_nz == 1) {
            m_detailU[layer].assign(count, 0.0f);
            m_detailV[layer].assign(count, 0.0f);
            ResetDetailLayer(layer);
        } else {
            m_detailU[layer].clear();
            m_detailV[layer].clear();
        }
        m_detailEpoch[layer] = 0;
    }
}

// 場を残せる条件は «今持っている場が、このレシピを頭から解いた途中経過だと言い張れるか»。
//   kind        : 気体でなければ格子そのものが別物
//   seed        : ノイズの切り出し位置 (m_noiseOffset) が変わる = 乱流も注入の揺らぎも別の流れ
//   要求解像度  : 呼び手が Reset に渡した nx を導いた値。変われば格子の細かさが変わる
// 逆に、刻みごとに効くだけの設定 (浮力・散逸・床・圧力反復・燃焼) は途中から差し替えてよい。
bool FluidGasSolver::ReplaceOperators(const FluidRecipe& recipe)
{
    if (recipe.kind != FluidKind::Gas) return false;
    if (recipe.seed != m_seed) return false;
    if (GasGridRequest(recipe, m_nz > 1) != m_gridRequest) return false;

    m_settings = recipe.gas;
    const bool hadColor = m_hasColor;
    AdoptOperators(recipe);
    // 鍵の付いた発生源が 1 つも無くなったら、運んでいた色も捨てる («鍵が無ければ全部 0» を保つ)。
    if (hadColor && !m_hasColor) {
        std::fill(m_colorMass.begin(), m_colorMass.end(), 0.0f);
        std::fill(m_fuelColorMass.begin(), m_fuelColorMass.end(), 0.0f);
        std::fill(m_colorKey.begin(), m_colorKey.end(), 0.0f);
    }
    // 障害物は刻みの始めに組み直す。ここで忘れておかないと、消した障害物が次の 1 刻みだけ残る。
    m_solid.clear();
    m_hasSolid = false;
    return true;
}

void FluidGasSolver::ResetDetailLayer(int layer)
{
    for (int y = 0; y < m_ny; ++y)
        for (int x = 0; x < m_nx; ++x) {
            const std::size_t i = Index(x, y, 0);
            m_detailU[layer][i] = CellCenter(x, m_nx);
            m_detailV[layer][i] = CellCenter(y, m_ny);
        }
}

void FluidGasSolver::AdvectDetail(float dt)
{
    if (!HasDetail()) return;
    const float cellsPerUnit = dt / m_h;
    for (int layer = 0; layer < 2; ++layer) {
        for (int y = 0; y < m_ny; ++y)
            for (int x = 0; x < m_nx; ++x) {
                const std::size_t i = Index(x, y, 0);
                const float gx = static_cast<float>(x) - m_prevVx[i] * cellsPerUnit;
                const float gy = static_cast<float>(y) - m_prevVy[i] * cellsPerUnit;
                m_scratchA[i] = SampleGrid(m_detailU[layer], gx, gy, 0.0f);
                m_scratchB[i] = SampleGrid(m_detailV[layer], gx, gy, 0.0f);
            }
        m_detailU[layer].swap(m_scratchA);
        m_detailV[layer].swap(m_scratchB);
    }
}

// 層 0 は位相 0、層 1 は位相 0.5 で初期位置へ戻す。どちらもその瞬間の重みが 0 なので継ぎ目は見えない。
void FluidGasSolver::UpdateDetailEpochs()
{
    if (!HasDetail()) return;
    const float period = (std::max)(m_settings.detailPeriod, 0.05f);
    const int epoch0 = static_cast<int>(std::floor(m_time / period));
    const int epoch1 = static_cast<int>(std::floor(m_time / period + 0.5f));
    if (epoch0 != m_detailEpoch[0]) {
        m_detailEpoch[0] = epoch0;
        ResetDetailLayer(0);
    }
    if (epoch1 != m_detailEpoch[1]) {
        m_detailEpoch[1] = epoch1;
        ResetDetailLayer(1);
    }
}

void FluidGasSolver::DetailWeights(float& outLayer0, float& outLayer1) const
{
    const float period = (std::max)(m_settings.detailPeriod, 0.05f);
    const float phase = m_time / period - std::floor(m_time / period);
    outLayer0 = 1.0f - std::fabs(2.0f * phase - 1.0f);
    outLayer1 = 1.0f - outLayer0;
}

void FluidGasSolver::SampleDetailCoordinate(int layer, float x, float y, float& outU, float& outV) const
{
    if (!HasDetail()) {
        outU = x;
        outV = y;
        return;
    }
    layer = std::clamp(layer, 0, 1);
    const float gx = ToGrid(x, m_nx);
    const float gy = ToGrid(y, m_ny);
    outU = SampleGrid(m_detailU[layer], gx, gy, 0.0f);
    outV = SampleGrid(m_detailV[layer], gx, gy, 0.0f);
}

float FluidGasSolver::CellCenter(int index, int count) const
{
    return (static_cast<float>(index) + 0.5f - static_cast<float>(count) * 0.5f) * m_h;
}

float FluidGasSolver::ToGrid(float position, int count) const
{
    return position / m_h + static_cast<float>(count) * 0.5f - 0.5f;
}

float FluidGasSolver::SampleGrid(const std::vector<float>& field, float gx, float gy, float gz) const
{
    gx = std::clamp(gx, 0.0f, static_cast<float>(m_nx - 1));
    gy = std::clamp(gy, 0.0f, static_cast<float>(m_ny - 1));
    gz = std::clamp(gz, 0.0f, static_cast<float>(m_nz - 1));
    const int x0 = static_cast<int>(gx);
    const int y0 = static_cast<int>(gy);
    const int z0 = static_cast<int>(gz);
    const int x1 = (std::min)(x0 + 1, m_nx - 1);
    const int y1 = (std::min)(y0 + 1, m_ny - 1);
    const int z1 = (std::min)(z0 + 1, m_nz - 1);
    const float tx = gx - static_cast<float>(x0);
    const float ty = gy - static_cast<float>(y0);
    const float tz = gz - static_cast<float>(z0);
    const float c00 = Lerp(field[Index(x0, y0, z0)], field[Index(x1, y0, z0)], tx);
    const float c10 = Lerp(field[Index(x0, y1, z0)], field[Index(x1, y1, z0)], tx);
    const float c01 = Lerp(field[Index(x0, y0, z1)], field[Index(x1, y0, z1)], tx);
    const float c11 = Lerp(field[Index(x0, y1, z1)], field[Index(x1, y1, z1)], tx);
    return Lerp(Lerp(c00, c10, ty), Lerp(c01, c11, ty), tz);
}

void FluidGasSolver::MinMaxAround(const std::vector<float>& field, float gx, float gy, float gz,
                                  float& outMin, float& outMax) const
{
    gx = std::clamp(gx, 0.0f, static_cast<float>(m_nx - 1));
    gy = std::clamp(gy, 0.0f, static_cast<float>(m_ny - 1));
    gz = std::clamp(gz, 0.0f, static_cast<float>(m_nz - 1));
    const int x0 = static_cast<int>(gx);
    const int y0 = static_cast<int>(gy);
    const int z0 = static_cast<int>(gz);
    const int x1 = (std::min)(x0 + 1, m_nx - 1);
    const int y1 = (std::min)(y0 + 1, m_ny - 1);
    const int z1 = (std::min)(z0 + 1, m_nz - 1);
    outMin = outMax = field[Index(x0, y0, z0)];
    for (const int z : { z0, z1 })
        for (const int y : { y0, y1 })
            for (const int x : { x0, x1 }) {
                const float value = field[Index(x, y, z)];
                outMin = (std::min)(outMin, value);
                outMax = (std::max)(outMax, value);
            }
}

// 軸方向の中心差分。縁では片側差分になり、1 セルしか無い軸 (2D の z) は 0。
float FluidGasSolver::Derivative(const std::vector<float>& field, int x, int y, int z, int axis) const
{
    const int count = axis == 0 ? m_nx : (axis == 1 ? m_ny : m_nz);
    if (count <= 1) return 0.0f;
    const int center = axis == 0 ? x : (axis == 1 ? y : z);
    const int lo = (std::max)(center - 1, 0);
    const int hi = (std::min)(center + 1, count - 1);
    const float low  = field[Index(axis == 0 ? lo : x, axis == 1 ? lo : y, axis == 2 ? lo : z)];
    const float high = field[Index(axis == 0 ? hi : x, axis == 1 ? hi : y, axis == 2 ? hi : z)];
    return (high - low) / (static_cast<float>(hi - lo) * m_h);
}

float FluidGasSolver::Divergence(int x, int y, int z) const
{
    const float inverseTwoH = 0.5f / m_h;
    float divergence = 0.0f;
    if (m_nx > 1) {
        const float right = m_vx[Index((std::min)(x + 1, m_nx - 1), y, z)];
        const float left  = m_vx[Index((std::max)(x - 1, 0), y, z)];
        divergence += (right - left) * inverseTwoH;
    }
    if (m_ny > 1) {
        const float up = m_vy[Index(x, (std::min)(y + 1, m_ny - 1), z)];
        // 床は «通り抜けられない壁»。下のゴーストは鏡映 (-vy) で、床へ向かう流れを発散として拾う。
        const float down = y > 0 ? m_vy[Index(x, y - 1, z)]
                                 : (m_settings.floor ? -m_vy[Index(x, 0, z)] : m_vy[Index(x, 0, z)]);
        divergence += (up - down) * inverseTwoH;
    }
    if (m_nz > 1) {
        const float front = m_vz[Index(x, y, (std::min)(z + 1, m_nz - 1))];
        const float back  = m_vz[Index(x, y, (std::max)(z - 1, 0))];
        divergence += (front - back) * inverseTwoH;
    }
    return divergence;
}

float FluidGasSolver::SampleDensity(float x, float y, float z) const
{
    return SampleGrid(m_density, ToGrid(x, m_nx), ToGrid(y, m_ny), m_nz > 1 ? ToGrid(z, m_nz) : 0.0f);
}

float FluidGasSolver::SampleTemperature(float x, float y, float z) const
{
    return SampleGrid(m_temperature, ToGrid(x, m_nx), ToGrid(y, m_ny), m_nz > 1 ? ToGrid(z, m_nz) : 0.0f);
}

float FluidGasSolver::SampleColorKey(float x, float y, float z) const
{
    if (!m_hasColor) return 0.0f;
    const float gx = ToGrid(x, m_nx);
    const float gy = ToGrid(y, m_ny);
    const float gz = m_nz > 1 ? ToGrid(z, m_nz) : 0.0f;
    return ColorKeyOf(SampleGrid(m_colorMass, gx, gy, gz), SampleGrid(m_density, gx, gy, gz));
}

void FluidGasSolver::SampleVelocity(float x, float y, float z, float& outX, float& outY, float& outZ) const
{
    const float gx = ToGrid(x, m_nx);
    const float gy = ToGrid(y, m_ny);
    const float gz = m_nz > 1 ? ToGrid(z, m_nz) : 0.0f;
    outX = SampleGrid(m_vx, gx, gy, gz);
    outY = SampleGrid(m_vy, gx, gy, gz);
    outZ = m_nz > 1 ? SampleGrid(m_vz, gx, gy, gz) : 0.0f;
}

float FluidGasSolver::MaxDivergence() const
{
    float maxDivergence = 0.0f;
    for (int z = 0; z < m_nz; ++z)
        for (int y = 0; y < m_ny; ++y)
            for (int x = 0; x < m_nx; ++x) {
                // 障害物の中は流体ではない (投影もそこを解かない)。縁の固体セルは外の流れとの差を拾うだけ。
                if (m_hasSolid && m_solid[Index(x, y, z)] != 0) continue;
                maxDivergence = (std::max)(maxDivergence, std::fabs(Divergence(x, y, z)));
            }
    return maxDivergence;
}

float FluidGasSolver::TotalDensity() const
{
    double total = 0.0;
    for (const float value : m_density) total += value;
    return static_cast<float>(total);
}

void FluidGasSolver::Advance(float dt)
{
    if (dt <= 0.0f) return;
    float maxSpeed = 0.0f;
    for (std::size_t i = 0; i < m_vx.size(); ++i)
        maxSpeed = (std::max)(maxSpeed, std::fabs(m_vx[i]) + std::fabs(m_vy[i]) + std::fabs(m_vz[i]));
    // 1 刻みで 2 セルまで。半ラグランジュ移流は安定だが、これを超えると細い煙が
    // 隙間を飛び越えて «ちぎれる»。
    const float cellsTravelled = maxSpeed * dt / m_h;
    const int steps = std::clamp(static_cast<int>(std::ceil(cellsTravelled / 2.0f)), 1, 16);
    const float stepDt = dt / static_cast<float>(steps);
    for (int step = 0; step < steps; ++step) Step(stepDt);
}

void FluidGasSolver::Step(float dt)
{
    if (dt <= 0.0f) return;
    BuildSolids();
    Inject(dt);
    Burn(dt);
    ApplyForces(dt);
    if (m_hasSolid) EnforceSolids(true);
    Project(&m_expansion);
    Advect(dt);
    // 移流の補間は固体セルの隣から値を引くので、煙が障害物の中へ滲む。刻みの終わりに消す。
    if (m_hasSolid) EnforceSolids(true);
    AdvectDetail(dt);
    Dissipate(dt);
    UpdateColorKey();
    m_time += dt;
    UpdateDetailEpochs();
}

void FluidGasSolver::BuildSolids()
{
    m_hasSolid = false;
    if (m_colliders.empty()) return;
    const bool volumetric = m_nz > 1;
    std::array<std::size_t, kMaxFluidColliders> indices{};
    std::array<FluidOperatorPose, kMaxFluidColliders> poses{};
    int activeCount = 0;
    for (std::size_t c = 0; c < m_colliders.size() && activeCount < kMaxFluidColliders; ++c) {
        if (!FluidColliderActive(m_colliders[c], m_time)) continue;
        indices[static_cast<std::size_t>(activeCount)] = c;
        poses[static_cast<std::size_t>(activeCount)] = PoseFluidCollider(m_colliders[c], m_time);
        ++activeCount;
    }
    if (activeCount == 0) return;

    const std::size_t count = m_density.size();
    m_solid.assign(count, 0);
    m_solidVx.resize(count);
    m_solidVy.resize(count);
    m_solidVz.resize(count);
    for (int z = 0; z < m_nz; ++z) {
        for (int y = 0; y < m_ny; ++y) {
            for (int x = 0; x < m_nx; ++x) {
                const math::Vector3 p = { CellCenter(x, m_nx), CellCenter(y, m_ny), CellCenter(z, m_nz) };
                for (int k = 0; k < activeCount; ++k) {
                    const std::size_t slot = static_cast<std::size_t>(k);
                    // minSize = セル幅: それより細い障害物はどのセル中心も覆えず、素通りになる。
                    if (FluidColliderDistance(m_colliders[indices[slot]], poses[slot].center, p, m_h, volumetric) >= 0.0f)
                        continue;
                    const std::size_t i = Index(x, y, z);
                    m_solid[i] = 1;
                    m_solidVx[i] = poses[slot].motionVelocity.x;
                    m_solidVy[i] = poses[slot].motionVelocity.y;
                    m_solidVz[i] = poses[slot].motionVelocity.z;
                    m_hasSolid = true;
                    break;
                }
            }
        }
    }
}

void FluidGasSolver::EnforceSolids(bool clearScalars)
{
    const bool volumetric = m_nz > 1;
    for (std::size_t i = 0; i < m_solid.size(); ++i) {
        if (m_solid[i] == 0) continue;
        m_vx[i] = m_solidVx[i];
        m_vy[i] = m_solidVy[i];
        // 2D は奥行きの速度を持たない (動きのキーに z があっても平面の流れには関係しない)。
        if (volumetric) m_vz[i] = m_solidVz[i];
        if (clearScalars) {
            m_density[i] = 0.0f;
            m_temperature[i] = 0.0f;
            m_fuel[i] = 0.0f;
            m_colorMass[i] = 0.0f;
            m_fuelColorMass[i] = 0.0f;
        }
    }
}

void FluidGasSolver::Inject(float dt)
{
    const bool volumetric = m_nz > 1;
    for (std::size_t sourceIndex = 0; sourceIndex < m_sources.size(); ++sourceIndex) {
        const FluidSource& source = m_sources[sourceIndex];
        if (!FluidSourceEmitting(source, m_time)) continue;
        const FluidOperatorPose pose = PoseFluidSource(source, m_time);

        math::Vector3 lo = pose.center;
        math::Vector3 hi = pose.center;
        SourceBounds(source, pose.center, m_h, lo, hi);
        const auto range = [&](float low, float high, int count, int& first, int& last) {
            first = std::clamp(static_cast<int>(std::floor(ToGrid(low, count))), 0, count - 1);
            last  = std::clamp(static_cast<int>(std::ceil(ToGrid(high, count))), 0, count - 1);
        };
        int x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;
        range(lo.x, hi.x, m_nx, x0, x1);
        range(lo.y, hi.y, m_ny, y0, y1);
        if (volumetric) range(lo.z, hi.z, m_nz, z0, z1);

        // 注ぐ «量» だけをエンベロープで揺らす (流速には掛けない — 勢いの変化は velocity と motion の担当)。
        // GPU (PackFluidGpuStep) も同じく «基準の量 × 倍率» を定数へ詰めてから weight・dt を掛ける。
        const float amount = FluidSourceAmount(source, m_time);
        const float density = source.density * amount;
        const float temperature = source.temperature * amount;
        const float fuel = source.fuel * amount;

        // 動く発生源は、動く速さでも周りの煙を引きずる (motion.inheritVelocity)。
        const math::Vector3 target = source.velocity + pose.motionVelocity;
        const bool drivesVelocity = target.x != 0.0f || target.y != 0.0f || target.z != 0.0f;
        const float sourceSeed = static_cast<float>(sourceIndex) * 13.17f;
        for (int z = z0; z <= z1; ++z) {
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const float px = CellCenter(x, m_nx);
                    const float py = CellCenter(y, m_ny);
                    const float pz = CellCenter(z, m_nz);
                    const math::Vector3 p = { px, py, pz };
                    float weight = source.shape == FluidSourceShape::Texture
                        ? FluidTextureSourceWeight(source, pose.center, p, m_h, volumetric, m_sourceMasks[sourceIndex])
                        : FluidSourceWeight(source, pose.center, p, m_h, volumetric);
                    if (weight <= 0.0f) continue;
                    if (source.noise > 0.0f) {
                        const float n = core::ValueNoise3D({ px * 5.0f + m_noiseOffset.x + sourceSeed,
                                                             py * 5.0f + m_noiseOffset.y - m_time * 1.3f,
                                                             pz * 5.0f + m_noiseOffset.z });
                        weight *= (std::max)(0.0f, 1.0f + source.noise * n);
                    }
                    const std::size_t i = Index(x, y, z);
                    // 色は «注いだ量 × 鍵» を煙と燃料の両方へ入れる。燃料の色は、後で燃えて生まれる
                    // 煤がどの発生源から来たかを覚えておくためのもの (煙を注がない炎でも色が付く)。
                    m_density[i]     = (std::min)(m_density[i] + density * weight * dt, kMaxDensity);
                    if (m_hasColor)
                        m_colorMass[i] = (std::min)(m_colorMass[i] + density * weight * dt * source.colorKey,
                                                    kMaxDensity);
                    m_temperature[i] = (std::min)(m_temperature[i] + temperature * weight * dt, kMaxTemperature);
                    m_fuel[i]        = (std::max)(0.0f, m_fuel[i] + fuel * weight * dt);
                    if (m_hasColor)
                        m_fuelColorMass[i] = std::clamp(
                            m_fuelColorMass[i] + fuel * weight * dt * source.colorKey, 0.0f, kMaxDensity);
                    if (drivesVelocity) {
                        const float blend = Saturate(weight * 10.0f * dt);
                        m_vx[i] += (target.x - m_vx[i]) * blend;
                        m_vy[i] += (target.y - m_vy[i]) * blend;
                        if (volumetric) m_vz[i] += (target.z - m_vz[i]) * blend;
                    }
                }
            }
        }
    }
}

void FluidGasSolver::Burn(float dt)
{
    std::fill(m_expansion.begin(), m_expansion.end(), 0.0f);
    const float fraction = Saturate((std::max)(m_settings.burnRate, 0.0f) * dt);
    if (fraction <= 0.0f) return;
    for (std::size_t i = 0; i < m_fuel.size(); ++i) {
        if (m_fuel[i] <= 1.0e-5f || m_temperature[i] < m_settings.ignitionTemperature) continue;
        const float burned = m_fuel[i] * fraction;
        // 煤は «燃えた燃料の鍵» だけで生まれる (その場に既にある煙の鍵は見ない)。鍵は燃料を減らす前に
        // 測り、燃えた分だけ燃料の色も連れて行くので、残った燃料の鍵は変わらない。
        // WHY その場の煙へ寄せないか: 色の付いた燃料が薄く広がった所で鍵が跳ぶ。煙は自分の色のまま
        //     残り、セルの鍵は «量で重み付けした平均» として自然に混ざる (GPU の写しも同じ規則)。
        const float fuelKey = m_hasColor ? ColorKeyOf(m_fuelColorMass[i], m_fuel[i]) : 0.0f;
        m_fuel[i] -= burned;
        if (m_hasColor) m_fuelColorMass[i] = (std::max)(m_fuelColorMass[i] - burned * fuelKey, 0.0f);
        m_temperature[i] = (std::min)(m_temperature[i] + burned * m_settings.burnHeat, kMaxTemperature);
        m_density[i]     = (std::min)(m_density[i] + burned * m_settings.burnSmoke, kMaxDensity);
        if (m_hasColor)
            m_colorMass[i] = (std::min)(m_colorMass[i] + burned * m_settings.burnSmoke * fuelKey, kMaxDensity);
        // 燃えた分だけガスが膨らむ。圧力解法へ «湧き出し» として渡すと、外向きの爆風になる。
        m_expansion[i] = burned * m_settings.burnExpansion / dt;
    }
}

void FluidGasSolver::ApplyForces(float dt)
{
    const bool volumetric = m_nz > 1;
    const float damping = std::exp(-(std::max)(m_settings.velocityDamping, 0.0f) * dt);
    const float turbulence = m_settings.turbulence;
    const float scale = (std::max)(m_settings.turbulenceScale, 0.01f);

    // 力の中心・効いているか・量の倍率は刻みの始めの時刻で 1 回だけ決める (GPU は刻みごとに CPU で詰める)。
    struct ActiveForce {
        std::size_t   index = 0;
        math::Vector3 center = { 0.0f, 0.0f, 0.0f };
        float         amount = 1.0f;
    };
    std::array<ActiveForce, kMaxFluidForces> active{};
    int activeCount = 0;
    for (std::size_t f = 0; f < m_forces.size() && activeCount < kMaxFluidForces; ++f) {
        if (!FluidForceActive(m_forces[f], m_time)) continue;
        active[static_cast<std::size_t>(activeCount)].index = f;
        active[static_cast<std::size_t>(activeCount)].center = PoseFluidForce(m_forces[f], m_time).center;
        active[static_cast<std::size_t>(activeCount)].amount = FluidForceAmount(m_forces[f], m_time);
        ++activeCount;
    }

    for (int z = 0; z < m_nz; ++z) {
        for (int y = 0; y < m_ny; ++y) {
            for (int x = 0; x < m_nx; ++x) {
                const std::size_t i = Index(x, y, z);
                m_vy[i] += (m_settings.buoyancy * m_temperature[i] - m_settings.weight * m_density[i]) * dt;
                m_vx[i] += m_settings.wind.x * dt;
                m_vy[i] += m_settings.wind.y * dt;
                if (volumetric) m_vz[i] += m_settings.wind.z * dt;
                if (turbulence != 0.0f) {
                    // 時間でゆっくりずらして «止まった渦» にしない。
                    const math::Vector3 curl = core::CurlNoise({
                        CellCenter(x, m_nx) * scale + m_noiseOffset.x,
                        CellCenter(y, m_ny) * scale + m_noiseOffset.y - m_time * 0.35f,
                        CellCenter(z, m_nz) * scale + m_noiseOffset.z + m_time * 0.2f });
                    m_vx[i] += curl.x * turbulence * dt;
                    m_vy[i] += curl.y * turbulence * dt;
                    if (volumetric) m_vz[i] += curl.z * turbulence * dt;
                }
                if (activeCount > 0) {
                    // 前の力を足した後の速度を次の力が見る (Drag の順番の意味を GPU と揃える)。
                    const math::Vector3 p = { CellCenter(x, m_nx), CellCenter(y, m_ny), CellCenter(z, m_nz) };
                    math::Vector3 v = { m_vx[i], m_vy[i], m_vz[i] };
                    for (int k = 0; k < activeCount; ++k) {
                        const ActiveForce& force = active[static_cast<std::size_t>(k)];
                        v += FluidForceDelta(m_forces[force.index], force.center, p, v, m_time, dt, m_noiseOffset,
                                             static_cast<int>(force.index), volumetric, force.amount);
                    }
                    m_vx[i] = v.x;
                    m_vy[i] = v.y;
                    if (volumetric) m_vz[i] = v.z;
                }
                m_vx[i] *= damping;
                m_vy[i] *= damping;
                m_vz[i] *= damping;
            }
        }
    }
    if (m_settings.vorticity > 0.0f) ApplyVorticityConfinement(dt);
}

// Fedkiw et al. 2001。数値拡散で失われる細かい渦を、渦度の強い方へ押し戻す力で補う。
void FluidGasSolver::ApplyVorticityConfinement(float dt)
{
    for (int z = 0; z < m_nz; ++z) {
        for (int y = 0; y < m_ny; ++y) {
            for (int x = 0; x < m_nx; ++x) {
                const std::size_t i = Index(x, y, z);
                const float wx = Derivative(m_vz, x, y, z, 1) - Derivative(m_vy, x, y, z, 2);
                const float wy = Derivative(m_vx, x, y, z, 2) - Derivative(m_vz, x, y, z, 0);
                const float wz = Derivative(m_vy, x, y, z, 0) - Derivative(m_vx, x, y, z, 1);
                m_curlX[i] = wx;
                m_curlY[i] = wy;
                m_curlZ[i] = wz;
                m_curlLength[i] = std::sqrt(wx * wx + wy * wy + wz * wz);
            }
        }
    }
    const float strength = m_settings.vorticity * m_h;
    const bool volumetric = m_nz > 1;
    for (int z = 0; z < m_nz; ++z) {
        for (int y = 0; y < m_ny; ++y) {
            for (int x = 0; x < m_nx; ++x) {
                const float ex = Derivative(m_curlLength, x, y, z, 0);
                const float ey = Derivative(m_curlLength, x, y, z, 1);
                const float ez = Derivative(m_curlLength, x, y, z, 2);
                const float length = std::sqrt(ex * ex + ey * ey + ez * ez);
                if (length < 1.0e-6f) continue;
                const float nx = ex / length;
                const float ny = ey / length;
                const float nz = ez / length;
                const std::size_t i = Index(x, y, z);
                // f = ε h (N × ω)
                m_vx[i] += (ny * m_curlZ[i] - nz * m_curlY[i]) * strength * dt;
                m_vy[i] += (nz * m_curlX[i] - nx * m_curlZ[i]) * strength * dt;
                if (volumetric) m_vz[i] += (nx * m_curlY[i] - ny * m_curlX[i]) * strength * dt;
            }
        }
    }
}

void FluidGasSolver::Project(const std::vector<float>* expansion)
{
    for (int z = 0; z < m_nz; ++z)
        for (int y = 0; y < m_ny; ++y)
            for (int x = 0; x < m_nx; ++x) {
                const std::size_t i = Index(x, y, z);
                m_divergence[i] = Divergence(x, y, z)
                    - (expansion != nullptr ? (*expansion)[i] : 0.0f);
            }

    // 境界: 開いた縁は p = 0 (流れが自由に出ていく)、床だけは ∂p/∂n = 0 (通り抜けない)。
    // 障害物 (固体セル) も床と同じ ∂p/∂n = 0。固体セル自身は解かない (速度は障害物が決める)。
    // 前の刻みの圧力を初期値に残す (warm start) と、同じ反復回数でも収束がずっと進む。
    const bool solids = m_hasSolid;
    const float h2 = m_h * m_h;
    constexpr float kOverRelaxation = 1.7f;
    const int iterations = std::clamp(m_settings.pressureIterations, 1, 400);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int color = 0; color < 2; ++color) {
            for (int z = 0; z < m_nz; ++z) {
                for (int y = 0; y < m_ny; ++y) {
                    for (int x = ((y + z + color) & 1); x < m_nx; x += 2) {
                        const std::size_t i = Index(x, y, z);
                        if (solids && m_solid[i] != 0) continue;
                        float sum = 0.0f;
                        int count = 0;
                        const auto neighbor = [&](int ax, int ay, int az, bool solid) {
                            if (ax >= 0 && ax < m_nx && ay >= 0 && ay < m_ny && az >= 0 && az < m_nz) {
                                const std::size_t j = Index(ax, ay, az);
                                if (solids && m_solid[j] != 0) return; // 固体の隣は数えない (床と同じ)
                                sum += m_pressure[j];
                                ++count;
                            } else if (!solid) {
                                ++count; // p = 0 の外側
                            }
                        };
                        if (m_nx > 1) { neighbor(x - 1, y, z, false); neighbor(x + 1, y, z, false); }
                        if (m_ny > 1) { neighbor(x, y - 1, z, m_settings.floor); neighbor(x, y + 1, z, false); }
                        if (m_nz > 1) { neighbor(x, y, z - 1, false); neighbor(x, y, z + 1, false); }
                        if (count == 0) continue;
                        const float target = (sum - h2 * m_divergence[i]) / static_cast<float>(count);
                        m_pressure[i] += kOverRelaxation * (target - m_pressure[i]);
                    }
                }
            }
        }
    }

    const float inverseTwoH = 0.5f / m_h;
    const auto pressureAt = [&](int x, int y, int z, float self, bool solid) {
        if (x >= 0 && x < m_nx && y >= 0 && y < m_ny && z >= 0 && z < m_nz) {
            const std::size_t j = Index(x, y, z);
            return solids && m_solid[j] != 0 ? self : m_pressure[j];
        }
        return solid ? self : 0.0f;
    };
    for (int z = 0; z < m_nz; ++z) {
        for (int y = 0; y < m_ny; ++y) {
            for (int x = 0; x < m_nx; ++x) {
                const std::size_t i = Index(x, y, z);
                if (solids && m_solid[i] != 0) continue;
                const float self = m_pressure[i];
                if (m_nx > 1)
                    m_vx[i] -= (pressureAt(x + 1, y, z, self, false) - pressureAt(x - 1, y, z, self, false)) * inverseTwoH;
                if (m_ny > 1)
                    m_vy[i] -= (pressureAt(x, y + 1, z, self, false) - pressureAt(x, y - 1, z, self, m_settings.floor)) * inverseTwoH;
                if (m_nz > 1)
                    m_vz[i] -= (pressureAt(x, y, z + 1, self, false) - pressureAt(x, y, z - 1, self, false)) * inverseTwoH;
            }
        }
    }
    if (m_settings.floor) {
        for (int z = 0; z < m_nz; ++z)
            for (int x = 0; x < m_nx; ++x) {
                float& vy = m_vy[Index(x, 0, z)];
                vy = (std::max)(vy, 0.0f);
            }
    }
    if (solids) EnforceSolids(false);
}

void FluidGasSolver::Advect(float dt)
{
    std::copy(m_vx.begin(), m_vx.end(), m_prevVx.begin());
    std::copy(m_vy.begin(), m_vy.end(), m_prevVy.begin());
    std::copy(m_vz.begin(), m_vz.end(), m_prevVz.begin());

    AdvectScalar(m_density, dt);
    AdvectScalar(m_temperature, dt);
    AdvectScalar(m_fuel, dt);
    if (m_hasColor) {
        AdvectScalar(m_colorMass, dt);
        AdvectScalar(m_fuelColorMass, dt);
    }

    // 速度の自己移流は半ラグランジュのまま。MacCormack を掛けると渦の芯で振動しやすく、
    // 形の鋭さはスカラー側で十分に出る。
    const float cellsPerUnit = dt / m_h;
    for (int z = 0; z < m_nz; ++z) {
        for (int y = 0; y < m_ny; ++y) {
            for (int x = 0; x < m_nx; ++x) {
                const std::size_t i = Index(x, y, z);
                const float gx = static_cast<float>(x) - m_prevVx[i] * cellsPerUnit;
                const float gy = static_cast<float>(y) - m_prevVy[i] * cellsPerUnit;
                const float gz = m_nz > 1 ? static_cast<float>(z) - m_prevVz[i] * cellsPerUnit : 0.0f;
                m_vx[i] = SampleGrid(m_prevVx, gx, gy, gz);
                m_vy[i] = SampleGrid(m_prevVy, gx, gy, gz);
                if (m_nz > 1) m_vz[i] = SampleGrid(m_prevVz, gx, gy, gz);
            }
        }
    }
}

void FluidGasSolver::AdvectScalar(std::vector<float>& field, float dt)
{
    const float cellsPerUnit = dt / m_h;
    const auto backtrace = [&](std::size_t i, int x, int y, int z, float sign, float& gx, float& gy, float& gz) {
        gx = static_cast<float>(x) - sign * m_prevVx[i] * cellsPerUnit;
        gy = static_cast<float>(y) - sign * m_prevVy[i] * cellsPerUnit;
        gz = m_nz > 1 ? static_cast<float>(z) - sign * m_prevVz[i] * cellsPerUnit : 0.0f;
    };

    float gx = 0.0f, gy = 0.0f, gz = 0.0f;
    for (int z = 0; z < m_nz; ++z)
        for (int y = 0; y < m_ny; ++y)
            for (int x = 0; x < m_nx; ++x) {
                const std::size_t i = Index(x, y, z);
                backtrace(i, x, y, z, 1.0f, gx, gy, gz);
                m_scratchA[i] = SampleGrid(field, gx, gy, gz);
            }
    if (!m_settings.sharpAdvection) {
        field.swap(m_scratchA);
        return;
    }

    // MacCormack: 往復させて戻ってこなかった分 (= 数値拡散の見積もり) を半分足し戻す。
    for (int z = 0; z < m_nz; ++z)
        for (int y = 0; y < m_ny; ++y)
            for (int x = 0; x < m_nx; ++x) {
                const std::size_t i = Index(x, y, z);
                backtrace(i, x, y, z, -1.0f, gx, gy, gz);
                m_scratchB[i] = SampleGrid(m_scratchA, gx, gy, gz);
            }
    for (int z = 0; z < m_nz; ++z)
        for (int y = 0; y < m_ny; ++y)
            for (int x = 0; x < m_nx; ++x) {
                const std::size_t i = Index(x, y, z);
                const float corrected = m_scratchA[i] + 0.5f * (field[i] - m_scratchB[i]);
                // 補正で元の近傍の範囲を超えると振動 (縞) が出る。範囲へ押し込めて抑える。
                backtrace(i, x, y, z, 1.0f, gx, gy, gz);
                float lo = 0.0f;
                float hi = 0.0f;
                MinMaxAround(field, gx, gy, gz, lo, hi);
                m_scratchC[i] = std::clamp(corrected, lo, hi);
            }
    field.swap(m_scratchC);
}

void FluidGasSolver::Dissipate(float dt)
{
    const float densityKeep     = std::exp(-(std::max)(m_settings.densityDissipation, 0.0f) * dt);
    const float temperatureKeep = std::exp(-(std::max)(m_settings.temperatureDissipation, 0.0f) * dt);
    for (std::size_t i = 0; i < m_density.size(); ++i) {
        m_density[i] *= densityKeep;
        m_temperature[i] *= temperatureKeep;
        // 非正規化数へ落ちると演算が桁違いに遅くなる。見えない量は 0 にしてしまう。
        if (m_density[i] < 1.0e-6f) m_density[i] = 0.0f;
        if (m_temperature[i] < 1.0e-6f) m_temperature[i] = 0.0f;
        if (m_hasColor) {
            m_colorMass[i] *= densityKeep;
            if (m_colorMass[i] < 1.0e-6f) m_colorMass[i] = 0.0f;
        }
    }
}

void FluidGasSolver::UpdateColorKey()
{
    if (!m_hasColor) return;
    for (std::size_t i = 0; i < m_density.size(); ++i)
        m_colorKey[i] = ColorKeyOf(m_colorMass[i], m_density[i]);
}

} // namespace fbzz::asset
