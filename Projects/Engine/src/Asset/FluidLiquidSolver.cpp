/// @file    FluidLiquidSolver.cpp
/// @brief   粒子液体ソルバー (Position Based Fluids + XSPH 粘性)。2D と 3D を同じコードで解く
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 2D のときは z と vz が常に 0 のまま進む (3D の項は 0 を足すだけ)。
#include <Engine/Asset/FluidSolver.hpp>

#include <Engine/Asset/FluidOperatorEval.hpp>
#include <Engine/Core/CurlNoise.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::asset {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// 近傍格子が覆う範囲。これより外へ出た粒子は画面外なので捨てる。
constexpr float kBoundsMinX = -1.6f;
constexpr float kBoundsMaxX =  1.6f;
constexpr float kBoundsMinY = -1.6f;
constexpr float kBoundsMaxY =  3.0f;
constexpr float kBoundsMinZ = -1.6f;
constexpr float kBoundsMaxZ =  1.6f;
constexpr int   kParticleHardLimit = 20000;
// 引張不安定の補正 (Macklin & Müller の s_corr)。粒子が 2 つずつ団子になるのを防ぐ。
// WHY 論文の k = 0.1 をそのまま使わないか: 論文の値は λ が O(1) になる単位系での話で、
//     ここの単位系では λ が 1e-4 程度になる。静止時の勾配和で割って «λ と同じ単位» に揃える。
constexpr float kTensileStrength = 0.05f;
// 密度拘束の押し出しが 1 刻みで速度へ持ち越せる上限 [領域単位/秒]。
constexpr float kMaxPushSpeed = 2.0f;

[[nodiscard]] float Saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

// 刻みの始めの時刻で効いている力と、その時刻の中心・量の倍率。
struct ActiveForce {
    std::size_t   index = 0;
    math::Vector3 center = { 0.0f, 0.0f, 0.0f };
    float         amount = 1.0f;
};

// Texture の点をマスクで選り分けるときの引き直しの上限。これで外れ続けるのは «ほぼ真っ黒な画像» だけ。
constexpr int kTextureEmitAttempts = 16;
// 障害物に «触れている» とみなす距離 (粒子半径の倍率)。床の接触と同じ幅。
constexpr float kColliderContactScale = 1.05f;

void LoadSourceMasks(const std::vector<FluidSource>& sources, std::vector<FluidSourceMask>& outMasks)
{
    outMasks.assign(sources.size(), FluidSourceMask{});
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].shape != FluidSourceShape::Texture || sources[i].texture.empty()) continue;
        if (!LoadFluidSourceMask(sources[i].texture, outMasks[i])) outMasks[i].values.clear();
    }
}

} // namespace

// 部品 (発生源・力・障害物) とその画像だけを取り込む。Reset と ReplaceOperators で共通。
// m_volumetric と m_radius が決まっていること (箱の奥行きと寸法の広げ方がそれで決まる)。
void FluidLiquidSolver::AdoptOperators(const FluidRecipe& recipe)
{
    m_sources.clear();
    for (const FluidSource& source : recipe.sources)
        if (source.enabled && m_sources.size() < static_cast<std::size_t>(kMaxFluidSources))
            m_sources.push_back(source);
    // WHY 粒子半径まで広げるか: それより細い寸法は 1 粒の幅にもならない。0 の軸が残ると形の体積が 0 になり、
    //     FluidLiquidEmitScale が «詰まりすぎ» を広げられずに全粒を 1 点へ出してしまう。
    for (FluidSource& source : m_sources) {
        source.size.x = (std::max)(source.size.x, m_radius);
        source.size.y = (std::max)(source.size.y, m_radius);
        source.size.z = (std::max)(source.size.z, m_radius);
    }
    m_forces.clear();
    for (const FluidForce& force : recipe.forces)
        if (force.enabled && m_forces.size() < static_cast<std::size_t>(kMaxFluidForces))
            m_forces.push_back(force);
    m_colliders.clear();
    for (const FluidCollider& collider : recipe.colliders) {
        if (!collider.enabled || m_colliders.size() >= static_cast<std::size_t>(kMaxFluidColliders)) continue;
        m_colliders.push_back(collider);
        // WHY 2D の箱の奥行きを無限にするか: 2D は d.z = 0 で測るので、箱の z の半分の大きさが x/y の深さより
        //     小さいと «中» の距離と法線が z 軸で決まり、粒子を画面の外 (z) へ押し出そうとしてしまう。
        if (!m_volumetric && collider.shape == FluidColliderShape::Box) m_colliders.back().size.z = 1.0e6f;
    }
    m_activeColliders.clear();
    LoadSourceMasks(m_sources, m_sourceMasks);
    // 撃ち出し済みの数は «有効な発生源の中での添字» で引き継ぐ。増えた分は 0 から、減った分は捨てる。
    m_emitted.resize(m_sources.size(), 0);
}

void FluidLiquidSolver::Reset(const FluidRecipe& recipe, bool volumetric)
{
    m_settings = recipe.liquid;
    m_volumetric = volumetric;
    m_radius = std::clamp(m_settings.particleRadius, 0.002f, 0.1f);
    m_emitted.clear();
    AdoptOperators(recipe);
    m_particles.clear();
    m_time = 0.0f;
    m_rngState = core::PcgHash(recipe.seed * 747796405u + 2891336453u) | 1u;

    m_h  = m_radius * 4.0f;
    m_h2 = m_h * m_h;
    // Poly6 / Spiky の係数は次元で違う。流用すると静止密度が狂い、液面が縮むか膨らむ。
    if (m_volumetric) {
        m_poly6         = 315.0f / (64.0f * kPi * std::pow(m_h, 9.0f));
        m_spikyGradient = -45.0f / (kPi * std::pow(m_h, 6.0f));
    } else {
        m_poly6         = 4.0f / (kPi * std::pow(m_h, 8.0f));
        m_spikyGradient = -30.0f / (kPi * std::pow(m_h, 5.0f));
    }

    // 静止密度は «粒子が直径の間隔で並んだ状態» で測る。解析的な値を置くと、
    // 格子の丸めの分だけ最初から圧縮 (または膨張) した状態で始まる。
    const float spacing = 2.0f * m_radius;
    const int reach = static_cast<int>(std::ceil(m_h / spacing));
    const int reachZ = m_volumetric ? reach : 0;
    float density = 0.0f;
    float gradientSquared = 0.0f;
    for (int k = -reachZ; k <= reachZ; ++k) {
        for (int j = -reach; j <= reach; ++j) {
            for (int i = -reach; i <= reach; ++i) {
                const float dx = static_cast<float>(i) * spacing;
                const float dy = static_cast<float>(j) * spacing;
                const float dz = static_cast<float>(k) * spacing;
                const float r2 = dx * dx + dy * dy + dz * dz;
                density += Poly6(r2);
                if (i == 0 && j == 0 && k == 0) continue;
                const float scale = SpikyGradientScale(std::sqrt(r2));
                gradientSquared += (scale * dx) * (scale * dx) + (scale * dy) * (scale * dy)
                                 + (scale * dz) * (scale * dz);
            }
        }
    }
    m_restDensity = (std::max)(density, 1.0e-6f);
    const float restGradientSum = (std::max)(gradientSquared / (m_restDensity * m_restDensity), 1.0e-12f);
    // 拘束をわずかに柔らかくする (CFM)。0 だと近傍が 1 つしか無い飛沫で分母が 0 に近づき跳ねる。
    m_relaxation = 0.01f * restGradientSum;
    m_tensileReference = (std::max)(Poly6((0.2f * m_h) * (0.2f * m_h)), 1.0e-12f);
    m_tensileScale = kTensileStrength / restGradientSum;

    m_cellsX = (std::max)(1, static_cast<int>(std::ceil((kBoundsMaxX - kBoundsMinX) / m_h)));
    m_cellsY = (std::max)(1, static_cast<int>(std::ceil((kBoundsMaxY - kBoundsMinY) / m_h)));
    m_cellsZ = m_volumetric ? (std::max)(1, static_cast<int>(std::ceil((kBoundsMaxZ - kBoundsMinZ) / m_h))) : 1;
}

// 粒子を残せる条件は «今居る粒子が、このレシピの粒子として通るか»。液体でなければ粒子ではなく、
// 粒子半径が変われば近傍格子・カーネル係数・静止密度が総取り替えになり、今の並びは «別の液体の途中» になる。
// 重力・粘性・まとまり・反復数・寿命は刻みごとに効くだけなので、途中から差し替えてよい。
bool FluidLiquidSolver::ReplaceOperators(const FluidRecipe& recipe)
{
    if (recipe.kind != FluidKind::Liquid) return false;
    if (std::clamp(recipe.liquid.particleRadius, 0.002f, 0.1f) != m_radius) return false;
    m_settings = recipe.liquid;
    AdoptOperators(recipe);
    return true;
}

float FluidLiquidSolver::Poly6(float distanceSquared) const
{
    if (distanceSquared >= m_h2) return 0.0f;
    const float d = m_h2 - distanceSquared;
    return m_poly6 * d * d * d;
}

// ∇W = scale × (p_i − p_j)。scale は負 (離れるほど W が減る)。
float FluidLiquidSolver::SpikyGradientScale(float distance) const
{
    if (distance <= 1.0e-7f || distance >= m_h) return 0.0f;
    const float d = m_h - distance;
    return m_spikyGradient * d * d / distance;
}

float FluidLiquidSolver::NextRandom()
{
    m_rngState = m_rngState * 1664525u + 1013904223u;
    return static_cast<float>(m_rngState >> 8) * (1.0f / 16777216.0f);
}

int FluidLiquidSolver::CellOf(float x, float y, float z) const
{
    const int cx = std::clamp(static_cast<int>((x - kBoundsMinX) / m_h), 0, m_cellsX - 1);
    const int cy = std::clamp(static_cast<int>((y - kBoundsMinY) / m_h), 0, m_cellsY - 1);
    const int cz = m_volumetric ? std::clamp(static_cast<int>((z - kBoundsMinZ) / m_h), 0, m_cellsZ - 1) : 0;
    return (cz * m_cellsY + cy) * m_cellsX + cx;
}

float FluidLiquidSolver::MeasuredDensity(std::size_t index) const
{
    return index < m_density.size() ? m_density[index] : 0.0f;
}

void FluidLiquidSolver::Emit()
{
    const std::size_t limit = static_cast<std::size_t>(
        std::clamp(m_settings.maxParticles, 1, kParticleHardLimit));
    for (std::size_t e = 0; e < m_sources.size(); ++e) {
        const FluidSource& source = m_sources[e];
        const int total = (std::max)(source.count, 0);
        int target = 0;
        if (source.duration <= 0.0f)
            target = m_time >= source.startTime ? total : 0;
        else
            target = static_cast<int>(static_cast<float>(total)
                                      * Saturate((m_time - source.startTime) / source.duration));
        target = std::clamp(target, 0, total);
        if (m_emitted[e] >= target || m_particles.size() >= limit) continue;

        const FluidOperatorPose pose = PoseFluidSource(source, m_time);
        math::Vector3 launch = source.velocity + pose.motionVelocity;
        // 2D は奥行きの速度を見ない (レシピに z が書いてあっても平面の絵には関係しない)。
        if (!m_volumetric) launch.z = 0.0f;
        const float speed = launch.Length();
        // 詰まりすぎを避けて広げる倍率は FluidLiquidEmitScale が正本 (Inspector も同じ値を出す)。
        // 撃ち出す速度に動きの速さを含めて測るため、その速度を持った写しを渡す。
        FluidSource fitted = source;
        fitted.velocity = launch;
        const float scale = FluidLiquidEmitScale(fitted, m_settings, m_volumetric);
        const float jitterScale = Saturate(source.spread) * speed;
        while (m_emitted[e] < target && m_particles.size() < limit) {
            Particle particle;
            particle.colorKey = Saturate(source.colorKey);
            math::Vector3 point = pose.center;
            bool placed = false;
            if (source.shape == FluidSourceShape::Texture) {
                for (int attempt = 0; attempt < kTextureEmitAttempts && !placed; ++attempt) {
                    const float u0 = NextRandom();
                    const float u1 = NextRandom();
                    const float u2 = NextRandom();
                    const float accept = NextRandom();
                    placed = SampleFluidTextureSourcePoint(source, pose.center, m_sourceMasks[e], u0, u1, u2, accept,
                                                           m_volumetric, point);
                }
            }
            if (!placed) {
                const float u0 = NextRandom();
                const float u1 = NextRandom();
                const float u2 = NextRandom();
                point = SampleFluidSourcePoint(source, pose.center, u0, u1, u2, m_volumetric);
            }
            particle.x = pose.center.x + (point.x - pose.center.x) * scale;
            particle.y = pose.center.y + (point.y - pose.center.y) * scale;
            if (m_volumetric) {
                particle.z = pose.center.z + (point.z - pose.center.z) * scale;
                // ばらつきの向きは球面上で一様にする (円柱座標で z を一様に取ると球面一様になる)。
                const float cz = NextRandom() * 2.0f - 1.0f;
                const float angle = NextRandom() * 2.0f * kPi;
                const float ring = std::sqrt((std::max)(0.0f, 1.0f - cz * cz));
                const float jitter = NextRandom() * jitterScale;
                particle.vx = launch.x + ring * std::cos(angle) * jitter;
                particle.vy = launch.y + ring * std::sin(angle) * jitter;
                particle.vz = launch.z + cz * jitter;
            } else {
                const float angle  = NextRandom() * 2.0f * kPi;
                const float jitter = NextRandom() * jitterScale;
                particle.vx = launch.x + std::cos(angle) * jitter;
                particle.vy = launch.y + std::sin(angle) * jitter;
            }
            m_particles.push_back(particle);
            ++m_emitted[e];
        }
    }
}

void FluidLiquidSolver::Advance(float dt)
{
    if (dt <= 0.0f) return;
    float maxSpeed = 0.0f;
    for (const Particle& particle : m_particles)
        maxSpeed = (std::max)(maxSpeed, std::sqrt(particle.vx * particle.vx + particle.vy * particle.vy
                                                  + particle.vz * particle.vz));
    for (const FluidSource& source : m_sources) {
        math::Vector3 launch = source.velocity + PoseFluidSource(source, m_time).motionVelocity;
        if (!m_volumetric) launch.z = 0.0f;
        maxSpeed = (std::max)(maxSpeed, launch.Length() * (1.0f + Saturate(source.spread)));
    }
    maxSpeed += (std::max)(m_settings.gravity, 0.0f) * dt;
    // 力で速くなる分も 1 刻みの移動に入れる。Drag は遅くするだけなので数えない。
    // 倍率を掛けた実際の強さで見積もる (倍率が 1 を超える間だけ刻みが足りなくなるのを防ぐ)。
    for (const FluidForce& force : m_forces)
        if (force.type != FluidForceType::Drag && FluidForceActive(force, m_time))
            maxSpeed += std::fabs(force.strength * FluidForceAmount(force, m_time)) * dt;
    // 1 刻みで粒子半径まで。これを超えると近傍の取りこぼしで粒子がすり抜ける。
    const int steps = std::clamp(static_cast<int>(std::ceil(maxSpeed * dt / m_radius)), 1, 48);
    const float stepDt = dt / static_cast<float>(steps);
    for (int step = 0; step < steps; ++step) Step(stepDt);
}

void FluidLiquidSolver::Step(float dt)
{
    if (dt <= 0.0f) return;
    Emit();
    if (m_settings.particleLifetime > 0.0f) {
        const float lifetime = m_settings.particleLifetime;
        std::erase_if(m_particles, [lifetime](const Particle& p) { return p.age >= lifetime; });
    }

    const std::size_t count = m_particles.size();
    m_predictedX.resize(count);
    m_predictedY.resize(count);
    m_predictedZ.resize(count);
    m_lambda.resize(count);
    m_density.resize(count);
    m_deltaX.resize(count);
    m_deltaY.resize(count);
    m_deltaZ.resize(count);

    std::array<ActiveForce, kMaxFluidForces> active{};
    int activeCount = 0;
    for (std::size_t f = 0; f < m_forces.size() && activeCount < kMaxFluidForces; ++f) {
        if (!FluidForceActive(m_forces[f], m_time)) continue;
        active[static_cast<std::size_t>(activeCount)].index = f;
        active[static_cast<std::size_t>(activeCount)].center = PoseFluidForce(m_forces[f], m_time).center;
        active[static_cast<std::size_t>(activeCount)].amount = FluidForceAmount(m_forces[f], m_time);
        ++activeCount;
    }
    const math::Vector3 noNoiseOffset = { 0.0f, 0.0f, 0.0f };

    m_activeColliders.clear();
    for (std::size_t c = 0; c < m_colliders.size(); ++c) {
        if (!FluidColliderActive(m_colliders[c], m_time)) continue;
        const FluidOperatorPose pose = PoseFluidCollider(m_colliders[c], m_time);
        ActiveCollider entry;
        entry.index = c;
        entry.center = pose.center;
        entry.velocity = pose.motionVelocity;
        if (!m_volumetric) entry.velocity.z = 0.0f;
        m_activeColliders.push_back(entry);
    }

    for (std::size_t i = 0; i < count; ++i) {
        Particle& particle = m_particles[i];
        particle.vy -= m_settings.gravity * dt;
        if (activeCount > 0) {
            const math::Vector3 p = { particle.x, particle.y, particle.z };
            math::Vector3 v = { particle.vx, particle.vy, particle.vz };
            for (int k = 0; k < activeCount; ++k) {
                const ActiveForce& force = active[static_cast<std::size_t>(k)];
                v += FluidForceDelta(m_forces[force.index], force.center, p, v, m_time, dt, noNoiseOffset,
                                     static_cast<int>(force.index), m_volumetric, force.amount);
            }
            particle.vx = v.x;
            particle.vy = v.y;
            particle.vz = v.z;
        }
        m_predictedX[i] = particle.x + particle.vx * dt;
        m_predictedY[i] = particle.y + particle.vy * dt;
        m_predictedZ[i] = particle.z + particle.vz * dt;
    }

    BuildNeighbors();
    const int iterations = std::clamp(m_settings.solverIterations, 1, 20);
    for (int iteration = 0; iteration < iterations; ++iteration) SolveDensity();

    const float floorContact = m_settings.floorHeight + m_radius * 1.05f;
    const float frictionKeep = std::exp(-(std::max)(m_settings.floorFriction, 0.0f) * 10.0f * dt);
    for (std::size_t i = 0; i < count; ++i) {
        Particle& particle = m_particles[i];
        // WHY 押し出しの速さに上限を置くか: PBF は位置の補正をそのまま速度にする。詰まった粒子を
        //     押し広げた補正まで速度になると、重なりや激突のたびに粒子が爆ぜる。位置は補正どおり動かし、
        //     速度へ持ち越す分だけを抑える (流れの速さは発生源と重力が決める)。
        float pushX = (m_predictedX[i] - particle.x) / dt - particle.vx;
        float pushY = (m_predictedY[i] - particle.y) / dt - particle.vy;
        float pushZ = (m_predictedZ[i] - particle.z) / dt - particle.vz;
        const float push = std::sqrt(pushX * pushX + pushY * pushY + pushZ * pushZ);
        if (push > kMaxPushSpeed) {
            const float shrink = kMaxPushSpeed / push;
            pushX *= shrink;
            pushY *= shrink;
            pushZ *= shrink;
        }
        particle.vx += pushX;
        particle.vy += pushY;
        particle.vz += pushZ;
        particle.x  = m_predictedX[i];
        particle.y  = m_predictedY[i];
        particle.z  = m_predictedZ[i];
        if (m_settings.floor && particle.y <= floorContact) {
            particle.vx *= frictionKeep;
            particle.vz *= frictionKeep;
            particle.vy = (std::max)(particle.vy, 0.0f);
        }
        // 床と同じ扱いを障害物の面で行う。速度は障害物に対する相対で見る (動く障害物に乗った粒は一緒に動く)。
        for (const ActiveCollider& contact : m_activeColliders) {
            const FluidCollider& collider = m_colliders[contact.index];
            const math::Vector3 p = { particle.x, particle.y, particle.z };
            if (FluidColliderDistance(collider, contact.center, p, 0.0f, m_volumetric) > m_radius * kColliderContactScale)
                continue;
            const math::Vector3 n = FluidColliderNormal(collider, contact.center, p, 0.0f, m_volumetric);
            const float keep = std::exp(-(std::max)(collider.friction, 0.0f) * 10.0f * dt);
            const float rx = particle.vx - contact.velocity.x;
            const float ry = particle.vy - contact.velocity.y;
            const float rz = particle.vz - contact.velocity.z;
            const float normalSpeed = rx * n.x + ry * n.y + rz * n.z;
            const float kept = (std::max)(normalSpeed, 0.0f);
            particle.vx = contact.velocity.x + n.x * kept + (rx - n.x * normalSpeed) * keep;
            particle.vy = contact.velocity.y + n.y * kept + (ry - n.y * normalSpeed) * keep;
            particle.vz = contact.velocity.z + n.z * kept + (rz - n.z * normalSpeed) * keep;
        }
    }
    ApplyViscosity();

    for (Particle& particle : m_particles) particle.age += dt;
    std::erase_if(m_particles, [](const Particle& p) {
        return p.x < kBoundsMinX || p.x > kBoundsMaxX || p.y < kBoundsMinY || p.y > kBoundsMaxY
            || p.z < kBoundsMinZ || p.z > kBoundsMaxZ;
    });
    m_time += dt;
}

void FluidLiquidSolver::BuildNeighbors()
{
    const std::size_t count = m_particles.size();
    const int cellCount = m_cellsX * m_cellsY * m_cellsZ;
    m_cellStart.assign(static_cast<std::size_t>(cellCount) + 1, 0);
    m_particleCell.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const int cell = CellOf(m_predictedX[i], m_predictedY[i], m_predictedZ[i]);
        m_particleCell[i] = cell;
        ++m_cellStart[static_cast<std::size_t>(cell) + 1];
    }
    for (int cell = 0; cell < cellCount; ++cell)
        m_cellStart[static_cast<std::size_t>(cell) + 1] += m_cellStart[static_cast<std::size_t>(cell)];

    std::vector<int> cursor(m_cellStart.begin(), m_cellStart.end() - 1);
    m_cellParticles.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        m_cellParticles[static_cast<std::size_t>(cursor[static_cast<std::size_t>(m_particleCell[i])]++)] =
            static_cast<int>(i);

    // 近傍は反復の前に 1 回だけ組む (論文どおり)。反復中の移動は粒子半径より十分小さい。
    const int reachZ = m_volumetric ? 1 : 0;
    const int layer = m_cellsX * m_cellsY;
    m_neighborStart.resize(count + 1);
    m_neighbors.clear();
    for (std::size_t i = 0; i < count; ++i) {
        m_neighborStart[i] = static_cast<int>(m_neighbors.size());
        const int cz = m_particleCell[i] / layer;
        const int cy = (m_particleCell[i] % layer) / m_cellsX;
        const int cx = m_particleCell[i] % m_cellsX;
        for (int oz = -reachZ; oz <= reachZ; ++oz) {
            const int nz = cz + oz;
            if (nz < 0 || nz >= m_cellsZ) continue;
            for (int oy = -1; oy <= 1; ++oy) {
                const int ny = cy + oy;
                if (ny < 0 || ny >= m_cellsY) continue;
                for (int ox = -1; ox <= 1; ++ox) {
                    const int nx = cx + ox;
                    if (nx < 0 || nx >= m_cellsX) continue;
                    const std::size_t cell = static_cast<std::size_t>((nz * m_cellsY + ny) * m_cellsX + nx);
                    for (int k = m_cellStart[cell]; k < m_cellStart[cell + 1]; ++k) {
                        const int j = m_cellParticles[static_cast<std::size_t>(k)];
                        if (static_cast<std::size_t>(j) == i) continue;
                        const float dx = m_predictedX[i] - m_predictedX[static_cast<std::size_t>(j)];
                        const float dy = m_predictedY[i] - m_predictedY[static_cast<std::size_t>(j)];
                        const float dz = m_predictedZ[i] - m_predictedZ[static_cast<std::size_t>(j)];
                        if (dx * dx + dy * dy + dz * dz < m_h2) m_neighbors.push_back(j);
                    }
                }
            }
        }
    }
    m_neighborStart[count] = static_cast<int>(m_neighbors.size());
}

void FluidLiquidSolver::SolveDensity()
{
    const std::size_t count = m_particles.size();
    const float inverseRest = 1.0f / m_restDensity;
    const float cohesion = Saturate(m_settings.cohesion);

    for (std::size_t i = 0; i < count; ++i) {
        float density = Poly6(0.0f);
        float gradientX = 0.0f;
        float gradientY = 0.0f;
        float gradientZ = 0.0f;
        float gradientSquared = 0.0f;
        for (int k = m_neighborStart[i]; k < m_neighborStart[i + 1]; ++k) {
            const std::size_t j = static_cast<std::size_t>(m_neighbors[static_cast<std::size_t>(k)]);
            const float dx = m_predictedX[i] - m_predictedX[j];
            const float dy = m_predictedY[i] - m_predictedY[j];
            const float dz = m_predictedZ[i] - m_predictedZ[j];
            const float r2 = dx * dx + dy * dy + dz * dz;
            density += Poly6(r2);
            const float scale = SpikyGradientScale(std::sqrt(r2)) * inverseRest;
            gradientX += scale * dx;
            gradientY += scale * dy;
            gradientZ += scale * dz;
            gradientSquared += (scale * dx) * (scale * dx) + (scale * dy) * (scale * dy)
                             + (scale * dz) * (scale * dz);
        }
        m_density[i] = density;
        float constraint = density * inverseRest - 1.0f;
        // 負 (まばら) の側は «引き寄せ» になる。cohesion で効きを絞り、0 なら飛沫はばらけたまま。
        if (constraint < 0.0f) constraint *= cohesion;
        m_lambda[i] = -constraint
            / (gradientX * gradientX + gradientY * gradientY + gradientZ * gradientZ + gradientSquared
               + m_relaxation);
    }

    // 1 反復の補正を粒子半径の半分までに抑える。発生直後の重なりで粒子が弾け飛ばないように。
    const float maxCorrection = m_radius * 0.5f;
    for (std::size_t i = 0; i < count; ++i) {
        float sumX = 0.0f;
        float sumY = 0.0f;
        float sumZ = 0.0f;
        for (int k = m_neighborStart[i]; k < m_neighborStart[i + 1]; ++k) {
            const std::size_t j = static_cast<std::size_t>(m_neighbors[static_cast<std::size_t>(k)]);
            const float dx = m_predictedX[i] - m_predictedX[j];
            const float dy = m_predictedY[i] - m_predictedY[j];
            const float dz = m_predictedZ[i] - m_predictedZ[j];
            const float r2 = dx * dx + dy * dy + dz * dz;
            const float scale = SpikyGradientScale(std::sqrt(r2));
            if (scale == 0.0f) continue;
            const float ratio = Poly6(r2) / m_tensileReference;
            const float tensile = -m_tensileScale * ratio * ratio * ratio * ratio;
            const float coefficient = (m_lambda[i] + m_lambda[j] + tensile) * scale * inverseRest;
            sumX += coefficient * dx;
            sumY += coefficient * dy;
            sumZ += coefficient * dz;
        }
        const float length = std::sqrt(sumX * sumX + sumY * sumY + sumZ * sumZ);
        if (length > maxCorrection) {
            const float shrink = maxCorrection / length;
            sumX *= shrink;
            sumY *= shrink;
            sumZ *= shrink;
        }
        m_deltaX[i] = sumX;
        m_deltaY[i] = sumY;
        m_deltaZ[i] = sumZ;
    }

    const float floorLimit = m_settings.floorHeight + m_radius;
    for (std::size_t i = 0; i < count; ++i) {
        m_predictedX[i] += m_deltaX[i];
        m_predictedY[i] += m_deltaY[i];
        m_predictedZ[i] += m_deltaZ[i];
        if (m_settings.floor && m_predictedY[i] < floorLimit) m_predictedY[i] = floorLimit;
        // 障害物の表面から粒子半径ぶん外へ出す。反復ごとに行うので、最後の反復の後は必ず外に居る。
        for (const ActiveCollider& active : m_activeColliders) {
            const FluidCollider& collider = m_colliders[active.index];
            const math::Vector3 p = { m_predictedX[i], m_predictedY[i], m_predictedZ[i] };
            const float distance = FluidColliderDistance(collider, active.center, p, 0.0f, m_volumetric);
            if (distance >= m_radius) continue;
            const math::Vector3 n = FluidColliderNormal(collider, active.center, p, 0.0f, m_volumetric);
            const float push = m_radius - distance;
            m_predictedX[i] += n.x * push;
            m_predictedY[i] += n.y * push;
            m_predictedZ[i] += n.z * push;
        }
    }
}

// XSPH: 近傍の速度へ寄せる。Σ W / ρ0 ≈ 1 なので viscosity はそのまま «寄せる割合» になる。
void FluidLiquidSolver::ApplyViscosity()
{
    const float viscosity = Saturate(m_settings.viscosity);
    if (viscosity <= 0.0f) return;
    const std::size_t count = m_particles.size();
    const float inverseRest = 1.0f / m_restDensity;
    for (std::size_t i = 0; i < count; ++i) {
        float sumX = 0.0f;
        float sumY = 0.0f;
        float sumZ = 0.0f;
        for (int k = m_neighborStart[i]; k < m_neighborStart[i + 1]; ++k) {
            const std::size_t j = static_cast<std::size_t>(m_neighbors[static_cast<std::size_t>(k)]);
            const float dx = m_particles[i].x - m_particles[j].x;
            const float dy = m_particles[i].y - m_particles[j].y;
            const float dz = m_particles[i].z - m_particles[j].z;
            const float weight = Poly6(dx * dx + dy * dy + dz * dz) * inverseRest;
            sumX += (m_particles[j].vx - m_particles[i].vx) * weight;
            sumY += (m_particles[j].vy - m_particles[i].vy) * weight;
            sumZ += (m_particles[j].vz - m_particles[i].vz) * weight;
        }
        m_deltaX[i] = sumX * viscosity;
        m_deltaY[i] = sumY * viscosity;
        m_deltaZ[i] = sumZ * viscosity;
    }
    for (std::size_t i = 0; i < count; ++i) {
        m_particles[i].vx += m_deltaX[i];
        m_particles[i].vy += m_deltaY[i];
        m_particles[i].vz += m_deltaZ[i];
    }
}

} // namespace fbzz::asset
