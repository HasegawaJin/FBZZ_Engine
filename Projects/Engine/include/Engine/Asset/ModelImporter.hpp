// FBZZ Engine
// ModelImporter.hpp | fbzz::asset
// Assimp を使って FBX / OBJ 等を Model に変換する
#pragma once
#include <memory>
#include <string>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::asset    { struct Model; }

namespace fbzz::asset {

class ModelImporter {
public:
    // 失敗時は nullptr を返す
    static std::shared_ptr<Model> Import(
        const std::string& path,
        renderer::ResourceManager& resources);
};

} // namespace fbzz::asset
