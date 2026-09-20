/// @file    SceneAssetReferences.hpp
/// @brief   シーンファイルの本文から、先読みすべきアセット参照を拾う。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

enum class SceneAssetKind : unsigned char { Texture, Model, Material };

struct SceneAssetReference {
    SceneAssetKind kind = SceneAssetKind::Texture;
    /// Load 系へそのまま渡せる参照 (guid 参照・Assets/ 起点パス)。Sprite と submesh の接尾辞は落としてある。
    std::string    reference;
    bool operator==(const SceneAssetReference&) const = default;
};

/// @brief 種類に対応する AssetStreamer の型名 (AssetStreamTypeName の値)。
[[nodiscard]] std::string_view StreamTypeNameOf(SceneAssetKind kind);

/// @brief 参照 1 つを種類付きへ直す。拡張子で種類が分からなければ nullopt。
/// @param pathOfGuid ヒントの無い guid 参照の実パスを引く関数 (null なら種類不明として扱う)。
[[nodiscard]] std::optional<SceneAssetReference> ClassifySceneAssetReference(
    std::string_view value,
    const std::function<std::string(std::string_view guidRef)>& pathOfGuid = {});

/// @brief TOML 本文の文字列値を走査し、拡張子で種類が分かる参照を重複なく集める。
/// @param pathOfGuid ヒントの無い guid 参照の実パスを引く関数 (null なら種類不明として捨てる)。
/// @note 本文を Scene へ組み立てずに拾うのは、先読みを «ロードの前» に始めるため。
///       取りこぼしても正しさは変わらない (その分が同期ロードになるだけ) ので、構造は解釈しない。
[[nodiscard]] std::vector<SceneAssetReference> CollectSceneAssetReferences(
    std::string_view sceneText,
    const std::function<std::string(std::string_view guidRef)>& pathOfGuid = {});

} // namespace fbzz::scene
