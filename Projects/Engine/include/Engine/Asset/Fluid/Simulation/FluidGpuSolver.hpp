/// @file    FluidGpuSolver.hpp
/// @brief   気体の格子ソルバーの GPU (Compute) 版 — Volume Flipbook Baker の 3D 流体を高解像度で解く
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note 手順は FluidGasSolver (CPU) と同じ: 固体 (障害物) → 注入+燃焼 → 外力 → 渦度保存 → 圧力投影 → 移流 (MacCormack) → 散逸。
/// @note 違いは 3 つ:
/// @note - 圧力は Jacobi 法 (CPU は赤黒 SOR)。UAV からの読み出しを使わないため (RGBA16F の読み書き同時は
/// @note 任意機能で、無い GPU がある)。収束が遅いぶん反復を倍にする
/// @note - 1 刻みの幅は固定 (recipe.output.substeps)。CPU のように最大速度から刻みを割るには読み戻しが要る
/// @note - 結果は読み戻さず、焼き用のボリューム (媒質・速度) へ直接書く
/// @note 数値は CPU と一致しない (浮動小数の精度と反復法が違う)。«同じレシピで同じ傾向の絵» を保証する。

/// @note Tick は **レンダラーのフレーム内** で呼ぶこと (VolumeFlipbookBaker と同じ制約)。
#pragma once
#include <Engine/Api.hpp>

#include <Fluid/Recipe/FluidRecipe.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Graphics/Renderer/Resources/OwnedResource.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::asset {

class FluidGpuSolver {
public:
    static constexpr int kMaxFramesPerTick = 4; ///< 1 Tick で進められるコマ数の上限。定数バッファを刻みごとに別に持つので、その本数で決まる。
    static constexpr int kMaxSubsteps = 8; ///< 1 出力コマで許可する気体ソルバーの内部ステップ数の上限。
    /// @note 1 Tick に積んでよい Dispatch のおおよその上限 (液体ソルバーと同じ値)。
    /// @note プレビューは再生ヘッドまで追いつくため切り替え直後は全コマ再解きになり、コマ数だけで
    /// @note 止めると 1 フレームに数千本積んで GPU ウォッチドッグ (TDR) に掛かる。本数で頭打ちにし
    /// @note 追いつきを複数フレームへ分ける。
    static constexpr int kDispatchBudgetPerTick = 1024; ///< 1 回の Tick で GPU へ発行する計算ディスパッチ数の上限。

    /// @note kind = gas 以外は失敗。同じ解像度なら GPU の資源は使い回す。
    [[nodiscard]] FBZZ_ENGINE_API bool Initialize(renderer::ResourceManager& resources, const fluid::FluidRecipe& recipe, int resolution,
                                  float frameDt, float densityScale, std::string& outError);
    FBZZ_ENGINE_API void Release(renderer::ResourceManager& resources);
    [[nodiscard]] bool IsReady() const { return m_ready; }
    [[nodiscard]] int Resolution() const { return m_resolution; }
    /// @note 解き終えたコマ。0 コマ目 = warmup の後に 1 コマ進めた状態 (CPU の 2D / 3D ベイクと同じ数え方)。
    /// @note -1 以下 = まだ 0 コマ目に届いていない。
    [[nodiscard]] int SolvedFrame() const { return m_steppedFrames - m_warmupFrames - 1; }
    /// @note 時刻 0 からやり直す (次の StepFrame で場を 0 に戻す)。
    FBZZ_ENGINE_API void Restart();
    /// @note フレームの頭で 1 回呼ぶ (刻みの定数バッファの輪番を先頭へ戻す)。
    FBZZ_ENGINE_API void BeginTick();
    /// @note 1 コマ進める。この Tick でもう進められなければ何もせず false。
    FBZZ_ENGINE_API bool StepFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    /// @note 1 コマを解くのに積む Dispatch の本数 (予算の判定に使う概算)。
    [[nodiscard]] FBZZ_ENGINE_API int DispatchesPerFrame() const;
    /// @note 今の場を焼き用ボリュームへ書く (VolumeFill と同じ意味: 媒質 = 密度/温度/色の鍵、速度 = xyz)。
    FBZZ_ENGINE_API void WriteVolumes(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                      renderer::ResourceHandle<renderer::TextureTag> medium,
                      renderer::ResourceHandle<renderer::TextureTag> velocity);

private:
    using TextureHandle = renderer::ResourceHandle<renderer::TextureTag>;
    struct TextureBinding {
        uint32_t slot; ///< シェーダーへこのテクスチャを渡すバインドスロット番号。
        TextureHandle texture; ///< このスロットへ束縛する借用テクスチャハンドル。
    };
    using ConstantHandle = renderer::ResourceHandle<renderer::ConstantBufferTag>;
    using OwnedTexture = renderer::OwnedResource<renderer::TextureTag>;
    using OwnedConstant = renderer::OwnedResource<renderer::ConstantBufferTag>;

    FBZZ_ENGINE_API void Step(renderer::IRenderer& renderer, renderer::ResourceManager& resources, float dt);
    FBZZ_ENGINE_API void Run(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
             renderer::ResourceHandle<renderer::ShaderTag> shader, ConstantHandle constants,
             std::initializer_list<TextureBinding> inputs, std::initializer_list<TextureBinding> outputs,
             ConstantHandle passConstants = {});
    FBZZ_ENGINE_API void ReleaseTextures(renderer::ResourceManager& resources);
    /// @note Texture 発生源のマスクを 1 枚のアトラスへ並べて作り直す (FluidGpuMaskPaths の順にタイルへ置く)。
    FBZZ_ENGINE_API void BuildMaskAtlas(renderer::ResourceManager& resources, const fluid::FluidRecipe& recipe);

    enum Kernel : std::size_t {
        Clear, Inject, Forces, Curl, Confine, Divergence, Jacobi, Project, AdvectVelocity, AdvectScalar,
        Correct, Output, Solid, KernelCount
    };
    std::array<renderer::ResourceHandle<renderer::ShaderTag>, KernelCount> m_kernels{}; ///< 気体計算の各カーネルを列挙値で引くシェーダーハンドル群。
    /// @note 速度は 2 枚を交互に使う。スカラーは役割が固定 (0 = 今の場 / 1 = 注入後 / 2 = 前進移流 / 3 = 後退移流)。
    /// @note 2 と 3 は補正を終えた後、燃料の色の往復移流にも貸す。
    /// @note スカラーの中身は x 密度 / y 温度 / z 燃料 / w 色の質量 (密度 × 色の鍵)。
    std::array<OwnedTexture, 2> m_velocity{}; ///< 速度格子を読み書き交互に使う所有 3D テクスチャの組。
    std::array<OwnedTexture, 4> m_scalars{}; ///< 密度・温度・燃料などの移流と補正で使う所有格子の作業領域。
    std::array<OwnedTexture, 2> m_pressure{}; ///< 反復圧力計算で入力と出力を交換する所有格子の組。
    /// @note 燃料の色の質量 (z だけ使う。0 = 今の場 / 1 = 注入後)。燃料と同じ運び方をし、燃えた分だけ煙の色へ移る。
    /// @note スカラーの 4 本は密度・温度・燃料・煙の色で埋まっているため別テクスチャにした。
    /// @note 往復移流の作業場は scalars[2]/[3] を借りるので、増えるのはこの 2 枚だけ。
    std::array<OwnedTexture, 2> m_fuelColor{}; ///< 燃料色を読み書き交互に使う所有 3D テクスチャの組。
    OwnedTexture m_expansion; ///< 燃焼の膨張 (x)。Inject が書き、同じ刻みの Divergence が読む。刻みを跨いでは持たない。
    OwnedTexture m_aux; ///< 渦度 (xyz + 大きさ) → 発散 の順に使い回す作業用。
    OwnedTexture m_solid; ///< 障害物の中のセル (xyz 固体の速度 / w 1 = 固体)。刻みの頭に作り直す。
    OwnedTexture m_maskAtlas; ///< Texture 発生源のマスクのアトラス。Texture 発生源が無ければ無効 (束縛は白 1×1 で代える)。
    std::size_t m_currentVelocity = 0; ///< m_velocity のうち現在の確定速度を指す添字。
    std::size_t m_currentPressure = 0; ///< m_pressure のうち現在の圧力反復結果を指す添字。

    /// @note 刻みごとの定数。同じ定数バッファをフレーム内で書き直すと、先に積んだ Dispatch まで
    /// @note 後の値で走る実装がある (DX12 の Upload Heap 直書き)。刻みの数だけ別に持つ。
    std::vector<OwnedConstant> m_stepConstants; ///< 使用中の GPU 定数との競合を避けるディスパッチ用バッファ列。
    std::size_t m_nextStepConstant = 0; ///< 次のディスパッチで割り当てる m_stepConstants の添字。
    int m_framesThisTick = 0; ///< この Tick で積んだコマ数と Dispatch 本数 (BeginTick で 0 に戻す)。
    int m_dispatchesThisTick = 0; ///< この Tick ですでに投入した計算ディスパッチ数。
    OwnedConstant m_forwardConstants; ///< 前進移流カーネルへ渡す定数バッファ。
    OwnedConstant m_backwardConstants; ///< 逆向き移流カーネルへ渡す定数バッファ。
    OwnedConstant m_outputConstants; ///< 描画用ボリュームを書き出す計算パスへ渡す定数バッファ。

    fluid::FluidRecipe m_recipe; ///< GPU 気体計算の注入・外力・出力条件の写し。
    int m_resolution = 0; ///< 気体フィールドを離散化する格子の一辺のセル数。
    float m_frameDt = 1.0f / 24.0f; ///< 隣接する出力コマ間に進めるシミュレーション時間 [s]。
    int m_substeps = 1; ///< 出力コマ間を刻む内部積分ステップ数。
    int m_pressureIterations = 80; ///< 内部ステップで非圧縮性を求める圧力反復回数。
    float m_densityScale = 1.0f; ///< 描画用ボリュームの密度チャンネルに掛ける校正倍率。
    float m_temperatureScale = 1.0f; ///< 描画用ボリュームの温度チャンネルに掛ける校正倍率。
    float m_time = 0.0f; ///< 初期状態から現在まで進めたシミュレーション時刻 [s]。
    int m_warmupFrames = 0; ///< 撮影開始時までにソルバーを馴らすコマ数。
    int m_steppedFrames = 0; ///< ソルバーが進行済みの出力フレーム数。
    bool m_needsClear = true; ///< 次のシミュレーション前に GPU 格子の初期化を要するか。
    bool m_ready = false; ///< 計算パスを投入できるシェーダーと資源が揃っているか。
    std::uint64_t m_resetVersion = 0; ///< 所有する資源を作ったときの GPU デバイスリセット世代。
    std::uint64_t m_resourceOwner = 0; ///< GPU 資源を作った ResourceManager を識別する寿命 ID。
};

} // namespace fbzz::asset
