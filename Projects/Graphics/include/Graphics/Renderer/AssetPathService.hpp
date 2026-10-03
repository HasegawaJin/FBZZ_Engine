/// @file    AssetPathService.hpp
/// @brief   Renderer 層がアセットのパス表記を実ファイルへ解決するための差し込み口。
/// @author  Hasegawa Jin
/// @date    2026-09-03
#pragma once
#include <Graphics/Renderer/ShaderCapabilities.hpp>
#include <string>
#include <string_view>

namespace fbzz::renderer {

/// @brief Asset 層が差し込むパス解決規則。
/// @note "guid:" 参照や ".meta" サイドカーの解釈は Asset 層が持ち、Renderer は規則の形だけを
/// @note       持つ (直接呼ぶとバックエンドを下位ライブラリへ切り出せなくなるため)。null のメンバーは
/// @note       パスを素通しにし、能力は未知として扱う既定実装。
struct AssetPathService {
    /// @brief "Assets/..." や "guid:..." をプロジェクト内の実パスへ解決する。
    std::string (*resolveAssetPath)(const std::string& path) = nullptr;

    /// @brief ".meta" サイドカー表記なら元画像パスを返す。生画像パスはそのまま返す。
    bool (*resolveTextureSource)(std::string_view texturePath, std::string& outSourcePath) = nullptr;

    /// @brief Sprite サブアセット参照から親テクスチャのパスだけを取り出す。
    /// @note GPU 上は親を共有するため、キャッシュキーはここまで正規化する。
    void (*normalizeTextureKey)(std::string_view reference, std::string& outTexturePath) = nullptr;

    /// @note Shader の .meta 解釈は Asset 層に閉じる。未登録時は未知の能力として扱う。
    ShaderCapabilities (*resolveShaderCapabilities)(std::string_view reference) = nullptr;
};

/// @brief 解決規則を差し込む。
/// @note Asset 層が起動時に一度だけ呼ぶ。
void SetAssetPathService(const AssetPathService& service);
[[nodiscard]] ShaderCapabilities ResolveShaderCapabilities(std::string_view reference);

std::string ResolveAssetPath(const std::string& path);
[[nodiscard]] bool ResolveTextureSource(std::string_view texturePath, std::string& outSourcePath);
[[nodiscard]] std::string NormalizeTextureKey(std::string_view reference);

} /// @note namespace fbzz::renderer
