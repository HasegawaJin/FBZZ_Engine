/// @file    AssetPathService.hpp
/// @brief   Renderer 層がアセットのパス表記を実ファイルへ解決するための差し込み口。
/// @author  Hasegawa Jin
/// @date    2026-09-03
#pragma once
#include <string>
#include <string_view>

namespace fbzz::renderer {

/// Asset 層が提供する解決規則。
///
/// "guid:" 参照・".meta" サイドカー・Sprite サブアセット参照の解釈は Asset 層が持つ。
/// Renderer から直接呼ぶとバックエンドを下位ライブラリへ切り出せなくなるため、
/// 規則の形だけをここに置き、実体は Asset 層が SetAssetPathService で差し込む。
/// null のメンバーは入力をそのまま返す素通しの既定実装になる。
struct AssetPathService {
    /// "Assets/..." や "guid:..." をプロジェクト内の実パスへ解決する。
    std::string (*resolveAssetPath)(const std::string& path) = nullptr;

    /// ".meta" サイドカー表記なら元画像パスを返す。生画像パスはそのまま返す。
    bool (*resolveTextureSource)(std::string_view texturePath, std::string& outSourcePath) = nullptr;

    /// Sprite サブアセット参照から親テクスチャのパスだけを取り出す。
    /// GPU 上は親を共有するため、キャッシュキーはここまで正規化する。
    void (*normalizeTextureKey)(std::string_view reference, std::string& outTexturePath) = nullptr;
};

/// 解決規則を差し込む。Asset 層が起動時に一度だけ呼ぶ。
void SetAssetPathService(const AssetPathService& service);

std::string ResolveAssetPath(const std::string& path);
[[nodiscard]] bool ResolveTextureSource(std::string_view texturePath, std::string& outSourcePath);
[[nodiscard]] std::string NormalizeTextureKey(std::string_view reference);

} // namespace fbzz::renderer
