/// @file    FluidBaker.hpp
/// @brief   流体レシピを焼く — 1 コマの絵 (プレビューと共有)・フリップブック・Motion Vector・速度場 PNG
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note Inspector のライブプレビューが別の描き方をすると «プレビューでは濃いのに焼くと薄い» が
/// @note 起きるため、プレビューも焼きも同じ RenderFluid*Frame() を通す。
#pragma once

#include <Fluid/FluidRecipe.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Scene/ScriptProxy/ScriptParticleProxy.hpp>
#include <atomic>
#include <span>
#include <string>
#include <vector>

namespace fbzz::fluid {
class FluidGasSolver;
class FluidLiquidSolver;
}

namespace fbzz::asset {

/// @brief 1 コマぶんの絵。
/// @note rgba   … PNG へそのまま書く値 (sRGB・[0,1])。Fire は事前乗算、それ以外はストレート
/// @note motion … «次のコマまでに中身が動く量» をコマの UV 単位で持つ (x: 右, y: 下)
struct FluidFrameImage {
    int size = 0;
    std::vector<float> rgba;
    std::vector<float> motion;
};

/// @brief 2D Bake の工程別実測値。ワーカースレッドが書き、Editor が読む。
struct FluidBakeTiming {
    std::atomic<float> simulationSeconds{ 0.0f };
    std::atomic<float> renderSeconds{ 0.0f };
    std::atomic<float> outputSeconds{ 0.0f };
    std::atomic<int> stage{ 0 };
    std::atomic<float> slowestSimulationFrameSeconds{ 0.0f };
    std::atomic<int> slowestSimulationFrame{ -1 };
    std::atomic<float> slowestRenderFrameSeconds{ 0.0f };
    std::atomic<int> slowestRenderFrame{ -1 };
};

/// @brief rgba が事前乗算かどうか (プレビューの合成とテクスチャ .meta が使う)。
[[nodiscard]] bool FluidShadingIsPremultiplied(fluid::FluidShading shading);

/// @brief size は描く解像度。格子より大きく描くときは双三次で補間し、流れに乗せた細部ノイズを重ねる。
void RenderFluidGasFrame(const fluid::FluidGasSolver& solver, const fluid::FluidRecipe& recipe,
                         int size, float frameDt, FluidFrameImage& out);
void RenderFluidLiquidFrame(const fluid::FluidLiquidSolver& solver, const fluid::FluidRecipe& recipe,
                            int size, float frameDt, FluidFrameImage& out);

/// @brief 焼きの «その番号のコマ» を解いて描く。焼き (BakeFluid) もプレビューもここを通る。
/// @note プレビュー専用に解き直すと warmup の進め方が焼きとずれ別シミュレーションになる
/// @note (substeps 既定 2・プリセット 3〜4)。ここを唯一の入口にし、刻み・超解像・ループの
/// @note クロスフェードまで焼きと同じ規則で再現する。
/// @param frames 欲しいコマの番号。[0, columns×rows + ループの重ね分) へ丸める。**同じ番号を 2 度入れないこと**
/// @param size   1 コマの 1 辺 [px]。0 以下なら output.frameSize
/// @param out    frames と同じ並びで返る
/// @param cancel 非 null で true になったら諦めて false を返す
[[nodiscard]] bool RenderFluidBakeFrames(const fluid::FluidRecipe& recipe, std::span<const int> frames, int size,
                                         std::vector<FluidFrameImage>& out,
                                         std::atomic<float>* progress = nullptr,
                                         const std::atomic<bool>* cancel = nullptr,
                                         FluidBakeTiming* timing = nullptr);

/// @brief 1 コマだけ欲しいときの入口 (RenderFluidBakeFrames の薄い包み)。
[[nodiscard]] bool RenderFluidBakeFrame(const fluid::FluidRecipe& recipe, int frame, int size, FluidFrameImage& out,
                                        std::atomic<float>* progress = nullptr,
                                        const std::atomic<bool>* cancel = nullptr);

struct FluidBakeResult {
    bool success = false;
    std::string message;
    std::string albedoPath;        ///< @brief 実パス
    std::string motionVectorPath;  ///< @brief 焼いていなければ空
    std::string vectorFieldPath;   ///< @brief 焼いていなければ空
    int columns = 1;
    int rows = 1;
    int frameSize = 0;
    int gridResolution = 0;        ///< @brief 2D で実際に解いた格子の 1 辺 (液体は 0)
    int supersampling = 1;         ///< @brief 実際に使った超解像の倍率
    /// @name 焼いた絵に合う .mat の設定
    /// @{
    renderer::BlendMode blendMode = renderer::BlendMode::ALPHA_BLEND;
    scene::ParticleFlipbookMode flipbookMode = scene::ParticleFlipbookMode::Lifetime;
    float framesPerSecond = 24.0f;
    /// @brief 焼いた Motion Vector が前提にしている強さ。これ以外の値では warp がずれる。
    float motionVectorStrength = 0.0f;
    bool  distortion = false;
    float emissiveScale = 1.0f;
    /// @brief 焼いたアトラス (色 + MV) の指紋。同じ環境で同じレシピを焼けば同じ値になる。
    /// @brief 変えたパラメータが絵に効いたかを、画像を見比べずに 1 回の文字列比較で判定するための値。
    std::string fingerprint;
    /// @brief 実際に解いたソルバー ("cpu" / "gpu")。2D は CPU しか持たない。
    std::string solverUsed = "cpu";
    /// @brief GPU を頼んだのに CPU へ落ちた理由 (落ちていなければ空)。
    std::string fallbackReason;
    /// @}
};

/// @brief レシピを解いて焼く。basePath は拡張子なしの出力先で、
/// @brief `"<base>_Flipbook.png"` / `"<base>_MV.png"` / `"<base>_Velocity.png"` (と各 .meta) を上書きする。
/// @param progress 非 null なら [0,1] の進み具合を書く (別スレッドから読んでよい)
/// @note 同期処理 (解像度とコマ数により数秒〜数十秒)。**別スレッドから呼んでよい** —
/// @note PNG の書き出し (WIC) に要る COM はこの関数が呼び出しスレッドで初期化する。
[[nodiscard]] FluidBakeResult BakeFluid(const fluid::FluidRecipe& recipe, const std::string& basePath,
                                        std::atomic<float>* progress = nullptr,
                                        FluidBakeTiming* timing = nullptr,
                                        const std::atomic<bool>* cancel = nullptr);

} // namespace fbzz::asset
