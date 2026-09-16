/// @file    AnimationPreview.hpp
/// @brief   Animation アセット用プレビュー API。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once

#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <string>
#include <string_view>

namespace fbzz::editor {

struct EditorContext;

// Inspector 埋め込みと Preview パネルで共有する Animation プレビュー。
bool DrawAnimationPreviewWidget(EditorContext& ctx, float previewHeight);
bool HasAnimationPreviewTarget();
// Preview ウィンドウが閉じていても、選択対象と再生時刻を保持・更新する。
// WHY: Unity の Inspector Preview はパネルの表示状態に依存せず対象を保持するため、
//      再表示や Inspector への再接続なしで同じクリップを続きから確認できるようにする。
void TickAnimationPreview(EditorContext& ctx);
// Editor 終了時に Preview のセッション状態と GPU ハンドルを破棄する。
void ShutdownAnimationPreview();

// 選択 FBX のメッシュを Avatar Mask のウェイトで色分けする専用プレビュー。
// Inspector 埋め込みではなく独立パネルからも同じ描画経路を利用する。
bool DrawAnimationMaskPreviewWidget(EditorContext& ctx,
                                    std::string_view modelPath,
                                    std::string_view maskPath,
                                    float previewHeight,
                                    const asset::AvatarMaskAsset* maskOverride = nullptr);

// Inspector と Preview 間で、現在フォーカスしている Mask ノードのパスを共有する。
void SetAnimationMaskPreviewSelection(std::string_view nodePath);
bool ConsumeAnimationMaskPreviewSelection(std::string& nodePath);
void ClearAnimationMaskPreviewSelection();

// Animation Mask Preview Panel の対象を外部から差し替える。
// WHY: Animation Graph のレイヤー一覧から「このレイヤーのマスクを 3D で見る」へ
//      直行させるため。Asset Browser で .mask を選び直す遠回りを挟むと、
//      どのレイヤーの話をしていたのかが途中で分からなくなる。
// modelPath が空なら .mask の Skeleton Source をそのまま使う。
void RequestAnimationMaskPreview(std::string_view maskPath, std::string_view modelPath = {});

// Animation Mask Preview Panel の本文を描画する。
void DrawAnimationMaskPreviewPanelContent(EditorContext& ctx);

} // namespace fbzz::editor
