/// @file    FluidGpuLiquidSolver.hpp
/// @brief   液体の粒子ソルバー (PBF) の GPU (Compute) 版 — Volume Flipbook Baker の 3D 液体を多粒子で解く
/// @author  Hasegawa Jin
/// @date    2026-09-12

/// @note 手順は FluidLiquidSolver (CPU) と同じ: 湧かせる → 外力・重力 → 位置の予測 → 近傍格子 → 密度拘束の反復
/// @note (障害物と床への押し出しを含む) → 速度の更新 (押し出しの持ち越し上限・床と障害物の摩擦) → XSPH 粘性 → 寿命・領域外。
/// @note 違いは 3 つ:
/// @note - 湧かせ方は Initialize で全粒子ぶんを CPU が決めて一度だけ上げる (粒子 i の出る時刻・位置・速度・色の鍵)。
/// @note GPU は時刻が来た粒子を起こすだけ。刻みごとに CPU から粒子を足さないので、上り転送が要らない
/// @note - 1 刻みの幅は固定 (recipe.output.substeps)。CPU のように最大速度から刻みを割るには読み戻しが要る
/// @note - 結果は読み戻さず、焼き用のボリューム (媒質・速度) へ直接書く (PackLiquidVolume と同じ意味)
/// @note 数値は CPU と一致しない。«同じレシピで同じ傾向の絵» を保証する。

/// @note Tick は **レンダラーのフレーム内** で呼ぶこと (FluidGpuSolver と同じ制約)。
#pragma once
#include <Engine/Api.hpp>

#include <Fluid/FluidRecipe.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>

#include <memory>
#include <string>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::asset {

class FluidGpuLiquidSolver {
public:
    /// @note 1 Tick で進められるコマ数の上限 (FluidGpuSolver と同じ理由で、刻みごとに別の定数バッファを持つ)。
    static constexpr int kMaxFramesPerTick = 4;
    static constexpr int kMaxSubsteps = 8;
    /// @note 粒子数の上限 (liquid.maxParticles をここで丸める)。
    static constexpr int kMaxParticles = 32768;

    FBZZ_ENGINE_API FluidGpuLiquidSolver();
    FBZZ_ENGINE_API ~FluidGpuLiquidSolver();
    FluidGpuLiquidSolver(const FluidGpuLiquidSolver&) = delete;
    FluidGpuLiquidSolver& operator=(const FluidGpuLiquidSolver&) = delete;

    /// @note kind = liquid 以外は失敗。resolution は WriteVolumes が書くボリュームの 1 辺。
    /// @note radiusScale は描画の半径 (粒子半径に対する倍率。render.liquidRadiusScale)。
    [[nodiscard]] FBZZ_ENGINE_API bool Initialize(renderer::ResourceManager& resources, const fluid::FluidRecipe& recipe, int resolution,
                                  float frameDt, float radiusScale, std::string& outError);
    FBZZ_ENGINE_API void Release(renderer::ResourceManager& resources);

    [[nodiscard]] FBZZ_ENGINE_API bool IsReady() const;
    [[nodiscard]] FBZZ_ENGINE_API int Resolution() const;
    /// @note 解き終えたコマ (FluidGpuSolver::SolvedFrame と同じ数え方。warmup の後に 1 コマ進めた状態が 0)。
    [[nodiscard]] FBZZ_ENGINE_API int SolvedFrame() const;
    /// @note 時刻 0 からやり直す。
    FBZZ_ENGINE_API void Restart();
    /// @note フレームの頭で 1 回呼ぶ。
    FBZZ_ENGINE_API void BeginTick();
    /// @note 1 コマ進める。この Tick でもう進められなければ何もせず false。
    FBZZ_ENGINE_API bool StepFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    /// @note 今の粒子を焼き用ボリュームへ書く。意味は PackLiquidVolume と同じ
    /// @note (媒質: R = 粒子の山の和 / B = 色の鍵 / A = 液体なら 1、速度: 重み付き平均)。
    FBZZ_ENGINE_API void WriteVolumes(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                      renderer::ResourceHandle<renderer::TextureTag> medium,
                      renderer::ResourceHandle<renderer::TextureTag> velocity);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} /// @note namespace fbzz::asset
