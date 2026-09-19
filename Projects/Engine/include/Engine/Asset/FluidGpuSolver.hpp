/// @file    FluidGpuSolver.hpp
/// @brief   気体の格子ソルバーの GPU (Compute) 版 — Volume Flipbook Baker の 3D 流体を高解像度で解く
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 手順は FluidGasSolver (CPU) と同じ: 固体 (障害物) → 注入+燃焼 → 外力 → 渦度保存 → 圧力投影 → 移流 (MacCormack) → 散逸。
/// 違いは 3 つ:
///   - 圧力は Jacobi 法 (CPU は赤黒 SOR)。UAV からの読み出しを使わないため (RGBA16F の読み書き同時は
///     任意機能で、無い GPU がある)。収束が遅いぶん反復を倍にする
///   - 1 刻みの幅は固定 (recipe.output.substeps)。CPU のように最大速度から刻みを割るには読み戻しが要る
///   - 結果は読み戻さず、焼き用のボリューム (媒質・速度) へ直接書く
/// 数値は CPU と一致しない (浮動小数の精度と反復法が違う)。«同じレシピで同じ傾向の絵» を保証する。
///
/// Tick は **レンダラーのフレーム内** で呼ぶこと (VolumeFlipbookBaker と同じ制約)。
#pragma once

#include <Fluid/FluidRecipe.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::asset {

class FluidGpuSolver {
public:
    /// 1 Tick で進められるコマ数の上限。定数バッファを刻みごとに別に持つので、その本数で決まる。
    static constexpr int kMaxFramesPerTick = 4;
    static constexpr int kMaxSubsteps = 8;
    /// 1 Tick に積んでよい Dispatch のおおよその上限 (液体ソルバーと同じ値)。
    /// @note プレビューは再生ヘッドまで追いつくため切り替え直後は全コマ再解きになり、コマ数だけで
    ///       止めると 1 フレームに数千本積んで GPU ウォッチドッグ (TDR) に掛かる。本数で頭打ちにし
    ///       追いつきを複数フレームへ分ける。
    static constexpr int kDispatchBudgetPerTick = 1024;

    /// kind = gas 以外は失敗。同じ解像度なら GPU の資源は使い回す。
    [[nodiscard]] bool Initialize(renderer::ResourceManager& resources, const fluid::FluidRecipe& recipe, int resolution,
                                  float frameDt, float densityScale, std::string& outError);
    void Release(renderer::ResourceManager& resources);
    [[nodiscard]] bool IsReady() const { return m_ready; }
    [[nodiscard]] int Resolution() const { return m_resolution; }
    /// 解き終えたコマ。0 コマ目 = warmup の後に 1 コマ進めた状態 (CPU の 2D / 3D ベイクと同じ数え方)。
    /// -1 以下 = まだ 0 コマ目に届いていない。
    [[nodiscard]] int SolvedFrame() const { return m_steppedFrames - m_warmupFrames - 1; }
    /// 時刻 0 からやり直す (次の StepFrame で場を 0 に戻す)。
    void Restart();
    /// フレームの頭で 1 回呼ぶ (刻みの定数バッファの輪番を先頭へ戻す)。
    void BeginTick();
    /// 1 コマ進める。この Tick でもう進められなければ何もせず false。
    bool StepFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    /// 1 コマを解くのに積む Dispatch の本数 (予算の判定に使う概算)。
    [[nodiscard]] int DispatchesPerFrame() const;
    /// 今の場を焼き用ボリュームへ書く (VolumeFill と同じ意味: 媒質 = 密度/温度/色の鍵、速度 = xyz)。
    void WriteVolumes(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                      renderer::ResourceHandle<renderer::TextureTag> medium,
                      renderer::ResourceHandle<renderer::TextureTag> velocity);

private:
    using TextureHandle = renderer::ResourceHandle<renderer::TextureTag>;
    using ConstantHandle = renderer::ResourceHandle<renderer::ConstantBufferTag>;

    void Step(renderer::IRenderer& renderer, renderer::ResourceManager& resources, float dt);
    void Run(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
             renderer::ResourceHandle<renderer::ShaderTag> shader, ConstantHandle constants,
             std::initializer_list<TextureHandle> inputs, std::initializer_list<TextureHandle> outputs,
             ConstantHandle passConstants = {});
    void ReleaseTextures(renderer::ResourceManager& resources);
    /// Texture 発生源のマスクを 1 枚のアトラスへ並べて作り直す (FluidGpuMaskPaths の順にタイルへ置く)。
    void BuildMaskAtlas(renderer::ResourceManager& resources, const fluid::FluidRecipe& recipe);

    enum Kernel : std::size_t {
        Clear, Inject, Forces, Curl, Confine, Divergence, Jacobi, Project, AdvectVelocity, AdvectScalar,
        Correct, Output, Solid, KernelCount
    };
    std::array<renderer::ResourceHandle<renderer::ShaderTag>, KernelCount> m_kernels{};
    /// 速度は 2 枚を交互に使う。スカラーは役割が固定 (0 = 今の場 / 1 = 注入後 / 2 = 前進移流 / 3 = 後退移流)。
    /// 2 と 3 は補正を終えた後、燃料の色の往復移流にも貸す。
    /// スカラーの中身は x 密度 / y 温度 / z 燃料 / w 色の質量 (密度 × 色の鍵)。
    std::array<TextureHandle, 2> m_velocity{};
    std::array<TextureHandle, 4> m_scalars{};
    std::array<TextureHandle, 2> m_pressure{};
    /// 燃料の色の質量 (z だけ使う。0 = 今の場 / 1 = 注入後)。燃料と同じ運び方をし、燃えた分だけ煙の色へ移る。
    /// @note スカラーの 4 本は密度・温度・燃料・煙の色で埋まっているため別テクスチャにした。
    ///       往復移流の作業場は scalars[2]/[3] を借りるので、増えるのはこの 2 枚だけ。
    std::array<TextureHandle, 2> m_fuelColor{};
    /// 燃焼の膨張 (x)。Inject が書き、同じ刻みの Divergence が読む。刻みを跨いでは持たない。
    TextureHandle m_expansion;
    /// 渦度 (xyz + 大きさ) → 発散 の順に使い回す作業用。
    TextureHandle m_aux;
    /// 障害物の中のセル (xyz 固体の速度 / w 1 = 固体)。刻みの頭に作り直す。
    TextureHandle m_solid;
    /// Texture 発生源のマスクのアトラス。Texture 発生源が無ければ無効 (束縛は白 1×1 で代える)。
    TextureHandle m_maskAtlas;
    std::size_t m_currentVelocity = 0;
    std::size_t m_currentPressure = 0;

    /// 刻みごとの定数。同じ定数バッファをフレーム内で書き直すと、先に積んだ Dispatch まで
    /// 後の値で走る実装がある (DX12 の Upload Heap 直書き)。刻みの数だけ別に持つ。
    std::vector<ConstantHandle> m_stepConstants;
    std::size_t m_nextStepConstant = 0;
    /// この Tick で積んだコマ数と Dispatch 本数 (BeginTick で 0 に戻す)。
    int m_framesThisTick = 0;
    int m_dispatchesThisTick = 0;
    ConstantHandle m_forwardConstants;
    ConstantHandle m_backwardConstants;
    ConstantHandle m_outputConstants;

    fluid::FluidRecipe m_recipe;
    int m_resolution = 0;
    float m_frameDt = 1.0f / 24.0f;
    int m_substeps = 1;
    int m_pressureIterations = 80;
    float m_densityScale = 1.0f;
    float m_temperatureScale = 1.0f;
    float m_time = 0.0f;
    int m_warmupFrames = 0;
    int m_steppedFrames = 0;
    bool m_needsClear = true;
    bool m_ready = false;
    std::uint64_t m_resetVersion = 0;
};

} // namespace fbzz::asset
