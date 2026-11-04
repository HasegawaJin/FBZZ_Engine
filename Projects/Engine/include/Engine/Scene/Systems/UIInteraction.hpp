/// @file    UIInteraction.hpp
/// @brief   描画資源を必要としない UI の計測・レイアウト・入力更新。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Api.hpp>
#include <Graphics/Renderer/FontAtlas.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
namespace fbzz::scene {
class Scene; class GameObject; struct UICanvas; struct UINavigation;
/// @note 描画と同じ規則で求める Canvas 全体の寸法。safeArea を差し引く前の Canvas 単位。
[[nodiscard]] FBZZ_ENGINE_API math::Vector2 GetCanvasRectSize(const UICanvas& canvas,
    float viewportWidth, float viewportHeight);

enum class UIRenderTargetView {
    GameViewport,  ///< @note 実ゲーム出力。ScreenSpace / WorldSpace の両方を最終合成として描く。
    SceneViewport, ///< @note 3D 編集ビュー。シーン内オブジェクトである WorldSpace Canvas だけ描く。
    CanvasEditor   ///< @note UI 専用編集ビュー。3D 空間に属する WorldSpace Canvas は描かない。
};

/// @note テキスト 1 行ぶんの分割結果。折り返しを入れると「どこで切れたか」を
/// @note 測るときと描くときで共有しないと、行数と行幅が食い違う。
struct UITextLine {
    std::size_t begin = 0;   ///< @note text.text へのバイトオフセット (この行の先頭)
    std::size_t end   = 0;   ///< @note 同 (この行の終端。改行文字は含まない)
    float       width = 0.0f;
    /// @note この行の送り高さ。リッチテキストで `<size>` を使うと行ごとに変わる。
    float       height = 0.0f;
    /// @note この行の末尾を "…" に置き換えて描く (TextOverflow::Ellipsis)。
    bool        ellipsis = false;
};
struct UIInputView {
    float width = 0.0f, height = 0.0f;
    math::Vector2 pointer;
    math::Matrix4 viewProjection = math::Matrix4::Identity();
    math::Vector3 cameraPosition;
    math::Quaternion cameraRotation;
    bool gameplayInput = true;
    bool sceneDepthAvailable = true;
};

/// @note 描画・GPU・イベント更新を待たず、現在のポインターと表示用の UI 矩形から判定する。
/// @note マスク・ScrollView・CanvasGroup を反映し、クリック通知やフォーカスを変更しない。
[[nodiscard]] FBZZ_ENGINE_API bool UIPointerOverUI(Scene& scene, const UIInputView& view);

/// @note リッチテキストのタグを除いた文字数。visibleCharacters の上限を知るのに使う。
/// @note richText が false なら単純な UTF-8 の文字数。
[[nodiscard]] FBZZ_ENGINE_API std::size_t UITextVisibleLength(const std::string& text, bool richText);

/// @note GPU 資源を保持しない。GameViewport のホストが Scene ごとに所有する。
struct UIInteractionContext {
    bool sceneDepthAvailable = true;
    bool gameplayInput = false;
    std::string defaultFontPath = "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght";
    float textPixelScale = 1.0f;
    std::deque<std::vector<GameObject*>> childScratch;
    std::vector<int> layoutIndexScratch;
    std::vector<math::Vector2> layoutSizeScratch;
    std::vector<UITextLine> textLineScratch;
    struct NavigationCandidate {
        GameObject* go = nullptr;
        UINavigation* nav = nullptr;
        math::Vector2 rectPosition, rectSize;
    };
    /// @note フォーカス候補は Context の作業領域を使い回し、Scene 間で大域共有しない。
    std::vector<NavigationCandidate> navigationScratch;
    /// @note LoadMetrics 専用。テクスチャのない静的フォントと動的フォントを同じ計測規則で読む。
    std::unordered_map<std::string, renderer::FontAtlas> fontMetricsCache;
    int cachedMetricsGeneration = -1;
    std::uint64_t inputInteractionRevision = ~std::uint64_t{0};
    std::uint64_t sceneGeneration = 0, lastInputFrame = ~std::uint64_t{0};
    math::Vector2 navHeldDirection{};
    float navRepeatTimer = 0.0f;
    struct DragFrame { GameObject* hoveredTarget = nullptr; GameObject* releasedSource = nullptr; GameObject* activeSource = nullptr; } dragFrame;
};
/// @note フォント計測とレイアウトだけを更新し、クリックとフォーカスは変更しない。
FBZZ_ENGINE_API void UpdateUILayout(Scene& scene, UIInteractionContext& context, float width, float height);
/// @note Script 更新後に呼ぶ。通知はフレーム N の結果を Update N+1 で読む。入力は Scene・フレームにつき1回。
FBZZ_ENGINE_API void UpdateUIInteraction(Scene& scene, UIInteractionContext& context, const UIInputView& view, bool pressed);
}
