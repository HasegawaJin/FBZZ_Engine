/// @file    FluidPreviewCache.hpp
/// @brief   Fluid Editor の 2D ライブプレビュー — 裏のスレッドで解いたコマを取っておき、スクラブで引く
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY コマを取っておくか:
///   以前の Inspector は値を 1 つ触るたびに t = 0 から UI スレッドで解き直していた (warmup の間は固まる)。
///   爆発の 0.8 秒目を詰めたいのに毎回頭へ戻る。ここは解いたコマを残し、変わった部品が効き始める時刻
///   (FluidInvalidationTime) より後だけを捨てて裏で解き直す。
///
/// コマの時刻は焼きと同じ刻み (output.duration ÷ columns×rows。warmup の後を 0 とする)。
#pragma once

#include <Engine/Asset/FluidRecipe.hpp>
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

/// before → after の変更で、解き直しが要る最初の時刻 [秒、warmup の後を 0]。
/// 部品だけが変わったなら、変わった部品の startTime と動きのキーの最小 (warmup 中から効く部品なら 0)。
/// 全体の設定 (Simulation / Output / 種類 / seed / 部品の数) が変わったら 0。Look だけの変更は描き直しだけで済むので
/// -1 (解き直し不要) を返す。
[[nodiscard]] float FluidInvalidationTime(const asset::FluidRecipe& before, const asset::FluidRecipe& after);

class FluidPreviewCache {
public:
    FluidPreviewCache();
    ~FluidPreviewCache();
    FluidPreviewCache(const FluidPreviewCache&) = delete;
    FluidPreviewCache& operator=(const FluidPreviewCache&) = delete;

    /// プレビューするレシピ (hide/solo を反映したもの)。revision が前と同じなら何もしない。
    /// invalidateFrom より後のコマを捨てる (0 = 全部、負 = 解き直さず描き直しだけ)。
    void SetRecipe(const asset::FluidRecipe& recipe, std::uint64_t revision, float invalidateFrom);
    void SetQuality(FluidPreviewQuality quality);
    [[nodiscard]] FluidPreviewQuality Quality() const;

    /// 画面に出している正方形の一辺 [画面画素]。これより小さく描いた絵を引き伸ばすとテクセルが四角く見えるので、
    /// 画像の一辺をここに合わせる (品質ごとの上限まで)。64 画素刻みに丸めるので、窓を少し伸縮しただけでは
    /// 描き直しにならない。渡さなければ 128px で描く。
    void SetPreviewSide(float sidePixels);

    /// 毎フレーム (UI スレッドで) 呼ぶ。裏で解けたコマを取り込み、次の解きを出し、表示中のコマを GPU へ上げる。
    void Tick(renderer::ResourceManager* resources, renderer::IImGuiRenderer* imguiRenderer);

    /// time (warmup の後を 0) に一番近い解けたコマ。まだ無ければ nullptr。
    [[nodiscard]] const asset::FluidFrameImage* FrameAt(float time) const;
    /// FrameAt(time) を GPU へ上げたもの。上げられない (レンダラーが無い等) なら 0 — 呼び手は FrameAt を矩形で描く。
    [[nodiscard]] ImTextureID TextureAt(float time);

    /// 頭から途切れずに解けている最後の時刻 (タイムラインの «解けた帯»)。
    [[nodiscard]] float SolvedUntil() const;
    [[nodiscard]] float Duration() const;
    [[nodiscard]] int FrameCount() const;
    [[nodiscard]] float FrameDt() const;
    [[nodiscard]] bool IsSolving() const;

    /// 裏の解きを止めて GPU 資源を返す。
    void Shutdown(renderer::ResourceManager* resources);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fbzz::editor
