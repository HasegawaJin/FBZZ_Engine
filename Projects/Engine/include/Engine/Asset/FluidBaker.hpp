/// @file    FluidBaker.hpp
/// @brief   流体レシピを焼く — 1 コマの絵 (プレビューと共有)・フリップブック・Motion Vector・.vfield
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY 1 コマの描画を公開するか:
///   Inspector のライブプレビューが別の描き方をすると «プレビューでは濃いのに焼くと薄い» が起きる。
///   プレビューも焼きも同じ RenderFluid*Frame() を通すので、見えたものがそのまま焼ける。
#pragma once

#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Scene/ScriptProxy/ScriptParticleProxy.hpp>
#include <atomic>
#include <span>
#include <string>
#include <vector>

namespace fbzz::asset {

class FluidGasSolver;
class FluidLiquidSolver;

/// 1 コマぶんの絵。
///   rgba   … PNG へそのまま書く値 (sRGB・[0,1])。Fire は事前乗算、それ以外はストレート
///   motion … «次のコマまでに中身が動く量» をコマの UV 単位で持つ (x: 右, y: 下)
struct FluidFrameImage {
    int size = 0;
    std::vector<float> rgba;
    std::vector<float> motion;
};

/// rgba が事前乗算かどうか (プレビューの合成とテクスチャ .meta が使う)。
[[nodiscard]] bool FluidShadingIsPremultiplied(FluidShading shading);

/// size は描く解像度。格子より大きく描くときは双三次で補間し、流れに乗せた細部ノイズを重ねる。
void RenderFluidGasFrame(const FluidGasSolver& solver, const FluidRecipe& recipe,
                         int size, float frameDt, FluidFrameImage& out);
void RenderFluidLiquidFrame(const FluidLiquidSolver& solver, const FluidRecipe& recipe,
                            int size, float frameDt, FluidFrameImage& out);

/// 焼きの «その番号のコマ» を解いて描く。焼き (BakeFluid) もプレビューもここを通る。
///
/// WHY プレビュー専用に解き直さないか:
///   プレビューが独自に刻んでいたころは warmup の進め方が焼きと違い、別のシミュレーションになっていた
///   (substeps は既定 2・プリセットは 3〜4 なので、どのレシピでもずれていた)。ここを唯一の入口に
///   すれば、刻み (FluidStepping)・超解像・ループのクロスフェードまで焼きと同じ規則で再現される。
///
/// @param frames 欲しいコマの番号。[0, columns×rows + ループの重ね分) へ丸める。**同じ番号を 2 度入れないこと**
/// @param size   1 コマの 1 辺 [px]。0 以下なら output.frameSize
/// @param out    frames と同じ並びで返る
/// @param cancel 非 null で true になったら諦めて false を返す
[[nodiscard]] bool RenderFluidBakeFrames(const FluidRecipe& recipe, std::span<const int> frames, int size,
                                         std::vector<FluidFrameImage>& out,
                                         std::atomic<float>* progress = nullptr,
                                         const std::atomic<bool>* cancel = nullptr);

/// 1 コマだけ欲しいときの入口 (RenderFluidBakeFrames の薄い包み)。
[[nodiscard]] bool RenderFluidBakeFrame(const FluidRecipe& recipe, int frame, int size, FluidFrameImage& out,
                                        std::atomic<float>* progress = nullptr,
                                        const std::atomic<bool>* cancel = nullptr);

struct FluidBakeResult {
    bool success = false;
    std::string message;
    std::string albedoPath;        ///< 実パス
    std::string motionVectorPath;  ///< 焼いていなければ空
    std::string vectorFieldPath;   ///< 焼いていなければ空
    int columns = 1;
    int rows = 1;
    int frameSize = 0;
    int gridResolution = 0;        ///< 2D で実際に解いた格子の 1 辺 (液体は 0)
    int supersampling = 1;         ///< 実際に使った超解像の倍率
    // ── 焼いた絵に合う .mat の設定 ──
    renderer::BlendMode blendMode = renderer::BlendMode::ALPHA_BLEND;
    scene::ParticleFlipbookMode flipbookMode = scene::ParticleFlipbookMode::Lifetime;
    float framesPerSecond = 24.0f;
    /// 焼いた Motion Vector が前提にしている強さ。これ以外の値では warp がずれる。
    float motionVectorStrength = 0.0f;
    bool  distortion = false;
    float emissiveScale = 1.0f;
    /// 焼いたアトラス (色 + MV) の指紋。同じ環境で同じレシピを焼けば同じ値になる。
    /// 変えたパラメータが絵に効いたかを、画像を見比べずに 1 回の文字列比較で判定するための値。
    std::string fingerprint;
    /// 実際に解いたソルバー ("cpu" / "gpu")。2D は CPU しか持たない。
    std::string solverUsed = "cpu";
    /// GPU を頼んだのに CPU へ落ちた理由 (落ちていなければ空)。
    std::string fallbackReason;
};

/// レシピを解いて焼く。basePath は拡張子なしの出力先で、
/// "<base>_Flipbook.png" / "<base>_MV.png" / "<base>.vfield" (と各 .meta) を上書きする。
/// @param progress 非 null なら [0,1] の進み具合を書く (別スレッドから読んでよい)
/// @note 同期処理 (解像度とコマ数により数秒〜数十秒)。**別スレッドから呼んでよい** —
///       PNG の書き出し (WIC) に要る COM はこの関数が呼び出しスレッドで初期化する。
[[nodiscard]] FluidBakeResult BakeFluid(const FluidRecipe& recipe, const std::string& basePath,
                                        std::atomic<float>* progress = nullptr);

} // namespace fbzz::asset
