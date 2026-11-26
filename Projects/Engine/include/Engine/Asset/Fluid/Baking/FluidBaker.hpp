/// @file    FluidBaker.hpp
/// @brief   流体レシピを焼く — 1 コマの絵 (プレビューと共有)・フリップブック・Motion Vector・速度場 PNG
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note Inspector のライブプレビューが別の描き方をすると «プレビューでは濃いのに焼くと薄い» が
/// @note 起きるため、プレビューも焼きも同じ RenderFluid*Frame() を通す。
#pragma once
#include <Engine/Api.hpp>

#include <Fluid/Recipe/FluidRecipe.hpp>
#include <Graphics/Renderer/State/RenderState.hpp>
#include <Engine/Scene/Components/Particles/ParticleTypes.hpp>
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
    int size = 0; ///< このフレーム画像の一辺 [px]。
    std::vector<float> rgba; ///< 行優先で格納したこのフレームの線形 RGBA データ。
    std::vector<float> motion; ///< このフレームの各テクセルに対応するモーションの浮動小数点データ。
};

/// @brief 2D Bake の工程別実測値。ワーカースレッドが書き、Editor が読む。
struct FluidBakeTiming {
    std::atomic<float> simulationSeconds{ 0.0f }; ///< 流体ソルバーを進めた時間の合計 [s]。
    std::atomic<float> renderSeconds{ 0.0f }; ///< 流体の各コマを描画した時間の合計 [s]。
    std::atomic<float> outputSeconds{ 0.0f }; ///< アトラスを組み立て、出力した時間の合計 [s]。
    std::atomic<int> stage{ 0 }; ///< 進捗を照会する側へ公開する現在のベイク工程番号。
    std::atomic<float> slowestSimulationFrameSeconds{ 0.0f }; ///< 最も時間を要したコマのシミュレーション時間 [s]。
    std::atomic<int> slowestSimulationFrame{ -1 }; ///< シミュレーションの最長時間を記録したコマ番号。-1 は未計測。
    std::atomic<float> slowestRenderFrameSeconds{ 0.0f }; ///< 最も時間を要したコマの描画時間 [s]。
    std::atomic<int> slowestRenderFrame{ -1 }; ///< 描画の最長時間を記録したコマ番号。-1 は未計測。
};

/// @brief rgba が事前乗算かどうか (プレビューの合成とテクスチャ .meta が使う)。
[[nodiscard]] FBZZ_ENGINE_API bool FluidShadingIsPremultiplied(fluid::FluidShading shading);

/// @brief size は描く解像度。格子より大きく描くときは双三次で補間し、流れに乗せた細部ノイズを重ねる。
FBZZ_ENGINE_API void RenderFluidGasFrame(const fluid::FluidGasSolver& solver, const fluid::FluidRecipe& recipe,
                         int size, float frameDt, FluidFrameImage& out);
FBZZ_ENGINE_API void RenderFluidLiquidFrame(const fluid::FluidLiquidSolver& solver, const fluid::FluidRecipe& recipe,
                            int size, float frameDt, FluidFrameImage& out);

/// @brief 焼きの «その番号のコマ» を解いて描く。焼き (BakeFluid) もプレビューもここを通る。
/// @note プレビュー専用に解き直すと warmup の進め方が焼きとずれ別シミュレーションになる
/// @note (substeps 既定 2・プリセット 3〜4)。ここを唯一の入口にし、刻み・超解像・ループの
/// @note クロスフェードまで焼きと同じ規則で再現する。
/// @param[in] frames 欲しいコマの番号。[0, columns×rows + ループの重ね分) へ丸める。**同じ番号を 2 度入れないこと**
/// @param[in] size   1 コマの 1 辺 [px]。0 以下なら output.frameSize
/// @param[out] out    frames と同じ並びで返る
/// @param[in] cancel 非 null で true になったら諦めて false を返す
[[nodiscard]] FBZZ_ENGINE_API bool RenderFluidBakeFrames(const fluid::FluidRecipe& recipe, std::span<const int> frames, int size,
                                         std::vector<FluidFrameImage>& out,
                                         std::atomic<float>* progress = nullptr,
                                         const std::atomic<bool>* cancel = nullptr,
                                         FluidBakeTiming* timing = nullptr);

/// @brief 1 コマだけ欲しいときの入口 (RenderFluidBakeFrames の薄い包み)。
[[nodiscard]] FBZZ_ENGINE_API bool RenderFluidBakeFrame(const fluid::FluidRecipe& recipe, int frame, int size, FluidFrameImage& out,
                                        std::atomic<float>* progress = nullptr,
                                        const std::atomic<bool>* cancel = nullptr);

struct FluidBakeResult {
    bool success = false; ///< 要求した流体ベイクの生成と保存を完了したか。
    std::string message; ///< ベイクの失敗や中止などを呼び出し側へ伝える診断。
    std::string albedoPath; ///< 実パス
    std::string motionVectorPath; ///< 焼いていなければ空
    std::string vectorFieldPath; ///< 焼いていなければ空
    int columns = 1; ///< 出力フリップブックの横方向のコマ数。
    int rows = 1; ///< 出力フリップブックの縦方向のコマ数。
    int frameSize = 0; ///< 出力フリップブック 1 コマの一辺 [px]。
    int gridResolution = 0; ///< 2D で実際に解いた格子の 1 辺 (液体は 0)
    int supersampling = 1; ///< 実際に使った超解像の倍率
    /// @name 焼いた絵に合う .mat の設定
    /// @{
    renderer::BlendMode blendMode = renderer::BlendMode::ALPHA_BLEND; ///< このベイク結果を再生する材質に設定するブレンド方式。
    scene::ParticleFlipbookMode flipbookMode = scene::ParticleFlipbookMode::Lifetime; ///< 再生側が経過時間からコマを選ぶ方式。
    float framesPerSecond = 24.0f; ///< 再生側に渡すフリップブックのフレームレート [frame/s]。
    float motionVectorStrength = 0.0f; ///< 焼いた Motion Vector が前提にしている強さ。これ以外の値では warp がずれる。
    bool  distortion = false; ///< このベイク結果が歪み描画向けか。
    float emissiveScale = 1.0f; ///< ベイクした色の発光輝度を材質へ戻すための倍率。
    /// @brief 焼いたアトラス (色 + MV) の指紋。同じ環境で同じレシピを焼けば同じ値になる。
    /// @brief 変えたパラメータが絵に効いたかを、画像を見比べずに 1 回の文字列比較で判定するための値。
    std::string fingerprint; ///< ベイク結果が現在のソース設定と対応するかを照合する指紋。
    std::string solverUsed = "cpu"; ///< 実際に解いたソルバー ("cpu" / "gpu")。2D は CPU しか持たない。
    std::string fallbackReason; ///< GPU を頼んだのに CPU へ落ちた理由 (落ちていなければ空)。
    /// @}
};

/// @brief レシピを解いて焼く。basePath は拡張子なしの出力先で、
/// @brief `"<base>_Flipbook.png"` / `"<base>_MV.png"` / `"<base>_Velocity.png"` (と各 .meta) を上書きする。
/// @param[out] progress 非 null なら [0,1] の進み具合を書く (別スレッドから読んでよい)
/// @note 同期処理 (解像度とコマ数により数秒〜数十秒)。**別スレッドから呼んでよい** —
/// @note PNG の書き出し (WIC) に要る COM はこの関数が呼び出しスレッドで初期化する。
[[nodiscard]] FBZZ_ENGINE_API FluidBakeResult BakeFluid(const fluid::FluidRecipe& recipe, const std::string& basePath,
                                        std::atomic<float>* progress = nullptr,
                                        FluidBakeTiming* timing = nullptr,
                                        const std::atomic<bool>* cancel = nullptr);

} // namespace fbzz::asset
