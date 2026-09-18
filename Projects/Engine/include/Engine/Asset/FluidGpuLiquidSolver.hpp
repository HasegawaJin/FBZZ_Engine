/// @file    FluidGpuLiquidSolver.hpp
/// @brief   液体の粒子ソルバー (PBF) の GPU (Compute) 版 — Volume Flipbook Baker の 3D 液体を多粒子で解く
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 手順は FluidLiquidSolver (CPU) と同じ: 湧かせる → 外力・重力 → 位置の予測 → 近傍格子 → 密度拘束の反復
/// (障害物と床への押し出しを含む) → 速度の更新 (押し出しの持ち越し上限・床と障害物の摩擦) → XSPH 粘性 → 寿命・領域外。
/// 違いは 3 つ:
///   - 湧かせ方は Initialize で全粒子ぶんを CPU が決めて一度だけ上げる (粒子 i の出る時刻・位置・速度・色の鍵)。
///     GPU は時刻が来た粒子を起こすだけ。刻みごとに CPU から粒子を足さないので、上り転送が要らない
///   - 1 刻みの幅は固定 (recipe.output.substeps)。CPU のように最大速度から刻みを割るには読み戻しが要る
///   - 結果は読み戻さず、焼き用のボリューム (媒質・速度) へ直接書く (PackLiquidVolume と同じ意味)
/// 数値は CPU と一致しない。«同じレシピで同じ傾向の絵» を保証する。
///
/// Tick は **レンダラーのフレーム内** で呼ぶこと (FluidGpuSolver と同じ制約)。
#pragma once

#include <Fluid/FluidRecipe.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>

#include <memory>
#include <string>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::asset {

class FluidGpuLiquidSolver {
public:
    /// 1 Tick で進められるコマ数の上限 (FluidGpuSolver と同じ理由で、刻みごとに別の定数バッファを持つ)。
    static constexpr int kMaxFramesPerTick = 4;
    static constexpr int kMaxSubsteps = 8;
    /// 粒子数の上限 (liquid.maxParticles をここで丸める)。
    static constexpr int kMaxParticles = 32768;

    FluidGpuLiquidSolver();
    ~FluidGpuLiquidSolver();
    FluidGpuLiquidSolver(const FluidGpuLiquidSolver&) = delete;
    FluidGpuLiquidSolver& operator=(const FluidGpuLiquidSolver&) = delete;

    /// kind = liquid 以外は失敗。resolution は WriteVolumes が書くボリュームの 1 辺。
    /// radiusScale は描画の半径 (粒子半径に対する倍率。render.liquidRadiusScale)。
    [[nodiscard]] bool Initialize(renderer::ResourceManager& resources, const fluid::FluidRecipe& recipe, int resolution,
                                  float frameDt, float radiusScale, std::string& outError);
    void Release(renderer::ResourceManager& resources);

    [[nodiscard]] bool IsReady() const;
    [[nodiscard]] int Resolution() const;
    /// 解き終えたコマ (FluidGpuSolver::SolvedFrame と同じ数え方。warmup の後に 1 コマ進めた状態が 0)。
    [[nodiscard]] int SolvedFrame() const;
    /// 時刻 0 からやり直す。
    void Restart();
    /// フレームの頭で 1 回呼ぶ。
    void BeginTick();
    /// 1 コマ進める。この Tick でもう進められなければ何もせず false。
    bool StepFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    /// 今の粒子を焼き用ボリュームへ書く。意味は PackLiquidVolume と同じ
    /// (媒質: R = 粒子の山の和 / B = 色の鍵 / A = 液体なら 1、速度: 重み付き平均)。
    void WriteVolumes(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                      renderer::ResourceHandle<renderer::TextureTag> medium,
                      renderer::ResourceHandle<renderer::TextureTag> velocity);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fbzz::asset
