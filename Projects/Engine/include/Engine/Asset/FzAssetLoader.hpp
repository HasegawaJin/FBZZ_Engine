// FBZZ Engine
// FzAssetLoader.hpp | fbzz::asset
// .fzasset (type="model") マニフェストから Model を生成するローダー
// fz* バイナリを直接デシリアライズし、Assimp 不要でランタイムロードを完結させる。
#pragma once
#include <memory>
#include <string>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::asset    { struct Model; }

namespace fbzz::asset {

class FzAssetLoader {
public:
    // fzassetPath : .fzasset ファイルの絶対パス
    // 失敗時は nullptr を返す。
    static std::shared_ptr<Model> Load(
        const std::string& fzassetPath,
        renderer::ResourceManager& resources);
};

} // namespace fbzz::asset
