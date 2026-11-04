/// @file    UIInteraction.cpp
/// @brief   フォント計測・レイアウト・入力と通知の CPU 更新。
/// @author  Hasegawa Jin
/// @date    2026-10-31
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
#include <Engine/Scene/UIPointer.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Input/Gamepad.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Math/Ray.hpp>
#include <Math/Plane.hpp>
#include "UIRuntimeGeometry.hpp"
#include "UITextLayout.hpp"
#include "UIInputPolicy.hpp"
#include "../../Input/GameInputConsumption.hpp"
#include <unordered_set>
namespace fbzz::scene {
using namespace ui_runtime_detail;
namespace {
/// @name UIButton 更新
bool UpdateButton(UIButton& button, const Rect& rect, math::Vector2 mouse, bool mousePressed)
{
    return ui_detail::UpdateButton(button, rect.pos, rect.size, mouse, mousePressed);
}
renderer::FontAtlas& GetOrLoadFontMetrics(const std::string& basePath,
                                        float rasterPixelHeight,
                                        UIInteractionContext& ctx)
{
    /// @note 静的アトラスは焼く解像度を持たない (PNG が決め打ち) ので、解像度をキーへ混ぜると
    /// @note 同じ PNG を段の数だけ読み込むだけになる。動的モードのときだけ分ける。
    const bool dynamicFont = renderer::FontAtlas::IsDynamicFontPath(basePath)
        || (basePath.starts_with("guid:")
            && renderer::FontAtlas::IsDynamicFontPath(ResolveUIFontBasePath(basePath)));
    const std::string key = dynamicFont
        ? basePath + '@' + std::to_string(static_cast<int>(rasterPixelHeight))
        : basePath;

    /// @note キャッシュは指定されたパスで引く。解決結果でキーを作ると、同じ指定が
    /// @note プロジェクト側と共有側で二重にロードされうる。
    auto it = ctx.fontMetricsCache.find(key);
    if (it != ctx.fontMetricsCache.end())
        return it->second;

    renderer::FontAtlas& atlas = ctx.fontMetricsCache[key];
    const std::string resolved = ResolveUIFontBasePath(basePath);
    if (!atlas.LoadMetrics(resolved, rasterPixelHeight)) {
        FBZZ_LOG_ERROR("UISystem: failed to load FontAtlas [%s]\n  resolved: %s",
                       basePath.c_str(), resolved.c_str());
    }

    return atlas;
}
/// @note 実測サイズを返し、autoSize の結果を text へ書き戻す。
math::Vector2 ComputeTextLogicalSize(UIText& text,
                                     UIInteractionContext& ctx)
{
    const std::string& path = text.fontPath.empty() ? ctx.defaultFontPath : text.fontPath;
    renderer::FontAtlas& atlas =
        GetOrLoadFontMetrics(path, ResolveTextRasterPixelHeight(text.fontSize, ctx),
                           ctx);
    if (atlas.GetLineHeight() <= 0.0f) return {};

    /// @note 動的フォントではこの文字列に必要なグリフをここで焼く。幅を測る前に登録が要る。
    /// @note 静的フォントでは即 return する。
    atlas.PrepareTextMetrics(text.text);

    text.resolvedFontSize = text.autoSize
        ? SolveAutoFontSize(text, atlas, ctx.textLineScratch)
        : text.fontSize;

    LayoutTextLines(text, atlas, text.resolvedFontSize, ctx.textLineScratch);
    ApplyTextOverflow(text, ctx.textLineScratch);

    float maxW = 0.0f;
    for (const UITextLine& line : ctx.textLineScratch)
        maxW = (std::max)(maxW, line.width);

    /// @note 折り返し幅を指定したら箱の幅もそれにする。「一番長い行」に縮めると、
    /// @note 中央揃え・右揃えの基準が文言によって毎回変わる。
    const float width  = text.maxWidth > 0.0f ? text.maxWidth : maxW;
    const float lines  = TotalTextHeight(ctx.textLineScratch, text.lineSpacing);
    /// @note 箱の高さを指定していれば、中身が短くても箱は縮まない。
    /// @note 縦揃えも省略も「余りがある」ことを前提にしているので、ここで確定させる。
    const float height = text.maxHeight > 0.0f ? text.maxHeight : lines;
    return { width, height };
}

void UpdateTextSizesRecursive(GameObject& go,
                              UIInteractionContext& ctx,
                               const asset::UIThemeStyle* theme)
{
    if (!go.activeInHierarchy()) return;
    if (auto* text = go.GetComponent<UIText>(); text && text->enabled && !text->text.empty()) {
        math::Vector2 size;
        if (theme) {
            UIText working = *text;
            ApplyUIThemeToText(*theme, working);
            size = ComputeTextLogicalSize(working, ctx);
            text->resolvedFontSize = working.resolvedFontSize;
        } else {
            size = ComputeTextLogicalSize(*text, ctx);
        }
        /// @note 実測サイズの置き場はここ。アンカー・ピボット・当たり判定・ギズモが読む。
        text->resolvedSize = size;
        /// @note transform.scale へも書き戻す (後方互換の写し)。この行がある限り文字の
        /// @note transform.scale はユーザーが設定できない。UISystem 内の読み手は全部
        /// @note UIElementSize() 経由へ移したので外せるが、シーンやスクリプトが読んでいないか
        /// @note 実機で確認できていないので残している。
        go.transform.scale.x = size.x;
        go.transform.scale.y = size.y;
    }
    for (int i = 0; i < go.GetChildCount(); ++i)
        if (GameObject* child = go.GetChild(i))
            UpdateTextSizesRecursive(*child, ctx, theme);
}

void UITextSizeSystem(const std::vector<CanvasEntry>& canvases,
                      UIInteractionContext& ctx,
                      float viewportWidth,
                      float viewportHeight)
{
    for (const CanvasEntry& entry : canvases) {
        /// @note 描画と同じ解像度のアトラスを引く。寸法は段が違っても変わらないが、
        /// @note 計測側が別のアトラスを触ると同じ字を 2 つの実体へ焼くことになる。
        ctx.textPixelScale =
            ResolveCanvasPixelScale(*entry.canvas, viewportWidth, viewportHeight);
        UpdateTextSizesRecursive(*entry.go, ctx, entry.theme ? &*entry.theme : nullptr);
    }
}
/// @name UILayoutGroup
/// @note containerSize は go 自身の矩形。引数で受けるのは、Canvas が自分の矩形を持たず
/// @note UIElementSize(go) だと 1x1 の箱としてはみ出し警告を出し続けるため。
/// @note 揃えの余りをどれだけずらすかへ直す。
float LayoutAlignOffset(UILayoutAlign align, float slack)
{
    if (slack <= 0.0f) return 0.0f;
    if (align == UILayoutAlign::Center) return slack * 0.5f;
    if (align == UILayoutAlign::End)    return slack;
    return 0.0f;
}

/// @note レイアウトが子のサイズを書き換えてよいか。文字を外すのは、UIText のサイズは
/// @note 実測が正で毎フレーム書き戻されるため (広げても次のフレームには戻る)。
bool LayoutCanResizeChild(GameObject& child)
{
    return !HasMeasuredUISize(child);
}

void ApplyLayout(GameObject& go, const UILayoutGroup& layout,
                 const math::Vector2& containerSize, float canvasScale,
                 UIInteractionContext& ctx, const UILayoutGroup* authoredLayout)
{
    static std::unordered_set<const UILayoutGroup*> s_warnedLayouts;

    const int count = go.GetChildCount();

    /// @note 毎フレーム・レイアウトグループごとに確保しないよう作業領域を借りる。
    std::vector<int>& indices = ctx.layoutIndexScratch;
    indices.clear();
    indices.reserve(count);
    for (int i = 0; i < count; ++i) {
        GameObject* child = go.GetChild(i);
        if (!child || !child->activeInHierarchy()) continue;
        indices.push_back(i);
    }
    if (layout.reverseOrder)
        std::reverse(indices.begin(), indices.end());
    if (indices.empty()) return;

    /// @note 寸法は先に全部集める。揃え・伸縮・Grid の折り返しは、どれも
    /// @note 「全部の大きさが分かってから」でないと解けない。
    std::vector<math::Vector2>& sizes = ctx.layoutSizeScratch;
    sizes.clear();
    sizes.reserve(indices.size());
    for (int idx : indices) {
        /// @note 送り幅は transform.scale ではなく要素サイズから取る。文字の scale は
        /// @note 実測値の写しなので、書き戻しを外した瞬間にレイアウトだけ崩れる。
        sizes.push_back(UIElementSize(*go.GetChild(idx)));
    }

    const float innerW = containerSize.x - layout.paddingLeft - layout.paddingRight;
    const float innerH = containerSize.y - layout.paddingTop  - layout.paddingBottom;

    /// @name Grid
    if (layout.axis == UILayoutAxis::Grid) {
        /// @note マス目の大きさ。0 の軸は「その列で最も大きい子」ではなく最初の子に
        /// @note 合わせる ─ マス目が子ごとに変わると格子に見えなくなるため。
        math::Vector2 cell = layout.cellSize;
        if (cell.x <= 0.0f || cell.y <= 0.0f) {
            math::Vector2 largest{};
            for (const math::Vector2& size : sizes) {
                largest.x = (std::max)(largest.x, size.x);
                largest.y = (std::max)(largest.y, size.y);
            }
            if (cell.x <= 0.0f) cell.x = largest.x;
            if (cell.y <= 0.0f) cell.y = largest.y;
        }

        int columns = layout.gridColumns;
        if (columns <= 0) {
            const float stride = cell.x + layout.spacing;
            columns = stride > 0.0f
                ? static_cast<int>((innerW + layout.spacing) / stride) : 1;
        }
        columns = (std::max)(1, columns);

        const int rows = (static_cast<int>(indices.size()) + columns - 1) / columns;
        const float usedW = static_cast<float>(columns) * cell.x
                          + static_cast<float>(columns - 1) * layout.spacing;
        const float usedH = static_cast<float>(rows) * cell.y
                          + static_cast<float>(rows - 1) * layout.spacingCross;
        const float offsetX = LayoutAlignOffset(layout.alignMain,  innerW - usedW);
        const float offsetY = LayoutAlignOffset(layout.alignCross, innerH - usedH);

        for (std::size_t i = 0; i < indices.size(); ++i) {
            GameObject* child = go.GetChild(indices[i]);
            const int column = static_cast<int>(i) % columns;
            const int row    = static_cast<int>(i) / columns;
            const math::Vector2 cellOrigin = {
                layout.paddingLeft + offsetX + static_cast<float>(column) * (cell.x + layout.spacing),
                layout.paddingTop  + offsetY + static_cast<float>(row)    * (cell.y + layout.spacingCross)
            };
            if (layout.expandChildren && LayoutCanResizeChild(*child)) {
                child->transform.scale.x = cell.x;
                child->transform.scale.y = cell.y;
                sizes[i] = cell;
            }
            /// @note マスの中での寄せ。マスと子が同じ大きさなら 0 になる。
            child->transform.position.x =
                cellOrigin.x + LayoutAlignOffset(layout.alignCross, cell.x - sizes[i].x);
            child->transform.position.y =
                cellOrigin.y + LayoutAlignOffset(layout.alignCross, cell.y - sizes[i].y);
        }
        return;
    }

    /// @name Horizontal / Vertical
    const bool horizontal = layout.axis == UILayoutAxis::Horizontal;
    const float mainInner  = horizontal ? innerW : innerH;
    const float crossInner = horizontal ? innerH : innerW;
    const float totalSpacing = layout.spacing * static_cast<float>(indices.size() - 1);

    float mainUsed = totalSpacing;
    for (const math::Vector2& size : sizes) mainUsed += horizontal ? size.x : size.y;

    /// @note 余りを等分して配る。縮める方向へは配らない (縮めると文字が消えるので、
    /// @note 溢れているという事実は警告で伝えるほうが直せる)。
    float share = 0.0f;
    if (layout.expandChildren) {
        int resizable = 0;
        for (std::size_t i = 0; i < indices.size(); ++i)
            if (LayoutCanResizeChild(*go.GetChild(indices[i]))) ++resizable;
        if (resizable > 0 && mainInner > mainUsed)
            share = (mainInner - mainUsed) / static_cast<float>(resizable);
    }

    /// @note 余りを子へ配りきったなら、束としての余りは残らない = 揃えは効かない。
    const float mainSlack = share > 0.0f ? 0.0f : mainInner - mainUsed;
    const float cursorStart = (horizontal ? layout.paddingLeft : layout.paddingTop)
                            + LayoutAlignOffset(layout.alignMain, mainSlack);
    float cursor = cursorStart;

    for (std::size_t i = 0; i < indices.size(); ++i) {
        GameObject* child = go.GetChild(indices[i]);
        auto& t = child->transform;
        const bool resizable = LayoutCanResizeChild(*child);

        if (share > 0.0f && resizable) {
            if (horizontal) t.scale.x = sizes[i].x + share;
            else            t.scale.y = sizes[i].y + share;
            (horizontal ? sizes[i].x : sizes[i].y) += share;
        }
        if (layout.stretchCross && resizable && crossInner > 0.0f) {
            if (horizontal) { t.scale.y = crossInner; sizes[i].y = crossInner; }
            else            { t.scale.x = crossInner; sizes[i].x = crossInner; }
        }

        const float crossSlack = crossInner - (horizontal ? sizes[i].y : sizes[i].x);
        const float crossPos = (horizontal ? layout.paddingTop : layout.paddingLeft)
                             + LayoutAlignOffset(layout.alignCross, crossSlack);
        if (horizontal) {
            t.position.x = cursor;
            t.position.y = crossPos;
            cursor += sizes[i].x + layout.spacing;
        } else {
            t.position.x = crossPos;
            t.position.y = cursor;
            cursor += sizes[i].y + layout.spacing;
        }
    }

    /// @note paddingRight / paddingBottom がコンテナ末端として機能しているか検証する。
    /// @note クリップが入った今でも、はみ出した中身は「切れて見えない」形で表に出る。
    {
        const float endPos  = cursor - layout.spacing;
        const bool overflow = horizontal
            ? (endPos > containerSize.x - layout.paddingRight)
            : (endPos > containerSize.y - layout.paddingBottom);

        if (overflow && s_warnedLayouts.find(authoredLayout) == s_warnedLayouts.end()) {
            s_warnedLayouts.insert(authoredLayout);
            const float excess = horizontal
                ? (endPos - (containerSize.x - layout.paddingRight))
                : (endPos - (containerSize.y - layout.paddingBottom));
            FBZZ_LOG_WARN("UILayoutGroup: children overflow container by %.1f canvas-px (%.1f viewport-px, canvasScale=%.2f)",
                          excess, excess * canvasScale, canvasScale);
        }
    }
}

/// @note 子の外接矩形 (この要素のローカル座標) を測る。
/// @note 子のアンカーは親のサイズ ─ いま決めようとしている値 ─ に依存するので、
/// @note 循環を避けて「ローカル位置に素直に置かれている」ものとして測る。
math::Vector2 MeasureChildrenExtent(GameObject& go)
{
    math::Vector2 extent{};
    for (int i = 0; i < go.GetChildCount(); ++i) {
        GameObject* child = go.GetChild(i);
        if (!child || !child->activeInHierarchy()) continue;
        const math::Vector2 size = UIElementSize(*child);
        extent.x = (std::max)(extent.x, child->transform.position.x + size.x);
        extent.y = (std::max)(extent.y, child->transform.position.y + size.y);
    }
    return extent;
}

void ApplyContentSizeFitter(GameObject& go, const UIContentSizeFitter& fitter)
{
    const math::Vector2 extent = MeasureChildrenExtent(go);
    auto& scale = go.transform.scale;

    const auto fit = [](UISizeFitMode mode, float measured, float low, float high,
                        float current) {
        if (mode == UISizeFitMode::None) return current;
        float value = measured;
        if (low  > 0.0f) value = (std::max)(value, low);
        if (high > 0.0f) value = (std::min)(value, high);
        return value;
    };

    scale.x = fit(fitter.horizontalFit, extent.x + fitter.padding.x + fitter.padding.z,
                  fitter.minSize.x, fitter.maxSize.x, scale.x);
    scale.y = fit(fitter.verticalFit, extent.y + fitter.padding.y + fitter.padding.w,
                  fitter.minSize.y, fitter.maxSize.y, scale.y);
}

void ApplyUILayoutRecursive(GameObject& go, const math::Vector2& containerSize,
                            float canvasScale, UIInteractionContext& ctx,
                            const asset::UIThemeStyle* theme)
{
    if (!go.activeInHierarchy()) return;

    /// @note 並べるのは降りる前。子のローカル位置が決まっていないと、
    /// @note その子がさらに持つレイアウトの容器サイズも決まらない。
    if (auto* layout = go.GetComponent<UILayoutGroup>(); layout && layout->enabled) {
        if (theme) {
            UILayoutGroup working = *layout;
            ApplyUIThemeToLayout(*theme, working);
            ApplyLayout(go, working, containerSize, canvasScale, ctx, layout);
        } else {
            ApplyLayout(go, *layout, containerSize, canvasScale, ctx, layout);
        }
    }

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ApplyUILayoutRecursive(*child, UIElementSize(*child), canvasScale, ctx, theme);
    }

    /// @note 縮むのは帰りがけ。中身が全部片付いてからでないと外接矩形が出ない。
    if (auto* fitter = go.GetComponent<UIContentSizeFitter>(); fitter && fitter->enabled)
        ApplyContentSizeFitter(go, *fitter);
}
/// @name UIButton イベント処理
/// @param groupInteractable 上位の UICanvasGroup が入力を許しているか。
/// @param groupBlocksRaycasts false の群は、当たっても「吸わない」(背後へ通す)。
bool ProcessUIEventsRecursive(GameObject& go,
                              UIInteractionContext& ctx,
                              const UITransform2D& parentTransform,
                              math::Vector2 mouseInCanvasSpace,
                              bool mousePressed,
                              bool inputAvailable = true,
                              bool applySelfTransform = true,
                              std::size_t depth = 0,
                              bool groupInteractable = true,
                              bool groupBlocksRaycasts = true)
{
    if (!go.activeInHierarchy()) return false;

    if (const auto* group = go.GetComponent<UICanvasGroup>(); group && group->enabled) {
        if (group->ignoreParentGroups) {
            groupInteractable   = group->interactable;
            groupBlocksRaycasts = group->blocksRaycasts;
        } else {
            groupInteractable   = groupInteractable   && group->interactable;
            groupBlocksRaycasts = groupBlocksRaycasts && group->blocksRaycasts;
        }
    }
    /// @note 触れない群は配下ごと入力の対象から外す。走査自体は続ける ─ ドラッグ中の
    /// @note 位置追従など、入力を受けない要素にも毎フレーム更新したい状態がある。
    if (!groupInteractable) inputAvailable = false;

    const UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform, parentTransform.parentSize)
        : parentTransform;

    const Rect selfRect = RectFromTransform(go, resolved);
    const bool insideSelf = mouseInCanvasSpace.x >= selfRect.pos.x
        && mouseInCanvasSpace.x <= selfRect.pos.x + selfRect.size.x
        && mouseInCanvasSpace.y >= selfRect.pos.y
        && mouseInCanvasSpace.y <= selfRect.pos.y + selfRect.size.y;
    bool childrenInputAvailable = inputAvailable;
    /// @note Mask も Scroll View も、枠の外に出た中身は触れない。描画のクリップと同じ条件に
    /// @note しないと、スクロールした先の項目を「見えないまま」踏むことになる。
    if (ElementClipsChildren(go) && !insideSelf)
        childrenInputAvailable = false;
    UITransform2D childTransform = resolved;
    /// @note 子のアンカーは「この要素の矩形」に対する割合。Canvas 自身は矩形を持たないので、
    /// @note 呼び出し元が入れた寸法をそのまま渡す。上書きすると transform.scale (既定 1,1) が
    /// @note 親サイズになり、直下の子のアンカーが常に潰れる。
    if (applySelfTransform)
        childTransform.parentSize = selfRect.size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= RotateUIVector(scroll->scrollPosition, resolved.rotationZ);

    /// @note 描画順の逆から入力を解決し、重なった UI では最前面の要素だけがポインターを受け取る。
    bool consumed = false;
    std::vector<GameObject*>& children = AcquireChildScratch(ctx, depth);
    SortUIChildren(go, children);
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        consumed |= ProcessUIEventsRecursive(**it, ctx, childTransform, mouseInCanvasSpace,
                                             mousePressed, childrenInputAvailable && !consumed,
                                             true, depth + 1,
                                             groupInteractable, groupBlocksRaycasts);
    }

    auto* button = go.GetComponent<UIButton>();
    if (button) {
        /// @note UIImage の有無に関わらず、解決した矩形に面積があればヒット判定する
        /// @note (UIText や子要素だけで構成されるボタンのため)。
        /// @note 判定に使う矩形は selfRect。scale で決めると「見えているのに押せない」が出る。
        if (selfRect.size.x > 0.0f && selfRect.size.y > 0.0f) {
            const math::Vector2 effectiveMouse = inputAvailable && !consumed
                ? mouseInCanvasSpace
                : math::Vector2{ -1.0e30f, -1.0e30f };
            consumed |= UpdateButton(*button, selfRect, effectiveMouse, mousePressed);
        }
        else {
            button->onClick          = false;
            button->onEnter          = false;
            button->onExit           = false;
            button->state            = UIButtonState::NORMAL;
            button->wasPressedOnThis = false;
            button->lastMouseState   = mousePressed;
        }
    }

    const Rect rect = RectFromTransform(go, resolved);
    const bool hit = mouseInCanvasSpace.x >= rect.pos.x
        && mouseInCanvasSpace.x <= rect.pos.x + rect.size.x
        && mouseInCanvasSpace.y >= rect.pos.y
        && mouseInCanvasSpace.y <= rect.pos.y + rect.size.y;
    const bool canReceive = inputAvailable && !consumed;

    if (auto* slider = go.GetComponent<UISlider>()) {
        slider->onValueChanged = false;
        const bool justPressed = mousePressed && !slider->runtimeLastMouse;
        if (slider->enabled && slider->interactable && canReceive && justPressed && hit)
            slider->runtimeDragging = true;
        if (!mousePressed)
            slider->runtimeDragging = false;
        if (slider->runtimeDragging) {
            const float axisSize = slider->vertical ? rect.size.y : rect.size.x;
            const float axisPosition = slider->vertical
                ? mouseInCanvasSpace.y - rect.pos.y
                : mouseInCanvasSpace.x - rect.pos.x;
            float normalized = axisSize > 0.0f
                ? std::clamp(axisPosition / axisSize, 0.0f, 1.0f) : 0.0f;
            if (slider->vertical)
                normalized = 1.0f - normalized;
            float next = slider->minimum + (slider->maximum - slider->minimum) * normalized;
            if (slider->wholeNumbers)
                next = std::round(next);
            next = std::clamp(next, (std::min)(slider->minimum, slider->maximum),
                              (std::max)(slider->minimum, slider->maximum));
            slider->onValueChanged = next != slider->value;
            slider->value = next;
            if (auto* sliderImage = go.GetComponent<UIImage>()) {
                const float range = slider->maximum - slider->minimum;
                sliderImage->fillAmount = std::abs(range) > 0.000001f
                    ? std::clamp((slider->value - slider->minimum) / range, 0.0f, 1.0f)
                    : 0.0f;
                sliderImage->fillOrigin = slider->vertical
                    ? UIImageFillOrigin::Bottom : UIImageFillOrigin::Left;
            }
            consumed = true;
        }
        slider->runtimeLastMouse = mousePressed;
    }

    if (auto* toggle = go.GetComponent<UIToggle>()) {
        toggle->onValueChanged = false;
        const bool justPressed = mousePressed && !toggle->runtimeLastMouse;
        const bool justReleased = !mousePressed && toggle->runtimeLastMouse;
        if (toggle->enabled && toggle->interactable && canReceive && justPressed && hit)
            toggle->runtimePressedHere = true;
        if (justReleased) {
            if (toggle->runtimePressedHere && hit) {
                toggle->isOn = !toggle->isOn;
                toggle->onValueChanged = true;
                consumed = true;
            }
            toggle->runtimePressedHere = false;
        }
        toggle->runtimeLastMouse = mousePressed;
    }

    if (auto* scroll = go.GetComponent<UIScrollView>()) {
        scroll->onValueChanged = false;
        const float wheel = ctx.gameplayInput ? input::input_detail::GameInputAccess::MouseScrollDelta()
            : input::Input::MouseScrollDelta();
        if (scroll->enabled && canReceive && hit && wheel != 0.0f) {
            if (scroll->vertical)
                scroll->scrollPosition.y -= wheel * scroll->sensitivity;
            if (scroll->horizontal)
                scroll->scrollPosition.x -= wheel * scroll->sensitivity;
            scroll->scrollPosition.x = std::clamp(scroll->scrollPosition.x, 0.0f,
                (std::max)(0.0f, scroll->contentSize.x - rect.size.x));
            scroll->scrollPosition.y = std::clamp(scroll->scrollPosition.y, 0.0f,
                (std::max)(0.0f, scroll->contentSize.y - rect.size.y));
            scroll->onValueChanged = true;
            consumed = true;
        }
    }

    if (auto* field = go.GetComponent<UIInputField>()) {
        field->onValueChanged = false;
        field->onSubmit = false;
        if (mousePressed && canReceive)
            field->focused = hit;
        if (field->enabled && field->interactable && field->focused) {
            const auto text = ctx.gameplayInput ? input::input_detail::GameInputAccess::TextInput() : input::Input::TextInput();
            for (const char character : text) {
                if (character == '\b') {
                    if (!field->text.empty()) {
                        size_t characterStart = field->text.size() - 1;
                        while (characterStart > 0
                               && (static_cast<unsigned char>(field->text[characterStart]) & 0xC0u) == 0x80u)
                            --characterStart;
                        field->text.erase(characterStart);
                        field->onValueChanged = true;
                    }
                } else if (character == '\r' || character == '\n') {
                    if (field->multiline) {
                        field->text.push_back('\n');
                        field->onValueChanged = true;
                    } else {
                        field->onSubmit = true;
                        field->focused = false;
                    }
                } else if ((field->contentType == UIInputContentType::Standard
                            || field->contentType == UIInputContentType::Password
                            || (field->contentType == UIInputContentType::Integer
                                && (character >= '0' && character <= '9'
                                    || character == '-' && field->text.empty()))
                            || (field->contentType == UIInputContentType::Decimal
                                && (character >= '0' && character <= '9'
                                    || character == '-' && field->text.empty()
                                    || character == '.' && field->text.find('.') == std::string::npos)))
                           && (field->characterLimit <= 0
                               || static_cast<int>(field->text.size()) < field->characterLimit)) {
                    field->text.push_back(character);
                    field->onValueChanged = true;
                }
            }
            field->caretPosition = field->text.size();
            consumed |= hit;
        }
        if (auto* inputText = go.GetComponent<UIText>()) {
            if (field->text.empty()) {
                inputText->text = field->placeholder;
            } else if (field->contentType == UIInputContentType::Password) {
                inputText->text.assign(field->text.size(), '*');
            } else {
                inputText->text = field->text;
            }
        }
    }

    if (auto* trigger = go.GetComponent<UIEventTrigger>()) {
        trigger->pointerEnter = trigger->pointerExit = false;
        trigger->pointerDown = trigger->pointerUp = trigger->pointerClick = false;
        trigger->beginDrag = trigger->drag = trigger->endDrag = false;
        trigger->pointerPosition = mouseInCanvasSpace;
        trigger->dragDelta = mouseInCanvasSpace - trigger->runtimeLastPointer;
        const bool currentHit = trigger->enabled && canReceive && hit;
        trigger->pointerEnter = currentHit && !trigger->runtimeHovered;
        trigger->pointerExit = !currentHit && trigger->runtimeHovered;
        trigger->pointerDown = currentHit && mousePressed && !trigger->runtimeLastMouse;
        trigger->pointerUp = !mousePressed && trigger->runtimeLastMouse
            && trigger->runtimePressedHere;
        if (trigger->pointerDown) {
            trigger->runtimePressedHere = true;
            trigger->beginDrag = true;
        }
        trigger->drag = trigger->runtimePressedHere && mousePressed
            && trigger->dragDelta != math::Vector2::ZERO;
        if (trigger->pointerUp) {
            trigger->pointerClick = currentHit;
            trigger->endDrag = true;
            trigger->runtimePressedHere = false;
        }
        trigger->runtimeHovered = currentHit;
        trigger->runtimeLastMouse = mousePressed;
        trigger->runtimeLastPointer = mouseInCanvasSpace;
        consumed |= trigger->pointerDown || trigger->drag;
    }

    /// @name 落とし先
    /// @note 走査は最前面から降りてくるので、最初に当たった受け皿がいちばん手前。
    if (auto* target = go.GetComponent<UIDropTarget>()) {
        target->received = false;
        target->hovered  = false;
        if (target->enabled && hit && ctx.dragFrame.hoveredTarget == nullptr
            && &go != ctx.dragFrame.activeSource) {
            ctx.dragFrame.hoveredTarget = &go;
        }
    }

    /// @name 掴んで運ぶ
    if (auto* drag = go.GetComponent<UIDragSource>()) {
        drag->dropped = false;
        drag->droppedOn = {};
        const bool justPressed  = mousePressed && !drag->runtimeLastMouse;
        const bool justReleased = !mousePressed && drag->runtimeLastMouse;

        if (drag->enabled && canReceive && justPressed && hit) {
            drag->dragging = true;
            drag->originPosition = { go.transform.position.x, go.transform.position.y };
            /// @note 掴んだ点と要素の左上のずれ。これを保たないと、掴んだ瞬間に
            /// @note 要素がポインターへ吸い付いて飛ぶ。
            drag->grabOffset = { mouseInCanvasSpace.x - rect.pos.x,
                                 mouseInCanvasSpace.y - rect.pos.y };
            consumed = true;
        }

        if (drag->dragging) {
            ctx.dragFrame.activeSource = &go;
            if (drag->moveWithPointer) {
                /// @note rect は親の矩形とアンカーを解いた結果なので、その差分だけ動かす。
                /// @note ローカル位置へ直接代入すると、アンカーを付けた要素が飛ぶ。
                const math::Vector2 desired = { mouseInCanvasSpace.x - drag->grabOffset.x,
                                                mouseInCanvasSpace.y - drag->grabOffset.y };
                go.transform.position.x += desired.x - rect.pos.x;
                go.transform.position.y += desired.y - rect.pos.y;
            }
            consumed = true;
            if (justReleased) {
                drag->dragging = false;
                /// @note 落とし先の確定は階層を見終わってから (受け皿がまだ来ていない可能性)。
                ctx.dragFrame.releasedSource = &go;
            }
        }
        drag->runtimeLastMouse = mousePressed;
    }

    /// @note 素通しの群は「当たったが吸わない」。背後の要素へ判定を渡す。
    if (!groupBlocksRaycasts) return false;
    return consumed;
}

bool HitUIElement(GameObject& go, const UITransform2D& parent, math::Vector2 pointer,
    bool applyTransform = true, bool interactable = true, bool blocks = true)
{
    if (!go.activeInHierarchy()) return false;
    if (const auto* group = go.GetComponent<UICanvasGroup>(); group && group->enabled) {
        interactable = group->interactable && (group->ignoreParentGroups || interactable);
        blocks = group->blocksRaycasts && (group->ignoreParentGroups || blocks);
    }
    const auto resolved = applyTransform
        ? ComposeUITransform(parent, go.transform, parent.parentSize) : parent;
    const Rect rect = RectFromTransform(go, resolved);
    const bool hit = rect.size.x > 0.0f && rect.size.y > 0.0f
        && pointer.x >= rect.pos.x && pointer.x <= rect.pos.x + rect.size.x
        && pointer.y >= rect.pos.y && pointer.y <= rect.pos.y + rect.size.y;
    auto childTransform = resolved;
    if (applyTransform) childTransform.parentSize = rect.size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= RotateUIVector(scroll->scrollPosition, resolved.rotationZ);
    if (!ElementClipsChildren(go) || hit) {
        for (int index = 0; index < go.GetChildCount(); ++index)
            if (auto* child = go.GetChild(index);
                child && HitUIElement(*child, childTransform, pointer, true, interactable, blocks)) return true;
    }
    if (!hit || !interactable || !blocks) return false;
    if (const auto* control = go.GetComponent<UIButton>(); control && control->enabled && control->isInteractable) return true;
    if (const auto* control = go.GetComponent<UISlider>(); control && control->enabled && control->interactable) return true;
    if (const auto* control = go.GetComponent<UIToggle>(); control && control->enabled && control->interactable) return true;
    if (const auto* control = go.GetComponent<UIInputField>(); control && control->enabled && control->interactable) return true;
    if (const auto* control = go.GetComponent<UIScrollView>(); control && control->enabled) return true;
    if (const auto* control = go.GetComponent<UIEventTrigger>(); control && control->enabled) return true;
    if (const auto* control = go.GetComponent<UIDragSource>(); control && control->enabled) return true;
    if (const auto* control = go.GetComponent<UIDropTarget>(); control && control->enabled) return true;
    return false;
}

bool ResolveCanvasPhysicalPointer(GameObject& go, const UICanvas& canvas, const UIInputView& view,
    math::Vector2& pointer)
{
    const float viewportWidth = (std::max)(1.0f, view.width);
    const float viewportHeight = (std::max)(1.0f, view.height);
    if (IsScreenSpaceRenderMode(canvas.renderMode)) {
        float width = 1.0f, height = 1.0f;
        ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight, width, height);
        pointer = {view.pointer.x * width / viewportWidth, view.pointer.y * height / viewportHeight};
        return true;
    }
    const auto ray = math::Ray::FromNDC(view.pointer.x / viewportWidth * 2.0f - 1.0f,
        1.0f - view.pointer.y / viewportHeight * 2.0f, view.cameraPosition,
        math::Matrix4::Inverse(view.viewProjection));
    const auto rotation = canvas.faceCamera ? view.cameraRotation : go.transform.worldRotation;
    const auto position = go.transform.worldPosition - rotation * math::Vector3::FORWARD * canvas.surfaceOffset;
    const auto world = math::Matrix4::TRS(position, rotation, math::Vector3::ONE);
    const auto plane = math::Plane::FromNormalAndPoint({world.m[0][2], world.m[1][2], world.m[2][2]}, position);
    float distance = 0.0f;
    if (canvas.worldScale <= 0.0f || !ray.IntersectPlane(plane, distance) || distance < 0.0f) return false;
    const auto hit = ray.At(distance);
    const auto local = MulMV(math::Matrix4::Inverse(world), {hit.x, hit.y, hit.z, 1.0f});
    pointer = {local.x / canvas.worldScale + canvas.canvasWidth * 0.5f,
        -local.y / canvas.worldScale + canvas.canvasHeight * 0.5f};
    return true;
}

/// @note ドラッグの後始末。走査が終わって初めて「どこへ落ちたか」が確定する。
void ResolveUIDrop(UIInteractionContext& ctx)
{
    GameObject* source = ctx.dragFrame.releasedSource;
    if (!source) return;
    auto* drag = source->GetComponent<UIDragSource>();
    if (!drag) return;

    GameObject* target = ctx.dragFrame.hoveredTarget;
    UIDropTarget* drop = target ? target->GetComponent<UIDropTarget>() : nullptr;
    /// @note 種類が合わない受け皿は無かったことにする。accepts が空なら何でも受ける。
    if (drop && !drop->accepts.empty() && drop->accepts != drag->payload) {
        drop   = nullptr;
        target = nullptr;
    }

    drag->dropped = true;
    if (drop) {
        drag->droppedOn        = { target->GetID() };
        drop->received         = true;
        drop->receivedFrom     = { source->GetID() };
        drop->receivedPayload  = drag->payload;
    } else if (drag->returnOnDrop) {
        source->transform.position.x = drag->originPosition.x;
        source->transform.position.y = drag->originPosition.y;
    }

}
/// @name サブシステム
void UILayoutSystem(const std::vector<CanvasEntry>& canvases, UIInteractionContext& ctx,
                    float viewportWidth, float viewportHeight)
{
    for (const CanvasEntry& entry : canvases) {
        const float scale = ResolveCanvasScale(*entry.canvas, viewportWidth, viewportHeight);
        /// @note 並べる基準はセーフエリアの内側。Canvas 直付けの Layout Group が
        /// @note 余白を無視して端まで詰めると、避けたはずの領域へ戻ってしまう。
        ApplyUILayoutRecursive(
            *entry.go,
            BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight).parentSize,
            scale, ctx, entry.theme ? &*entry.theme : nullptr);
    }
}

/// @name フォーカス移動
/// @note 移動先の候補 1 件。矩形は Canvas 空間。
using UINavCandidate = UIInteractionContext::NavigationCandidate;

/// @note Canvas 配下から、フォーカスを置ける要素を矩形つきで集める。
/// @note 矩形は入力処理と同じ式で出す。別に計算すると、アンカーやレイアウトが絡んだ画面で
/// @note だけ「隣に見えるのに飛ばない」が出る。
void CollectNavCandidates(GameObject& go, const UITransform2D& parentTransform,
                          bool applySelfTransform, bool groupInteractable,
                          std::vector<UINavCandidate>& out)
{
    if (!go.activeInHierarchy()) return;

    if (const auto* group = go.GetComponent<UICanvasGroup>(); group && group->enabled) {
        groupInteractable = group->ignoreParentGroups
            ? group->interactable
            : (groupInteractable && group->interactable);
    }

    const UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform, parentTransform.parentSize)
        : parentTransform;
    const Rect selfRect = RectFromTransform(go, resolved);

    if (groupInteractable) {
        if (auto* nav = go.GetComponent<UINavigation>();
            nav && nav->enabled && selfRect.size.x > 0.0f && selfRect.size.y > 0.0f) {
            out.push_back({ &go, nav, selfRect.pos, selfRect.size });
        }
    }

    UITransform2D childTransform = resolved;
    if (applySelfTransform) childTransform.parentSize = selfRect.size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= RotateUIVector(scroll->scrollPosition, resolved.rotationZ);

    for (int i = 0; i < go.GetChildCount(); ++i)
        if (GameObject* child = go.GetChild(i))
            CollectNavCandidates(*child, childTransform, true, groupInteractable, out);
}

/// @note 方向 dir (単位ベクトル) にいちばん近い候補。無ければ nullptr。
/// @note 距離だけで選ぶと斜め後ろの近い項目が勝ち、下キーで上へ戻ることが起きる。
/// @note 進行方向の成分が横ずれを上回るものだけを候補にする。
const UINavCandidate* FindNearestInDirection(const std::vector<UINavCandidate>& candidates,
                                             const UINavCandidate& from, const math::Vector2& dir,
                                             const GameObject* exclude)
{
    const math::Vector2 origin = { from.rectPosition.x + from.rectSize.x * 0.5f,
                                   from.rectPosition.y + from.rectSize.y * 0.5f };
    const UINavCandidate* best = nullptr;
    float bestScore = 0.0f;

    for (const UINavCandidate& candidate : candidates) {
        if (candidate.go == exclude) continue;
        const math::Vector2 center = { candidate.rectPosition.x + candidate.rectSize.x * 0.5f,
                                       candidate.rectPosition.y + candidate.rectSize.y * 0.5f };
        const math::Vector2 delta = { center.x - origin.x, center.y - origin.y };
        const float along  = delta.x * dir.x + delta.y * dir.y;
        /// @note 進みたい向きの逆
        if (along <= 0.0f) continue;
        const float lateral = std::fabs(delta.x * dir.y - delta.y * dir.x);
        /// @note 45 度より外は「その方向」ではない
        if (lateral > along) continue;

        /// @note 横ずれを重く見る。同じ距離なら真っ直ぐ並んでいるほうを選ぶ。
        const float score = along + lateral * 2.0f;
        if (!best || score < bestScore) {
            best = &candidate;
            bestScore = score;
        }
    }
    return best;
}

/// @note いま倒されている方向。倒していなければゼロ。
math::Vector2 ReadNavigationDirection(bool gameplayInput)
{
    const auto keyHeld = [gameplayInput](input::KeyCode key) {
        return gameplayInput ? input::input_detail::GameInputAccess::KeyHeld(key) : input::Input::KeyHeld(key);
    };
    const auto padButton = [gameplayInput](input::GamepadButton button, int pad) {
        return gameplayInput ? input::input_detail::GameInputAccess::PadButtonHeld(button, pad) : input::Gamepad::ButtonHeld(button, pad);
    };
    const auto padAxis = [gameplayInput](input::GamepadAxis axis, int pad) {
        return gameplayInput ? input::input_detail::GameInputAccess::PadAxis(axis, pad) : input::Gamepad::Axis(axis, pad);
    };
    math::Vector2 dir{};
    if (keyHeld(input::KeyCode::LEFT))  dir.x -= 1.0f;
    if (keyHeld(input::KeyCode::RIGHT)) dir.x += 1.0f;
    if (keyHeld(input::KeyCode::UP))    dir.y -= 1.0f;
    if (keyHeld(input::KeyCode::DOWN))  dir.y += 1.0f;

    const int pad = input::Gamepad::GetFirstConnectedPad();
    if (pad >= 0) {
        if (padButton(input::GamepadButton::DPAD_LEFT,  pad)) dir.x -= 1.0f;
        if (padButton(input::GamepadButton::DPAD_RIGHT, pad)) dir.x += 1.0f;
        if (padButton(input::GamepadButton::DPAD_UP,    pad)) dir.y -= 1.0f;
        if (padButton(input::GamepadButton::DPAD_DOWN,  pad)) dir.y += 1.0f;
        /// @note スティックは倒し込みで初めて 1 方向として扱う。
        /// @note 生値は静止時も 0 にならないので、デッドゾーンが無いとフォーカスが流れ続ける。
        constexpr float kStickThreshold = 0.6f;
        const float sx = padAxis(input::GamepadAxis::LEFT_STICK_X, pad);
        const float sy = padAxis(input::GamepadAxis::LEFT_STICK_Y, pad);
        if (sx < -kStickThreshold) dir.x -= 1.0f;
        if (sx >  kStickThreshold) dir.x += 1.0f;
        /// @note スティックの +Y は上、Canvas の +Y は下
        if (sy >  kStickThreshold) dir.y -= 1.0f;
        if (sy < -kStickThreshold) dir.y += 1.0f;
    }

    /// @note 斜めは扱わない。UI は縦横の格子で並んでいるので、斜めを許すと
    /// @note 「どちらへ行きたかったのか」が入力から決まらない。
    if (std::fabs(dir.x) > std::fabs(dir.y)) return { dir.x > 0.0f ? 1.0f : -1.0f, 0.0f };
    if (std::fabs(dir.y) > 0.0f)             return { 0.0f, dir.y > 0.0f ? 1.0f : -1.0f };
    return {};
}

bool ReadNavigationSubmit(bool gameplayInput)
{
    if (gameplayInput) {
        if (input::input_detail::GameInputAccess::KeyHeld(input::KeyCode::ENTER)
            || input::input_detail::GameInputAccess::KeyHeld(input::KeyCode::SPACE)) return true;
        const int gamePad = input::Gamepad::GetFirstConnectedPad();
        return gamePad >= 0 && input::input_detail::GameInputAccess::PadButtonHeld(input::GamepadButton::A, gamePad);
    }
    if (input::Input::KeyHeld(input::KeyCode::ENTER)) return true;
    if (input::Input::KeyHeld(input::KeyCode::SPACE)) return true;
    const int pad = input::Gamepad::GetFirstConnectedPad();
    return pad >= 0 && input::Gamepad::ButtonHeld(input::GamepadButton::A, pad);
}

/// @note フォーカスを更新し、決まった位置へポインターを移す。
/// @note 動かしたら true (呼び出し側はこのフレームのポインターをフォーカスへ寄せる)。
bool UINavigationSystem(Scene& scene, UICanvas& canvas, GameObject& canvasGO,
                        const UITransform2D& canvasRoot,
                        std::vector<UINavCandidate>& scratch, UIInteractionContext& ctx, bool gameplayInput,
                        math::Vector2& pointerPosition, bool& pointerPressed)
{
    if (gameplayInput && !core::Cursor::CanReadGameInput()) {
        bool injectedNavigation = false;
        for (const auto key : {input::KeyCode::LEFT, input::KeyCode::RIGHT, input::KeyCode::UP,
                              input::KeyCode::DOWN, input::KeyCode::ENTER, input::KeyCode::SPACE})
            injectedNavigation |= input::input_detail::GameInputAccess::InjectedKeyPending(key);
        if (!injectedNavigation) return false;
    }
    scratch.clear();
    CollectNavCandidates(canvasGO, canvasRoot, false, true, scratch);
    for (UINavCandidate& candidate : scratch) candidate.nav->focused = false;
    if (scratch.empty()) return false;

    /// @note いまのフォーカス。消えていたら選び直す。
    const UINavCandidate* current = nullptr;
    for (const UINavCandidate& candidate : scratch)
        if (candidate.go->GetID() == canvas.focusedObject.id) { current = &candidate; break; }

    if (!current) {
        for (const UINavCandidate& candidate : scratch)
            if (candidate.nav->selectOnStart) { current = &candidate; break; }
        if (!current) current = &scratch.front();
        canvas.focusedObject = { current->go->GetID() };
    }

    /// @note 方向入力の繰り返し。倒し始めは即座に、以降は間隔をあけて 1 段ずつ。
    const math::Vector2 dir = ReadNavigationDirection(gameplayInput);
    bool step = false;
    if (dir.x == 0.0f && dir.y == 0.0f) {
        ctx.navHeldDirection = {};
        ctx.navRepeatTimer   = 0.0f;
    } else if (dir.x != ctx.navHeldDirection.x || dir.y != ctx.navHeldDirection.y) {
        ctx.navHeldDirection = dir;
        ctx.navRepeatTimer   = canvas.navigationRepeatDelay;
        step = true;
    } else {
        /// @note 未スケールの時間を使う。ポーズ中のメニューは timeScale = 0 で動くため、
        /// @note スケール済みの刻みだと方向キーの連続入力が止まる。
        ctx.navRepeatTimer -= fbzz::Time::unscaledDeltaTime;
        if (ctx.navRepeatTimer <= 0.0f) {
            ctx.navRepeatTimer = canvas.navigationRepeatInterval;
            step = true;
        }
    }

    if (step) {
        const UINavigation& nav = *current->nav;
        GameObject* explicitNext = nullptr;
        if (nav.mode == UINavigationMode::Explicit) {
            const EntityRef& link = dir.x < 0.0f ? nav.left
                                  : dir.x > 0.0f ? nav.right
                                  : dir.y < 0.0f ? nav.up
                                                 : nav.down;
            explicitNext = link.Resolve(scene);
        }

        if (nav.mode != UINavigationMode::None) {
            const UINavCandidate* next = nullptr;
            if (explicitNext) {
                for (const UINavCandidate& candidate : scratch)
                    if (candidate.go == explicitNext) { next = &candidate; break; }
            } else if (nav.mode == UINavigationMode::Automatic) {
                next = FindNearestInDirection(scratch, *current, dir, current->go);
            }
            if (next) {
                current = next;
                canvas.focusedObject = { next->go->GetID() };
            }
        }
    }

    current->nav->focused = true;

    /// @note ポインターをフォーカスの中心へ置く。ここから先は既存のウィジェットが
    /// @note マウスと同じように反応する (理由は UIPointer.hpp を参照)。
    /// @note Built-in navigation is local to this pass; only script-authored UIPointer retains virtual ownership.
    pointerPosition = { current->rectPosition.x + current->rectSize.x * 0.5f,
                        current->rectPosition.y + current->rectSize.y * 0.5f };
    pointerPressed = ReadNavigationSubmit(gameplayInput);
    return true;
}

void CancelUIInteractionsRecursive(GameObject& go)
{
    if (auto* control = go.GetComponent<UIButton>()) ui_detail::CancelInteraction(*control);
    if (auto* control = go.GetComponent<UISlider>()) ui_detail::CancelInteraction(*control);
    if (auto* control = go.GetComponent<UIToggle>()) ui_detail::CancelInteraction(*control);
    if (auto* control = go.GetComponent<UIScrollView>()) ui_detail::CancelInteraction(*control);
    if (auto* control = go.GetComponent<UIInputField>()) ui_detail::CancelInteraction(*control);
    if (auto* control = go.GetComponent<UINavigation>()) ui_detail::CancelInteraction(*control);
    if (auto* control = go.GetComponent<UIEventTrigger>()) ui_detail::CancelInteraction(*control);
    if (auto* control = go.GetComponent<UIDragSource>()) ui_detail::CancelInteraction(*control);
    if (auto* control = go.GetComponent<UIDropTarget>()) ui_detail::CancelInteraction(*control);
    for (int child = 0; child < go.GetChildCount(); ++child)
        if (auto* next = go.GetChild(child)) CancelUIInteractionsRecursive(*next);
}

void UIEventSystem(Scene& scene,
                   const std::vector<CanvasEntry>& canvases,
                   UIInteractionContext& ctx,
                   float viewportWidth,
                   float viewportHeight,
                   math::Vector2 rawMouseInViewport,
                   bool mousePressed,
                   const math::Matrix4& viewProjection,
                   math::Vector3 cameraWorldPos,
                   math::Quaternion cameraWorldRot,
                   UIRenderTargetView targetView)
{
    /// @note ドラッグの持ち越しはフレームごとに作り直す。
    /// @note 前フレームの受け皿が残っていると、離した瞬間に古い相手へ落ちる。
    ctx.dragFrame = {};
    if (ctx.gameplayInput) {
        const auto revision = input::input_detail::GameInputAccess::InteractionRevision();
        if (ctx.inputInteractionRevision != revision) {
            for (const auto& entry : canvases) CancelUIInteractionsRecursive(*entry.go);
            ctx.inputInteractionRevision = revision;
            ctx.navHeldDirection = {};
            ctx.navRepeatTimer = 0.0f;
        }
        rawMouseInViewport = ui_detail::ResolvePointerPosition(rawMouseInViewport,
            input::input_detail::GameInputAccess::PointerAvailable());
        mousePressed = input::input_detail::GameInputAccess::PointerPressed(mousePressed);
    }
    bool navigationPointerActive = false;
    math::Vector2 navigationPointerPosition{};
    bool navigationPointerPressed = false;

    /// @note ナビゲーションはポインターを差し替えるので、当たり判定より先に解く。
    /// @note 既に差し替えが宣言されていれば手を出さない (ゲーム内カーソルを持つ画面が優先)。
    if (!UIPointer::IsActive()) {
        auto& navScratch = ctx.navigationScratch;
        for (const CanvasEntry& entry : canvases) {
            if (!entry.canvas->navigationEnabled) continue;
            if (!ShouldRenderCanvas(*entry.canvas, targetView, ctx.sceneDepthAvailable)) continue;
            const UITransform2D canvasRoot =
                BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
            if (UINavigationSystem(scene, *entry.canvas, *entry.go, canvasRoot, navScratch,
                                   ctx, ctx.gameplayInput, navigationPointerPosition, navigationPointerPressed))
                /// @note フォーカスは画面に 1 つ。最初に受け持った Canvas が持つ。
                { navigationPointerActive = true; break; }
        }
    }

    /// @note Canvas 間でも描画順の逆から入力を解決し、最前面で消費された入力を背面へ渡さない。
    if (ctx.gameplayInput && !input::input_detail::GameInputAccess::PointerAvailable()
        && !UIPointer::IsActive() && !navigationPointerActive) {
        for (const auto& entry : canvases) CancelUIInteractionsRecursive(*entry.go);
        return;
    }
    bool inputConsumed = false;
    for (auto canvasIt = canvases.rbegin(); canvasIt != canvases.rend(); ++canvasIt) {
        const CanvasEntry& entry = *canvasIt;
        if (!ShouldRenderCanvas(*entry.canvas, targetView, ctx.sceneDepthAvailable)) {
            CancelUIInteractionsRecursive(*entry.go);
            continue;
        }

        /// @name WorldSpace ヒット判定: スクリーン座標→ワールドレイ→キャンバスピクセル
        if (!IsScreenSpaceRenderMode(entry.canvas->renderMode)) {
            if (ctx.gameplayInput && !input::input_detail::GameInputAccess::PointerAvailable()
                && (UIPointer::IsActive() || navigationPointerActive)) {
                const UITransform2D canvasRoot = BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
                const auto pointer = UIPointer::IsActive() ? UIPointer::Position() : navigationPointerPosition;
                const bool pressed = UIPointer::IsActive() ? UIPointer::Pressed() : navigationPointerPressed;
                entry.canvas->resolvedMousePosition = {-1.0e30f, -1.0e30f};
                inputConsumed |= ProcessUIEventsRecursive(*entry.go, ctx, canvasRoot, pointer,
                    pressed, !inputConsumed, false);
                continue;
            }
            const UIInputView inputView{viewportWidth, viewportHeight, rawMouseInViewport,
                viewProjection, cameraWorldPos, cameraWorldRot};
            math::Vector2 canvasPx;
            if (!ResolveCanvasPhysicalPointer(*entry.go, *entry.canvas, inputView, canvasPx)) continue;

            /// @note Canvas 直下の子から見た「親」は Canvas そのもの。
            /// @note ここを 0 のままにするとアンカーが常に左上へ潰れる。
            const UITransform2D canvasRoot =
                BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
            const math::Vector2 pointerPx =
                UIPointer::IsActive() ? UIPointer::Position() : navigationPointerActive ? navigationPointerPosition : canvasPx;
            const bool pointerPressed =
                UIPointer::IsActive() ? UIPointer::Pressed() : navigationPointerActive ? navigationPointerPressed : mousePressed;
            entry.canvas->resolvedMousePosition = canvasPx;
            inputConsumed |= ProcessUIEventsRecursive(*entry.go, ctx, canvasRoot, pointerPx,
                                                      pointerPressed, !inputConsumed, false);
            continue;
        }

        /// @name ScreenSpace (Overlay / ScreenSpaceCamera)
        /// @note BuildCanvasRuntimeState を呼ばず直接計算する (cameraWorldRot 不要)
        const UIInputView inputView{viewportWidth, viewportHeight, rawMouseInViewport,
            viewProjection, cameraWorldPos, cameraWorldRot};
        math::Vector2 mouseInCanvas;
        if (!ResolveCanvasPhysicalPointer(*entry.go, *entry.canvas, inputView, mouseInCanvas)) continue;

        const UITransform2D canvasRoot =
            BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
        /// @note ゲーム内カーソルがあればそちらを唯一のポインターとして使う。
        /// @note 判定は「Canvas 空間の座標 1 つと押下状態」だけで決まるので、入口を 1 つに絞れば
        /// @note マウスとパッドの両対応をウィジェット側に書かずに済む (UIPointer.hpp)。
        const math::Vector2 pointer =
            UIPointer::IsActive() ? UIPointer::Position() : navigationPointerActive ? navigationPointerPosition : mouseInCanvas;
        const bool pointerPressed =
            UIPointer::IsActive() ? UIPointer::Pressed() : navigationPointerActive ? navigationPointerPressed : mousePressed;

        /// @note ここへ残すのは常に「OS のマウスを Canvas 空間へ直した値」。差し替え後の値を
        /// @note 返すと、カーソルを動かす側が自分の出力を読み直してマウスで動かせなくなる。
        /// @note 式は renderMode / Canvas Scaler / viewport 寸法で決まるので、スクリプト側で
        /// @note 書き直させると viewport ≠ ウィンドウの場面でだけ静かにずれる。
        entry.canvas->resolvedMousePosition = mouseInCanvas;
        inputConsumed |= ProcessUIEventsRecursive(*entry.go, ctx, canvasRoot, pointer,
                                                  pointerPressed, !inputConsumed, false);
    }

    /// @note 落とし先が決まるのは全 Canvas を見終わったあと。
    ResolveUIDrop(ctx);

}

}
math::Vector2 GetCanvasRectSize(const UICanvas& canvas, float viewportWidth, float viewportHeight)
{
    return ResolveCanvasRectSize(canvas, viewportWidth, viewportHeight);
}
bool UIPointerOverUI(Scene& scene, const UIInputView& view)
{
    if (!std::isfinite(view.width) || !std::isfinite(view.height) || view.width <= 0.0f || view.height <= 0.0f)
        return false;
    const bool overridden = UIPointer::IsActive();
    if (!overridden && (view.gameplayInput && !input::input_detail::GameInputAccess::PointerAvailable()
        || !std::isfinite(view.pointer.x) || !std::isfinite(view.pointer.y)
        || view.pointer.x < 0.0f || view.pointer.y < 0.0f
        || view.pointer.x > view.width || view.pointer.y > view.height)) return false;
    for (const auto id : ui_detail::CollectCanvasEntities(scene)) {
        auto* canvas = scene.GetComponent<UICanvas>(id);
        auto* object = scene.GetGameObject(id);
        if (!canvas || !object || !ShouldRenderCanvas(*canvas, UIRenderTargetView::GameViewport, view.sceneDepthAvailable)) continue;
        math::Vector2 pointer;
        if (overridden) pointer = UIPointer::Position();
        else if (!ResolveCanvasPhysicalPointer(*object, *canvas, view, pointer)) continue;
        if (HitUIElement(*object, BuildCanvasRootTransform(*canvas, view.width, view.height), pointer, false)) return true;
    }
    return false;
}

std::size_t UITextVisibleLength(const std::string& text, bool richText)
{
    UIRichTextState style{};
    std::size_t count = 0;
    std::size_t offset = 0;
    std::size_t begin  = 0;
    /// @note fontSize は `<size>` の解釈にしか使わない。文字数には影響しないので 1 でよい。
    while (NextRichGlyph(text, offset, richText, 1.0f, style, begin) != 0)
        ++count;
    return count;
}

void UpdateUILayout(Scene& scene, UIInteractionContext& ctx, float width, float height)
{
    const int generation = asset::AssetManager::GetAssetGeneration();
    if (ctx.cachedMetricsGeneration != generation) {
        ctx.fontMetricsCache.clear();
        ctx.cachedMetricsGeneration = generation;
    }
    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);
    UITextSizeSystem(canvases, ctx, width, height);
    UILayoutSystem(canvases, ctx, width, height);
}
void UpdateUIInteraction(Scene& scene, UIInteractionContext& ctx, const UIInputView& view, bool pressed)
{
    if (!std::isfinite(view.width) || !std::isfinite(view.height) || view.width <= 0.0f || view.height <= 0.0f) return;
    const auto generation = scene.GetRenderSceneGeneration();
    if (ctx.sceneGeneration != generation) {
        ctx.sceneGeneration = generation;
        ctx.lastInputFrame = ~std::uint64_t{0};
        ctx.navHeldDirection = {};
        ctx.navRepeatTimer = 0.0f;
        ctx.inputInteractionRevision = ~std::uint64_t{0};
        ctx.dragFrame = {};
    }
    ctx.sceneDepthAvailable = view.sceneDepthAvailable;
    if (!view.sceneDepthAvailable) {
        std::vector<CanvasEntry> canvases;
        CollectCanvases(scene, canvases);
        for (const auto& entry : canvases)
            if (!ShouldRenderCanvas(*entry.canvas, UIRenderTargetView::GameViewport, false))
                CancelUIInteractionsRecursive(*entry.go);
    }
    if (ctx.lastInputFrame == Time::frameCount) return;
    ctx.lastInputFrame = Time::frameCount;
    ctx.gameplayInput = view.gameplayInput;
    UpdateUILayout(scene, ctx, view.width, view.height);
    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);
    UIEventSystem(scene, canvases, ctx, view.width, view.height, view.pointer, pressed,
        view.viewProjection, view.cameraPosition, view.cameraRotation, UIRenderTargetView::GameViewport);
}
}
