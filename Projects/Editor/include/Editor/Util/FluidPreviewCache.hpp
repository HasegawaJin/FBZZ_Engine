/// @file    FluidPreviewCache.hpp
/// @brief   Fluid Editor の 2D ライブプレビュー — 裏のスレッドで解いたコマを取っておき、スクラブで引く
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note 変わった部品が効き始める時刻 (FluidInvalidationTime) より後だけを捨てて裏で解き直す。
/// @note       コマの時刻は焼きと同じ刻み (output.duration ÷ columns×rows。warmup の後を 0 とする)。
#pragma once

#include <Fluid/FluidRecipe.hpp>
#include <imgui.h>

#include <cstdint>
#include <memory>

namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }
namespace fbzz::asset { struct FluidFrameImage; }

namespace fbzz::editor {

enum class FluidPreviewQuality : std::uint8_t {
    Draft = 0,  ///< 格子 48 / 画像は 256px まで。触っている間の既定
    Normal,     ///< 格子 96 / 画像は 384px まで
    Final,      ///< 焼きと同じ格子 / 画像は焼きのコマの大きさ以上 512px まで (重い)
};

/// @brief 2D Fluid プレビュー用の背景色。市松は 8 画素単位、無地は暗色を返す。
[[nodiscard]] ImU32 FluidPreviewBackgroundPixel(int x, int y, bool checkerBackground);
/// @brief 2D Fluid プレビューの色を、背景と FluidShading のブレンド規則で不透明色へ合成する。
/// @note x / y / imageSide は元画像の画素、viewSidePixels は画面に出す一辺。市松は表示上 8 画素ごとに切り替える。
void CompositeFluidPreviewPixel(const float rgba[4], int x, int y, int imageSide, float viewSidePixels,
                                fluid::FluidShading shading, bool checkerBackground, float outRgb[3]);

/// @note before → after の変更で、解き直しが要る最初の時刻 [秒、warmup の後を 0]。
/// @note 部品だけが変わったなら、変わった部品の startTime と動きのキーの最小 (warmup 中から効く部品なら 0)。
/// @note 全体の設定 (Simulation / Output / 種類 / seed / 部品の数) が変わったら 0。Look だけの変更は描き直しだけで済むので
/// @note -1 (解き直し不要) を返す。
[[nodiscard]] float FluidInvalidationTime(const fluid::FluidRecipe& before, const fluid::FluidRecipe& after);

class FluidPreviewCache {
public:
    FluidPreviewCache();
    ~FluidPreviewCache();
    FluidPreviewCache(const FluidPreviewCache&) = delete;
    FluidPreviewCache& operator=(const FluidPreviewCache&) = delete;

    /// @note プレビューするレシピ (hide/solo を反映したもの)。revision が前と同じなら何もしない。
    /// @note invalidateFrom より後のコマを捨てる (0 = 全部、負 = 解き直さず描き直しだけ)。
    void SetRecipe(const fluid::FluidRecipe& recipe, std::uint64_t revision, float invalidateFrom);
    void SetQuality(FluidPreviewQuality quality);
    [[nodiscard]] FluidPreviewQuality Quality() const;
    /// @note 背景だけを切り替える。ソルバーは変更せず、現在の GPU テクスチャは次の TextureAt で合成し直す。
    void SetCheckerBackground(bool enabled);

    /// @note 画面に出している正方形の一辺 [画面画素]。これより小さく描いた絵を引き伸ばすとテクセルが四角く見えるので、
    /// @note 画像の一辺をここに合わせる (品質ごとの上限まで)。64 画素刻みに丸めるので、窓を少し伸縮しただけでは
    /// @note 描き直しにならない。渡さなければ 128px で描く。
    void SetPreviewSide(float sidePixels);

    /// @note 毎フレーム (UI スレッドで) 呼ぶ。裏で解けたコマを取り込み、次の解きを出し、表示中のコマを GPU へ上げる。
    void Tick(renderer::ResourceManager* resources, renderer::IImGuiRenderer* imguiRenderer);

    /// @brief time (warmup の後を 0) に一番近い解けたコマ。まだ無ければ nullptr。
    /// @note shading を指定した場合は返すコマの合成モードを設定する。コマが無ければ変更しない。
    [[nodiscard]] const asset::FluidFrameImage* FrameAt(float time, fluid::FluidShading* shading = nullptr) const;
    /// @note FrameAt(time) を GPU へ上げたもの。上げられない (レンダラーが無い等) なら 0 — 呼び手は FrameAt を矩形で描く。
    [[nodiscard]] ImTextureID TextureAt(float time);

    /// @note 頭から途切れずに解けている最後の時刻 (タイムラインの «解けた帯»)。
    [[nodiscard]] float SolvedUntil() const;
    [[nodiscard]] float Duration() const;
    [[nodiscard]] int FrameCount() const;
    [[nodiscard]] float FrameDt() const;
    [[nodiscard]] bool IsSolving() const;

    /// @note 裏の解きを止めて GPU 資源を返す。
    void Shutdown(renderer::ResourceManager* resources);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
