/// @file    VolumeFlipbookFluid.cpp
/// @brief   3D 流体 → Volume Flipbook Baker の入力への変換と、裏で 1 コマずつ解くストリーム
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/VolumeFlipbookFluid.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace fbzz::asset {

void PackFluidVolume(const FluidGasSolver& solver, const FluidVolumeScale& scale, PackedFluidVolume& out)
{
    const int n = solver.SizeX();
    const std::size_t cells = static_cast<std::size_t>(n) * static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
    const std::vector<float>& density = solver.Density();
    const std::vector<float>& temperature = solver.Temperature();
    const std::vector<float>& vx = solver.VelocityX();
    const std::vector<float>& vy = solver.VelocityY();
    const std::vector<float>& vz = solver.VelocityZ();
    const std::vector<float>& colorKey = solver.ColorKey();
    if (n <= 0 || solver.SizeY() != n || solver.SizeZ() != n || density.size() < cells || temperature.size() < cells
        || vx.size() < cells || vy.size() < cells || vz.size() < cells) {
        out.resolution = 0;
        out.medium.clear();
        out.velocity.clear();
        return;
    }

    out.resolution = n;
    out.medium.resize(cells);
    out.velocity.resize(cells);
    const bool hasKey = colorKey.size() >= cells;
    for (std::size_t i = 0; i < cells; ++i) {
        out.medium[i] = { (std::max)(density[i], 0.0f) * scale.density,
                          (std::max)(temperature[i], 0.0f) * scale.temperature,
                          hasKey ? colorKey[i] : 0.0f, 0.0f };
        out.velocity[i] = { vx[i], vy[i], vz[i], 0.0f };
    }
}

void PackLiquidVolume(const FluidLiquidSolver& solver, int resolution, float radiusScale, float lifetime,
                      PackedFluidVolume& out)
{
    const int n = std::clamp(resolution, 1, 256);
    const std::size_t cells = static_cast<std::size_t>(n) * static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
    out.resolution = n;
    out.medium.assign(cells, { 0.0f, 0.0f, 0.0f, 0.0f });
    out.velocity.assign(cells, { 0.0f, 0.0f, 0.0f, 0.0f });
    std::vector<float> weights(cells, 0.0f);

    // bake 空間 [-1,1] の 1 辺を n セルに割る。セル中心は (i + 0.5) / n * 2 - 1。
    const float cellsPerUnit = static_cast<float>(n) * 0.5f;
    const float radius = solver.ParticleRadius() * (std::max)(radiusScale, 0.5f);
    // WHY 塗る半径に下限を置くか: 低い解像度 (プレビューや 16³) では粒子がセルより小さく、
    //     そのまま塗ると 1 セルにも届かず液体が丸ごと消える。半径をセル 1 つ分弱まで広げ、
    //     広げた分だけ量を減らして体積を保つ。
    constexpr float kMinSplatCells = 0.75f;
    for (const FluidLiquidSolver::Particle& particle : solver.Particles()) {
        // 寿命がある飛沫は細りながら消える。いきなり消すとコマ間でポツポツ抜けて見える (2D と同じ)。
        const float life = lifetime > 0.0f ? std::clamp(1.0f - particle.age / lifetime, 0.0f, 1.0f) : 1.0f;
        if (life <= 0.0f) continue;
        const float trueRadius = radius * std::sqrt(life) * cellsPerUnit;
        const float r = (std::max)(trueRadius, kMinSplatCells);
        const float shrink = trueRadius / r;
        const float amount = shrink * shrink * shrink;
        const float cx = (particle.x + 1.0f) * cellsPerUnit - 0.5f;
        const float cy = (particle.y + 1.0f) * cellsPerUnit - 0.5f;
        const float cz = (particle.z + 1.0f) * cellsPerUnit - 0.5f;
        const int x0 = (std::max)(0, static_cast<int>(std::floor(cx - r)));
        const int x1 = (std::min)(n - 1, static_cast<int>(std::ceil(cx + r)));
        const int y0 = (std::max)(0, static_cast<int>(std::floor(cy - r)));
        const int y1 = (std::min)(n - 1, static_cast<int>(std::ceil(cy + r)));
        const int z0 = (std::max)(0, static_cast<int>(std::floor(cz - r)));
        const int z1 = (std::min)(n - 1, static_cast<int>(std::ceil(cz + r)));
        const float inverseRadius = 1.0f / r;
        for (int z = z0; z <= z1; ++z) {
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const float dx = (static_cast<float>(x) - cx) * inverseRadius;
                    const float dy = (static_cast<float>(y) - cy) * inverseRadius;
                    const float dz = (static_cast<float>(z) - cz) * inverseRadius;
                    const float q2 = dx * dx + dy * dy + dz * dz;
                    if (q2 >= 1.0f) continue;
                    const float falloff = (1.0f - q2) * (1.0f - q2) * (1.0f - q2) * amount;
                    const std::size_t i = static_cast<std::size_t>(x)
                        + static_cast<std::size_t>(n) * (static_cast<std::size_t>(y) + static_cast<std::size_t>(n) * z);
                    out.medium[i].x += falloff;
                    out.medium[i].z += falloff * particle.colorKey;
                    out.velocity[i].x += falloff * particle.vx;
                    out.velocity[i].y += falloff * particle.vy;
                    out.velocity[i].z += falloff * particle.vz;
                    weights[i] += falloff;
                }
            }
        }
    }
    for (std::size_t i = 0; i < cells; ++i) {
        // 量を減らした粒の縁は重みが極小になる。閾値で切ると «密度はあるのに液体でないセル» ができる。
        if (weights[i] <= 0.0f) continue;
        out.medium[i].w = 1.0f;
        const float inverse = 1.0f / weights[i];
        out.medium[i].z = std::clamp(out.medium[i].z * inverse, 0.0f, 1.0f);
        out.velocity[i].x *= inverse;
        out.velocity[i].y *= inverse;
        out.velocity[i].z *= inverse;
    }
}

float FluidRecipeTemperatureScale(const FluidRecipe& recipe)
{
    float peak = 0.0f;
    for (const FluidSource& source : recipe.sources)
        if (source.enabled) peak = (std::max)(peak, source.temperature);
    return peak > 1.0e-4f ? 1.0f / peak : 1.0f;
}

FluidVolumeStream::~FluidVolumeStream()
{
    Close();
}

bool FluidVolumeStream::Open(const FluidRecipe& recipe, int resolution, float frameDt, float densityScale,
                             std::string& outError)
{
    Close();
    m_recipe = recipe;
    m_resolution = std::clamp(resolution, 8, 128);
    m_frameDt = (std::max)(frameDt, 1.0e-4f);
    m_substeps = std::clamp(recipe.output.substeps, 1, 16);
    m_warmupFrames = FluidWarmupFrames(recipe.output.warmup, m_frameDt);
    m_scale.density = (std::max)(densityScale, 0.0f);
    m_scale.temperature = FluidRecipeTemperatureScale(recipe);
    if (recipe.kind == FluidKind::Liquid) {
        m_liquid = std::make_unique<FluidLiquidSolver>();
        m_liquidRadiusScale = recipe.render.liquidRadiusScale;
        m_liquidLifetime = recipe.liquid.particleLifetime;
    } else {
        m_solver = std::make_unique<FluidGasSolver>();
    }
    m_advancedFrames = -1;
    (void)outError;
    return true;
}

void FluidVolumeStream::Cancel() noexcept
{
    if (m_cancel) m_cancel->store(true, std::memory_order_relaxed);
}

void FluidVolumeStream::Close()
{
    Cancel();
    if (m_job.valid()) m_job.wait();
    m_job = {};
    m_cancel.reset();
    m_solver.reset();
    m_liquid.reset();
    m_advancedFrames = -1;
}

bool FluidVolumeStream::Request(int frame)
{
    if (!IsOpen() || m_job.valid()) return false;
    frame = (std::max)(frame, 0);
    const bool restart = m_advancedFrames < 0 || frame + 1 < m_advancedFrames;
    // 旗をコマごとに作り直す。前のコマへの Cancel が次のコマまで残って «頼んだそばから畳まれる» のを防ぐ。
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_cancel = cancel;
    m_job = std::async(std::launch::async, [this, frame, restart, cancel]() {
        const auto cancelled = [&cancel]() { return cancel->load(std::memory_order_relaxed); };
        const auto advance = [this](float dt) {
            if (m_liquid) m_liquid->Advance(dt);
            else          m_solver->Advance(dt);
        };
        const float stepDt = m_frameDt / static_cast<float>(m_substeps);
        if (restart) {
            if (m_liquid) m_liquid->Reset(m_recipe, /*volumetric=*/true);
            else          m_solver->Reset(m_recipe, m_resolution, m_resolution, m_resolution);
            // warmup も «普通のコマ» として解く (刻みの正本は FluidStepping)。
            for (int i = 0; i < m_warmupFrames; ++i) {
                for (int step = 0; step < m_substeps; ++step) {
                    if (cancelled()) return PackedFluidVolume{};
                    advance(stepDt);
                }
            }
            m_advancedFrames = 0;
        }
        while (m_advancedFrames < frame + 1) {
            for (int step = 0; step < m_substeps; ++step) {
                if (cancelled()) {
                    // 途中まで進めた場はどのコマでもない。続きの起点にせず、次の Request で頭から解き直す。
                    m_advancedFrames = -1;
                    return PackedFluidVolume{};
                }
                advance(stepDt);
            }
            ++m_advancedFrames;
        }
        PackedFluidVolume volume;
        if (m_liquid) PackLiquidVolume(*m_liquid, m_resolution, m_liquidRadiusScale, m_liquidLifetime, volume);
        else          PackFluidVolume(*m_solver, m_scale, volume);
        volume.frame = frame;
        return volume;
    });
    return true;
}

bool FluidVolumeStream::Poll(PackedFluidVolume& out)
{
    if (!m_job.valid() || m_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    PackedFluidVolume volume = m_job.get();
    // 畳まれたワーカーの空手形。out を潰すと今出している絵まで消える。
    if (volume.frame < 0) return false;
    out = std::move(volume);
    return true;
}

} // namespace fbzz::asset
