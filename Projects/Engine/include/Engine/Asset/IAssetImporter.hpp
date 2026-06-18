// FBZZ Engine
// IAssetImporter.hpp | fbzz::asset
// ランタイムアセットローダーの基底インターフェース
// AssetManager が内部で使う。型ごとに SupportedExtensions() を自己申告し、
// Load<T> の特殊化がディスパッチする。
#pragma once
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::asset {

template<typename T>
class IAssetImporter {
public:
    virtual ~IAssetImporter() = default;

    // absPath: 絶対ファイルパス。失敗時は nullptr を返す。例外は使わない。
    // resources: GPU アップロードが必要な型のみ使用（不要な型では nullptr も可）
    [[nodiscard]] virtual std::unique_ptr<T> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) = 0;

    // このインポーターが処理できる拡張子（ドット付き小文字、例: ".model"）
    virtual std::span<const std::string_view> SupportedExtensions() const = 0;
};

} // namespace fbzz::asset
