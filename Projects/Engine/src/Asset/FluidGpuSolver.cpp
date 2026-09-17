/// @file    FluidGpuSolver.cpp
/// @brief   気体の格子ソルバーの GPU 版
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/FluidGpuSolver.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Fluid/FluidGpuStep.hpp>
#include <Engine/Asset/FluidSourceMaskLoader.hpp>
#include <Fluid/FluidStepping.hpp>
#include <Engine/Asset/VolumeFlipbookFluid.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fbzz::asset {
namespace {

constexpr const char* kKernelPaths[] = {
    "Assets/Shaders/Bake/Fluid/FluidClear.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidInject.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidForces.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidCurl.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidConfine.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidDivergence.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidJacobi.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidProject.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidAdvectVelocity.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidAdvectScalar.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidCorrect.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidOutput.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/FluidSolid.cs.hlsl",
};

/// FluidGpuCommon.hlsli の FluidPassConstants と 1:1。
struct alignas(16) PassConstants {
    float advectSign;
    float pad[3];
};

} // namespace

bool FluidGpuSolver::Initialize(renderer::ResourceManager& resources, const fluid::FluidRecipe& recipe, int resolution,
                                float frameDt, float densityScale, std::string& outError)
{
    if (recipe.kind != fluid::FluidKind::Gas) {
        outError = "3D で解けるのは気体 (kind = gas) のレシピだけです";
        return false;
    }
    /// @note デバイスリセット後の古いハンドルは返せない (実体ごと消えている)。捨てて作り直す。
    if (m_resetVersion != resources.GetResetVersion()) {
        m_kernels = {};
        m_velocity = {};
        m_scalars = {};
        m_pressure = {};
        m_fuelColor = {};
        m_expansion = {};
        m_aux = {};
        m_solid = {};
        m_maskAtlas = {};
        m_stepConstants.clear();
        m_forwardConstants = {};
        m_backwardConstants = {};
        m_outputConstants = {};
        m_resolution = 0;
        m_resetVersion = resources.GetResetVersion();
    }

    static_assert(sizeof(kKernelPaths) / sizeof(kKernelPaths[0]) == KernelCount,
                  "kKernelPaths は Kernel の並びと 1:1");
    for (std::size_t i = 0; i < KernelCount; ++i) {
        if (!m_kernels[i].IsValid())
            m_kernels[i] = resources.LoadShader(AssetManager::ResolveAssetPath(kKernelPaths[i]));
        if (!m_kernels[i].IsValid()) {
            outError = std::string("流体の Compute シェーダーを読み込めません: ") + kKernelPaths[i];
            m_ready = false;
            return false;
        }
    }

    const int clampedResolution = std::clamp(resolution, 16, 192);
    if (clampedResolution != m_resolution) {
        ReleaseTextures(resources);
        const auto n = static_cast<std::uint32_t>(clampedResolution);
        const auto create = [&](TextureHandle& texture) {
            texture = resources.CreateComputeTexture3D(n, n, n);
            return texture.IsValid();
        };
        bool ok = true;
        for (auto& texture : m_velocity) ok &= create(texture);
        for (auto& texture : m_scalars) ok &= create(texture);
        for (auto& texture : m_pressure) ok &= create(texture);
        for (auto& texture : m_fuelColor) ok &= create(texture);
        ok &= create(m_expansion);
        ok &= create(m_aux);
        ok &= create(m_solid);
        if (!ok) {
            ReleaseTextures(resources);
            outError = "流体の 3D テクスチャを作れません (このバックエンドは未対応か、VRAM が足りません)";
            m_ready = false;
            return false;
        }
        m_resolution = clampedResolution;
    }
    BuildMaskAtlas(resources, recipe);

    const std::size_t constantCount = static_cast<std::size_t>(kMaxFramesPerTick) * kMaxSubsteps;
    while (m_stepConstants.size() < constantCount)
        m_stepConstants.push_back(resources.CreateConstantBuffer(sizeof(fluid::FluidGpuStepConstants)));
    if (!m_forwardConstants.IsValid()) {
        m_forwardConstants = resources.CreateConstantBuffer(sizeof(PassConstants));
        const PassConstants forward{ 1.0f, { 0.0f, 0.0f, 0.0f } };
        resources.Update(m_forwardConstants, &forward, sizeof(forward));
    }
    if (!m_backwardConstants.IsValid()) {
        m_backwardConstants = resources.CreateConstantBuffer(sizeof(PassConstants));
        const PassConstants backward{ -1.0f, { 0.0f, 0.0f, 0.0f } };
        resources.Update(m_backwardConstants, &backward, sizeof(backward));
    }
    if (!m_outputConstants.IsValid())
        m_outputConstants = resources.CreateConstantBuffer(sizeof(fluid::FluidGpuStepConstants));

    /// @note 確保に失敗しても size() は増えるので、ここで確かめないと «半端に成功» のまま
    ///       m_ready = true まで進む。無効な定数バッファは束縛が飛ばされ、直前のパスが b0 に残した
    ///       別物の定数でシェーダーが走る (= 解像度すら別の値で読む)。落とすなら開く前に落とす。
    const bool constantsReady =
        std::all_of(m_stepConstants.begin(), m_stepConstants.end(),
                    [](const ConstantHandle& handle) { return handle.IsValid(); })
        && m_forwardConstants.IsValid() && m_backwardConstants.IsValid() && m_outputConstants.IsValid();
    if (!constantsReady) {
        outError = "流体ソルバーの定数バッファを作れません";
        return false;
    }

    m_recipe = recipe;
    m_frameDt = (std::max)(frameDt, 1.0e-4f);
    m_substeps = std::clamp(recipe.output.substeps, 1, kMaxSubsteps);
    /// @note Jacobi は SOR より収束が遅い。CPU と同じ反復回数では圧力が抜けきらず、煙が膨らんで見える。
    m_pressureIterations = std::clamp(recipe.gas.pressureIterations * 2, 20, 400);
    m_densityScale = (std::max)(densityScale, 0.0f);
    m_temperatureScale = FluidRecipeTemperatureScale(recipe);
    m_warmupFrames = fluid::FluidWarmupFrames(recipe.output.warmup, m_frameDt);
    m_ready = true;
    Restart();
    return true;
}

void FluidGpuSolver::ReleaseTextures(renderer::ResourceManager& resources)
{
    const bool owned = m_resetVersion == resources.GetResetVersion();
    const auto release = [&](TextureHandle& texture) {
        if (owned && texture.IsValid()) resources.Release(texture);
        texture = {};
    };
    for (auto& texture : m_velocity) release(texture);
    for (auto& texture : m_scalars) release(texture);
    for (auto& texture : m_pressure) release(texture);
    for (auto& texture : m_fuelColor) release(texture);
    release(m_expansion);
    release(m_aux);
    release(m_solid);
    release(m_maskAtlas);
    m_resolution = 0;
}

void FluidGpuSolver::BuildMaskAtlas(renderer::ResourceManager& resources, const fluid::FluidRecipe& recipe)
{
    if (m_maskAtlas.IsValid() && m_resetVersion == resources.GetResetVersion()) resources.Release(m_maskAtlas);
    m_maskAtlas = {};
    const std::vector<std::string> paths = fluid::FluidGpuMaskPaths(recipe);
    if (paths.empty()) return;

    constexpr int kTile = fluid::kFluidSourceMaskSize;
    constexpr int kColumns = fluid::kFluidSourceMaskAtlasColumns;
    constexpr int kAtlasSize = kTile * kColumns;
    /// @note 読めなかったタイル・使わないタイルは 1 (白)。SampleFluidSourceMask の «無効なら 1» と同じく板の形に湧く。
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kAtlasSize) * kAtlasSize * 4u, 255u);
    const std::size_t tileCount = (std::min)(paths.size(), static_cast<std::size_t>(kColumns * kColumns));
    for (std::size_t tile = 0; tile < tileCount; ++tile) {
        fluid::FluidSourceMask mask;
        std::string error;
        if (!LoadFluidSourceMask(paths[tile], mask, &error) || !mask.IsValid()) {
            FBZZ_LOG_WARN("FluidGpuSolver: マスクを読めないので板の形で湧かせます (%s): %s", paths[tile].c_str(),
                          error.c_str());
            continue;
        }
        const std::size_t x0 = (tile % kColumns) * kTile;
        const std::size_t y0 = (tile / kColumns) * kTile;
        for (std::size_t y = 0; y < static_cast<std::size_t>(kTile); ++y) {
            for (std::size_t x = 0; x < static_cast<std::size_t>(kTile); ++x) {
                const float value = std::clamp(mask.values[y * kTile + x], 0.0f, 1.0f);
                rgba[((y0 + y) * kAtlasSize + x0 + x) * 4u] = static_cast<std::uint8_t>(value * 255.0f + 0.5f);
            }
        }
    }
    m_maskAtlas = resources.CreateTexture(rgba.data(), kAtlasSize, kAtlasSize);
    if (!m_maskAtlas.IsValid())
        FBZZ_LOG_WARN("FluidGpuSolver: マスクのアトラスを作れないので、Texture 発生源は板の形で湧かせます");
}

void FluidGpuSolver::Release(renderer::ResourceManager& resources)
{
    ReleaseTextures(resources);
    /// @note デバイスリセット後のハンドルは既に実体が無い。返しに行くと別のリソースを消しかねない。
    if (m_resetVersion == resources.GetResetVersion()) {
        for (const auto& constants : m_stepConstants)
            if (constants.IsValid()) resources.Release(constants);
        for (auto* constants : { &m_forwardConstants, &m_backwardConstants, &m_outputConstants })
            if (constants->IsValid()) resources.Release(*constants);
    }
    m_stepConstants.clear();
    m_forwardConstants = {};
    m_backwardConstants = {};
    m_outputConstants = {};
    m_kernels = {};
    m_ready = false;
}

void FluidGpuSolver::Restart()
{
    m_time = 0.0f;
    m_steppedFrames = 0;
    m_needsClear = true;
    m_currentVelocity = 0;
    m_currentPressure = 0;
}

void FluidGpuSolver::BeginTick()
{
    m_nextStepConstant = 0;
    m_framesThisTick = 0;
    m_dispatchesThisTick = 0;
}

int FluidGpuSolver::DispatchesPerFrame() const
{
    /// @note 1 刻みの内訳: 固体/注入/力 3 + 渦度 2 + 発散 1 + 圧力 m_pressureIterations + 投影/移流/補正 8。
    constexpr int kFixedPerStep = 14;
    return (std::max)(m_substeps, 1) * (kFixedPerStep + (std::max)(m_pressureIterations, 0));
}

void FluidGpuSolver::Run(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                         renderer::ResourceHandle<renderer::ShaderTag> shader, ConstantHandle constants,
                         std::initializer_list<TextureHandle> inputs, std::initializer_list<TextureHandle> outputs,
                         ConstantHandle passConstants)
{
    renderer::ComputeCall call;
    call.shader = shader;
    call.constantBuffers[0] = constants;
    call.constantBuffers[1] = passConstants;
    std::size_t slot = 0;
    for (const TextureHandle& texture : inputs) call.srvInputs[slot++] = texture;
    slot = 0;
    for (const TextureHandle& texture : outputs) call.uavOutputs[slot++] = texture;
    call.dispatchX = call.dispatchY = call.dispatchZ = (static_cast<std::uint32_t>(m_resolution) + 3u) / 4u;
    renderer.Dispatch(call, resources);
}

bool FluidGpuSolver::StepFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    if (!m_ready || m_nextStepConstant + static_cast<std::size_t>(m_substeps) > m_stepConstants.size())
        return false;
    /// @note 1 コマ目は予算を超えても必ず通す (通さないと «永久に追いつかない» になる)。2 コマ目からは
    ///       本数とコマ数の両方で止める。止めても呼び手は次の Tick で続きを解くので、絵が遅れるだけで済む。
    if (m_framesThisTick > 0) {
        if (m_framesThisTick >= kMaxFramesPerTick) return false;
        if (m_dispatchesThisTick + DispatchesPerFrame() > kDispatchBudgetPerTick) return false;
    }
    if (m_needsClear) {
        /// @note 作ったばかりの 3D テクスチャの中身は未定義。場は 0 から始める。
        const ConstantHandle constants = m_stepConstants[m_nextStepConstant];
        const fluid::FluidGpuStepConstants cleared =
            fluid::PackFluidGpuStep(m_recipe, m_resolution, 0.0f, 0.0f, m_densityScale, m_temperatureScale);
        resources.Update(constants, &cleared, sizeof(cleared));
        Run(renderer, resources, m_kernels[Clear], constants, {}, { m_velocity[0], m_velocity[1] });
        Run(renderer, resources, m_kernels[Clear], constants, {}, { m_scalars[0], m_scalars[1] });
        Run(renderer, resources, m_kernels[Clear], constants, {}, { m_scalars[2], m_scalars[3] });
        Run(renderer, resources, m_kernels[Clear], constants, {}, { m_pressure[0], m_pressure[1] });
        Run(renderer, resources, m_kernels[Clear], constants, {}, { m_fuelColor[0], m_fuelColor[1] });
        Run(renderer, resources, m_kernels[Clear], constants, {}, { m_aux });
        m_needsClear = false;
    }
    const float stepDt = m_frameDt / static_cast<float>(m_substeps);
    for (int step = 0; step < m_substeps; ++step) Step(renderer, resources, stepDt);
    ++m_steppedFrames;
    ++m_framesThisTick;
    m_dispatchesThisTick += DispatchesPerFrame();
    return true;
}

void FluidGpuSolver::Step(renderer::IRenderer& renderer, renderer::ResourceManager& resources, float dt)
{
    const ConstantHandle constants = m_stepConstants[m_nextStepConstant++];
    const fluid::FluidGpuStepConstants packed =
        fluid::PackFluidGpuStep(m_recipe, m_resolution, m_time, dt, m_densityScale, m_temperatureScale);
    resources.Update(constants, &packed, sizeof(packed));

    std::size_t current = m_currentVelocity;
    TextureHandle velocity = m_velocity[current];
    TextureHandle spare = m_velocity[1 - current];
    const auto swapVelocity = [&]() {
        current = 1 - current;
        velocity = m_velocity[current];
        spare = m_velocity[1 - current];
    };

    /// @note 固体は刻みの頭に 1 回だけ作り、以降の段はすべて同じものを見る (段ごとに作ると動く障害物の位置が段でずれる)。
    ///       障害物が無ければ全セル 0 になり、以降の段は障害物を足す前と同じ結果を出す。
    Run(renderer, resources, m_kernels[Solid], constants, {}, { m_solid });
    /// @note Inject の t3 を空のまま Dispatch させない。白ならどのタイルを引いてもマスク 1 = 板の形。
    const TextureHandle masks = m_maskAtlas.IsValid() ? m_maskAtlas : resources.GetWhiteTexture();

    /// @note 注入 + 燃焼: 今の場 (scalars[0]) → 注入後 (scalars[1])。速度も発生源の流速へ寄せる。
    Run(renderer, resources, m_kernels[Inject], constants, { m_scalars[0], velocity, m_solid, masks, m_fuelColor[0] },
        { m_scalars[1], spare, m_expansion, m_fuelColor[1] });
    swapVelocity();
    Run(renderer, resources, m_kernels[Forces], constants, { velocity, m_scalars[1] }, { spare });
    swapVelocity();
    if (packed.vorticity > 0.0f) {
        Run(renderer, resources, m_kernels[Curl], constants, { velocity }, { m_aux });
        Run(renderer, resources, m_kernels[Confine], constants, { velocity, m_aux }, { spare });
        swapVelocity();
    }

    /// @note 圧力投影。前の刻みの圧力を初期値に残す (warm start) と、同じ反復回数でも収束が進む。
    Run(renderer, resources, m_kernels[Divergence], constants, { velocity, m_expansion, m_solid }, { m_aux });
    for (int iteration = 0; iteration < m_pressureIterations; ++iteration) {
        Run(renderer, resources, m_kernels[Jacobi], constants, { m_pressure[m_currentPressure], m_aux, m_solid },
            { m_pressure[1 - m_currentPressure] });
        m_currentPressure = 1 - m_currentPressure;
    }
    Run(renderer, resources, m_kernels[Project], constants, { velocity, m_pressure[m_currentPressure], m_solid },
        { spare });
    swapVelocity();

    /// @note 移流は投影後の速度で運ぶ (CPU と同じ)。スカラーは MacCormack で往復させ、補正して今の場へ戻す。
    Run(renderer, resources, m_kernels[AdvectScalar], constants, { m_scalars[1], velocity }, { m_scalars[2] },
        m_forwardConstants);
    if (packed.sharp != 0u)
        Run(renderer, resources, m_kernels[AdvectScalar], constants, { m_scalars[2], velocity }, { m_scalars[3] },
            m_backwardConstants);
    Run(renderer, resources, m_kernels[Correct], constants,
        { m_scalars[1], m_scalars[2], packed.sharp != 0u ? m_scalars[3] : m_scalars[2], velocity, m_solid },
        { m_scalars[0] });

    /// @note 燃料の色も燃料と同じ速度・同じ往復で運ぶ (Correct の z の規則がそのまま燃料の散逸規則になる)。
    ///       作業場は scalars[2]/[3] を借用する。直前の Correct で用済みで、専用に 2 枚増やすと 192³ で 100 MB 余分に要る。
    Run(renderer, resources, m_kernels[AdvectScalar], constants, { m_fuelColor[1], velocity }, { m_scalars[2] },
        m_forwardConstants);
    if (packed.sharp != 0u)
        Run(renderer, resources, m_kernels[AdvectScalar], constants, { m_scalars[2], velocity }, { m_scalars[3] },
            m_backwardConstants);
    Run(renderer, resources, m_kernels[Correct], constants,
        { m_fuelColor[1], m_scalars[2], packed.sharp != 0u ? m_scalars[3] : m_scalars[2], velocity, m_solid },
        { m_fuelColor[0] });

    Run(renderer, resources, m_kernels[AdvectVelocity], constants, { velocity, m_solid }, { spare });
    swapVelocity();

    m_currentVelocity = current;
    m_time += dt;
}

void FluidGpuSolver::WriteVolumes(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                  TextureHandle medium, TextureHandle velocity)
{
    if (!m_ready || !medium.IsValid() || !velocity.IsValid()) return;
    const fluid::FluidGpuStepConstants packed =
        fluid::PackFluidGpuStep(m_recipe, m_resolution, m_time, 0.0f, m_densityScale, m_temperatureScale);
    resources.Update(m_outputConstants, &packed, sizeof(packed));
    Run(renderer, resources, m_kernels[Output], m_outputConstants,
        { m_scalars[0], m_velocity[m_currentVelocity] }, { medium, velocity });
}

} // namespace fbzz::asset
