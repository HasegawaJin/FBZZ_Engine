/// @file    SceneAssetReferences.cpp
/// @brief   シーン本文からアセット参照を拾う。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <Engine/Scene/SceneAssetReferences.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/TextureAsset.hpp>

#include <algorithm>
#include <cctype>
#include <optional>

namespace fbzz::scene {

namespace {

std::string ToLower(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

std::optional<SceneAssetKind> KindOfPath(std::string_view path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return std::nullopt;
    const std::string ext = ToLower(path.substr(dot));
    static constexpr std::string_view kTextures[] = {
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp", ".hdr", ".exr",
    };
    for (const std::string_view candidate : kTextures)
        if (ext == candidate) return SceneAssetKind::Texture;
    if (ext == ".fbx" || ext == ".fzasset") return SceneAssetKind::Model;
    if (ext == ".mat") return SceneAssetKind::Material;
    return std::nullopt;
}

/// @brief MeshRenderer の "models/foo.fbx:0" の submesh 添字を落とす。"C:/..." のドライブ区切りは残す。
std::string_view StripSubmeshSuffix(std::string_view value)
{
    const std::size_t colon = value.find_last_of(':');
    const std::size_t dot = value.find_last_of('.');
    if (colon == std::string_view::npos || dot == std::string_view::npos || colon < dot) return value;
    const std::string_view digits = value.substr(colon + 1);
    if (digits.empty()) return value;
    for (const char c : digits)
        if (!std::isdigit(static_cast<unsigned char>(c))) return value;
    return value.substr(0, colon);
}

} // namespace

std::string_view StreamTypeNameOf(SceneAssetKind kind)
{
    switch (kind) {
    case SceneAssetKind::Texture:  return asset::AssetStreamTypeName<asset::TextureAsset>();
    case SceneAssetKind::Model:    return asset::AssetStreamTypeName<asset::Model>();
    case SceneAssetKind::Material: return asset::AssetStreamTypeName<asset::MaterialAsset>();
    }
    return {};
}

std::optional<SceneAssetReference> ClassifySceneAssetReference(
    std::string_view value,
    const std::function<std::string(std::string_view)>& pathOfGuid)
{
    if (value.empty()) return std::nullopt;
    /// @note Sprite 参照は親テクスチャを先読みする。
    if (const std::size_t sprite = value.find("::sprite::"); sprite != std::string_view::npos)
        value = value.substr(0, sprite);

    std::optional<SceneAssetKind> kind;
    std::string reference;
    if (value.rfind("guid:", 0) == 0) {
        /// @note guid 参照は `guid:<hex>|<最後に知っていたパス>`。種類はヒントの拡張子で決め、
        ///       参照自体は guid のまま渡す (移動・改名に追従させる)。
        const std::size_t bar = value.find('|');
        const std::string_view hint = bar != std::string_view::npos ? value.substr(bar + 1) : std::string_view{};
        const std::string_view guidPart = bar != std::string_view::npos ? value.substr(0, bar) : value;
        const std::string_view stripped = StripSubmeshSuffix(hint);
        if (!stripped.empty()) kind = KindOfPath(stripped);
        else if (pathOfGuid) kind = KindOfPath(pathOfGuid(guidPart));
        reference = std::string(guidPart);
        if (!stripped.empty()) reference += "|" + std::string(stripped);
    } else {
        const std::string_view stripped = StripSubmeshSuffix(value);
        kind = KindOfPath(stripped);
        reference = std::string(stripped);
    }
    if (!kind) return std::nullopt;
    return SceneAssetReference{ *kind, std::move(reference) };
}

std::vector<SceneAssetReference> CollectSceneAssetReferences(
    std::string_view sceneText,
    const std::function<std::string(std::string_view)>& pathOfGuid)
{
    std::vector<SceneAssetReference> references;
    std::size_t i = 0;
    while (i < sceneText.size()) {
        const char quote = sceneText[i];
        if (quote == '#') {
            /// @note コメント行の中身は拾わない。
            const std::size_t end = sceneText.find('\n', i);
            i = end == std::string_view::npos ? sceneText.size() : end + 1;
            continue;
        }
        if (quote != '"' && quote != '\'') { ++i; continue; }
        std::string value;
        std::size_t j = i + 1;
        for (; j < sceneText.size() && sceneText[j] != quote && sceneText[j] != '\n'; ++j) {
            /// @note 基本文字列の \" と \\ だけを戻す。パスに他のエスケープは現れない。
            if (quote == '"' && sceneText[j] == '\\' && j + 1 < sceneText.size()) ++j;
            value.push_back(sceneText[j]);
        }
        if (auto found = ClassifySceneAssetReference(value, pathOfGuid);
            found && std::find(references.begin(), references.end(), *found) == references.end())
            references.push_back(std::move(*found));
        i = j + 1;
    }
    return references;
}

} // namespace fbzz::scene
