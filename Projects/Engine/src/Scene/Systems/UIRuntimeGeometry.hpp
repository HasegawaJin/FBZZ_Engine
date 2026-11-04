/// @file    UIRuntimeGeometry.hpp
/// @brief   入力・レイアウト・描画で共有する Canvas の矩形と座標変換。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Scene/Systems/UIInteraction.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UICanvasGroup.hpp>
#include <Engine/Scene/Components/UIRect.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIElement.hpp>
#include <Engine/Scene/UITheme.hpp>
#include <Engine/Asset/UIThemeAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/Utf8.hpp>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include "CanvasWorklist.hpp"
namespace fbzz::scene::ui_runtime_detail {
struct CanvasEntry {
    GameObject* go = nullptr;
    UICanvas* canvas = nullptr;
    std::optional<asset::UIThemeStyle> theme;
};
struct Rect { math::Vector2 pos; math::Vector2 size; };
/// @note この要素がクリップを張るか。Mask と Scroll View のどちらも中身を枠で切る。
/// @note Scroll View も切るのは、切らないと枠の外へはみ出したぶんが見えてしまうため。
inline bool ElementClipsChildren(GameObject& go)
{
    if (const auto* mask = go.GetComponent<UIMask>(); mask && mask->enabled && mask->affectChildren)
        return true;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        return true;
    return false;
}

inline int GetUISortOrder(GameObject& go)
{
    /// @note UIImage と UIText を同じ GO に持つ Button は一つの描画単位として扱う。
    /// @note 両方に値がある場合は手前側を採用し、どちらの Inspector からでも調整できるようにする。
    int order = 0;
    if (const auto* image = go.GetComponent<UIImage>()) order = image->sortOrder;
    if (const auto* text = go.GetComponent<UIText>()) order = (std::max)(order, text->sortOrder);
    return order;
}

/// @note 子を sortOrder 順に out へ並べる。
/// @note 出力引数なのは UI ノード 1 つにつき毎フレーム呼ばれるため (vector を返すと
/// @note 確保と解放が規模 × フレームレートで走り続ける)。
/// @note 並びは子の処理中も読み続けるので、呼び出し側は深さごとに別の領域を渡すこと
/// @note (AcquireChildScratch を参照)。
inline void SortUIChildren(GameObject& go, std::vector<GameObject*>& out)
{
    out.clear();
    out.reserve(static_cast<size_t>(go.GetChildCount()));
    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i)) out.push_back(child);
    }
    /// @note 同値時は Hierarchy 順を維持し、既存シーンの見た目を変えない。
    std::stable_sort(out.begin(), out.end(),
        [](GameObject* a, GameObject* b) {
            return GetUISortOrder(*a) < GetUISortOrder(*b);
        });
}

/// @note 再帰の深さごとに 1 本ずつ領域を貸す。1 本を共有すると最初の子を降りた先で
/// @note 親のリストが上書きされ、2 番目以降の兄弟が消える。
/// @note 返す参照はより深い階層で再度呼ばれても生き続ける
/// @note (UIInteractionContext::childScratch が deque である理由)。
inline std::vector<GameObject*>& AcquireChildScratch(UIInteractionContext& ctx, std::size_t depth)
{
    if (ctx.childScratch.size() <= depth)
        ctx.childScratch.resize(depth + 1);
    return ctx.childScratch[depth];
}

struct UITransform2D {
    math::Vector2 position = math::Vector2::ZERO;
    float rotationZ = 0.0f;
    /// @note 親要素の矩形サイズ。アンカーはこれに対する割合なので、階層を降りるときに
    /// @note 一緒に運ばないと「親のどこ」が決まらない。Canvas 直下では Canvas の寸法。
    math::Vector2 parentSize = math::Vector2::ZERO;
};

/// @param parentSize 子から見た親の矩形サイズ。アンカーの基準になる。
inline UITransform2D ComposeUITransform(const UITransform2D& parent, const scene::Transform& local,
                                 const math::Vector2& parentSize)
{
    /// @note 親の位置と回転だけ継承し、サイズは各要素の localScale.xy から読む。
    /// @note UI の localScale.xy は倍率ではなく幅・高さなので、TransformSystem のように
    /// @note 親 scale を子 position へ掛けると親のサイズ変更だけで子が飛ぶ。
    const math::Vector2 localPos = { local.position.x, local.position.y };
    UITransform2D result{};
    result.position = parent.position + RotateUIVector(localPos, parent.rotationZ);
    result.rotationZ = parent.rotationZ + ExtractUIZRotation(local.rotation);
    /// @note アンカー / ピボットの解釈は ResolveUIRect に閉じる。
    /// @note ここが運ぶのは「子から見た親のサイズ」だけ。
    result.parentSize = parentSize;
    return result;
}

/// @note UI 要素の矩形。アンカー・ピボットの解釈は Components/UIRect.hpp が唯一の定義。
/// @note size を引数で受けるのは、画像は scale.xy そのままだが文字は実測が要るため。
inline Rect ResolveElementRect(const UITransform2D& resolved,
                        const math::Vector2& size,
                        const UIAnchor& anchoring)
{
    const UIRect rect = ResolveUIRect(resolved.parentSize,
                                      resolved.position, size, anchoring);
    return { rect.position, rect.size };
}

/// @note UI 要素の矩形サイズ。矩形の解決・当たり判定・レイアウトの 3 箇所が必要とする。
/// @note 画像は transform.scale.xy が正、文字は実測 (UIText::resolvedSize) が正で
/// @note scale はその写し。どちらを見るかを決めるのはここだけにする。
inline math::Vector2 UIElementSize(scene::GameObject& go)
{
    const math::Vector2 scaleSize = { go.transform.scale.x, go.transform.scale.y };
    if (go.GetComponent<UIImage>()) return scaleSize;
    if (const auto* text = go.GetComponent<UIText>()) {
        if (text->resolvedSize.x > 0.0f || text->resolvedSize.y > 0.0f)
            return text->resolvedSize;
    }
    return scaleSize;
}

inline UIAnchor UIElementAnchoring(scene::GameObject& go)
{
    if (const auto* image = go.GetComponent<UIImage>()) return image->anchoring;
    if (const auto* text = go.GetComponent<UIText>())   return text->anchoring;
    return {};
}

/// @note GameObject の UI 矩形。持っているコンポーネントに応じてサイズの出所だけが変わり、
/// @note アンカーの解き方は共通。
inline Rect RectFromTransform(scene::GameObject& go, const UITransform2D& resolved)
{
    return ResolveElementRect(resolved, UIElementSize(go), UIElementAnchoring(go));
}

/// @name Matrix4 × Vector4
/// @note Matrix4 に operator*(Vector4) が無いので、WorldSpace のヒット判定用にここで持つ。
inline static math::Vector4 MulMV(const math::Matrix4& m, const math::Vector4& v)
{
    return {
        m.m[0][0]*v.x + m.m[0][1]*v.y + m.m[0][2]*v.z + m.m[0][3]*v.w,
        m.m[1][0]*v.x + m.m[1][1]*v.y + m.m[1][2]*v.z + m.m[1][3]*v.w,
        m.m[2][0]*v.x + m.m[2][1]*v.y + m.m[2][2]*v.z + m.m[2][3]*v.w,
        m.m[3][0]*v.x + m.m[3][1]*v.y + m.m[3][2]*v.z + m.m[3][3]*v.w,
    };
}

/// @name Canvas 収集
inline void CollectCanvases(Scene& scene, std::vector<CanvasEntry>& canvases)
{
    for (const auto id : ui_detail::CollectCanvasEntities(scene)) {
        CanvasEntry entry{ scene.GetGameObject(id), scene.GetComponent<UICanvas>(id), {} };
        asset::UIThemeStyle style;
        if (asset::ResolveUIThemeStyle(entry.canvas->theme.path, style)) entry.theme = std::move(style);
        canvases.push_back(std::move(entry));
    }
    std::sort(canvases.begin(), canvases.end(),
        [](const CanvasEntry& a, const CanvasEntry& b) {
            return a.canvas->sortOrder < b.canvas->sortOrder;
        });
}

inline bool IsScreenSpaceRenderMode(UIRenderMode mode);

inline bool ShouldRenderCanvas(const UICanvas& canvas, UIRenderTargetView targetView, bool sceneDepthAvailable = true)
{
    if (!sceneDepthAvailable && canvas.renderMode != UIRenderMode::ScreenSpaceOverlay
        && canvas.depthMode == UICanvasDepthMode::SCENE_DEPTH)
        return false;
    /// @note GameViewport は最終出力として全 Canvas、SceneViewport は WorldSpace、
    /// @note CanvasEditor は ScreenSpace だけ描く。
    if (targetView == UIRenderTargetView::SceneViewport)
        return canvas.renderMode == UIRenderMode::WorldSpace;
    if (targetView == UIRenderTargetView::CanvasEditor)
        return IsScreenSpaceRenderMode(canvas.renderMode);
    return true;
}

inline bool IsScreenSpaceRenderMode(UIRenderMode mode)
{
    return mode == UIRenderMode::ScreenSpaceOverlay
        || mode == UIRenderMode::ScreenSpaceCamera;
}

inline float ResolveCanvasScale(const UICanvas& canvas, float viewportWidth, float viewportHeight)
{
    /// @note Unity の Canvas Scaler と同じ考え方。基準解像度と現在 Viewport の差を
    /// @note UI 座標変換に集約する。
    if (canvas.scaleMode != UICanvasScaleMode::ScaleWithScreenSize)
        return 1.0f;

    const float refW = (std::max)(1.0f, canvas.referenceWidth);
    const float refH = (std::max)(1.0f, canvas.referenceHeight);
    const float scaleW = (std::max)(1.0f, viewportWidth) / refW;
    const float scaleH = (std::max)(1.0f, viewportHeight) / refH;
    const float match = std::clamp(canvas.matchWidthOrHeight, 0.0f, 1.0f);
    return std::exp(std::log(scaleW) * (1.0f - match) + std::log(scaleH) * match);
}

inline void ResolveScreenSpaceCanvasArea(const UICanvas& canvas,
                                  float viewportWidth,
                                  float viewportHeight,
                                  float& visibleCanvasW,
                                  float& visibleCanvasH)
{
    if (canvas.scaleMode == UICanvasScaleMode::ScaleWithScreenSize) {
        const float scale = ResolveCanvasScale(canvas, viewportWidth, viewportHeight);
        visibleCanvasW = (std::max)(1.0f, viewportWidth) / scale;
        visibleCanvasH = (std::max)(1.0f, viewportHeight) / scale;
        return;
    }
    visibleCanvasW = (std::max)(1.0f, canvas.canvasWidth);
    visibleCanvasH = (std::max)(1.0f, canvas.canvasHeight);
}

/// @note Canvas 直下の子から見た「親の矩形」の寸法。
/// @note Overlay の ScaleWithScreenSize では見えている範囲が viewport / scale で決まり、
/// @note canvasHeight と一致しない。ここがずれると anchor = 1 が画面端に来ない
/// @note (Editor の GetCanvasEditorSize() も最初からこの値で描いている)。
/// @note WorldSpace と ScreenSpaceCamera は板を 3D へ置いたものなので画面の広さと無関係。
inline math::Vector2 ResolveCanvasRectSize(const UICanvas& canvas,
                                    float viewportWidth,
                                    float viewportHeight)
{
    if (canvas.renderMode != UIRenderMode::ScreenSpaceOverlay) {
        return { (std::max)(1.0f, canvas.canvasWidth),
                 (std::max)(1.0f, canvas.canvasHeight) };
    }
    float visibleW = 1.0f, visibleH = 1.0f;
    ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight, visibleW, visibleH);
    return { visibleW, visibleH };
}

/// @note Canvas 直下の子から見た「親」。セーフエリアぶんだけ内側へ寄せた矩形になる。
/// @note 原点もずらすのは、寸法を縮めるだけだと左上に貼った要素が画面の角に残るため。
/// @note 入力・描画・レイアウト・選択マスクの 4 経路が同じ矩形を要るので、作るのはここだけ。
inline UITransform2D BuildCanvasRootTransform(const UICanvas& canvas,
                                       float viewportWidth, float viewportHeight)
{
    const math::Vector2 full = ResolveCanvasRectSize(canvas, viewportWidth, viewportHeight);
    UITransform2D root{};
    root.position   = { canvas.safeArea.x, canvas.safeArea.y };
    root.parentSize = {
        (std::max)(0.0f, full.x - canvas.safeArea.x - canvas.safeArea.z),
        (std::max)(0.0f, full.y - canvas.safeArea.y - canvas.safeArea.w)
    };
    return root;
}

/// @note Canvas 1px が実画面で何 px になるか。フォントを焼く解像度がこれで決まる。
/// @note Canvas Scaler の倍率は ConstantPixelSize で常に 1 だが、1920x1080 の Canvas を
/// @note 900px 幅へ映せば実際は 0.47 倍。「見えている領域」と viewport の比なら両モードを
/// @note 同じ式で扱える。WorldSpace を 1 に固定するのは、実寸がカメラ距離で毎フレーム変わり
/// @note グリフを焼き直し続けるため。
inline float ResolveCanvasPixelScale(const UICanvas& canvas, float viewportWidth, float viewportHeight)
{
    if (canvas.renderMode == UIRenderMode::WorldSpace) return 1.0f;

    float visibleW = 1.0f, visibleH = 1.0f;
    ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight, visibleW, visibleH);
    return (std::max)((std::max)(1.0f, viewportWidth)  / visibleW,
                      (std::max)(1.0f, viewportHeight) / visibleH);
}
}
