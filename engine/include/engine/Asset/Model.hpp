// FBZZ Engine
// Model.hpp | fbzz::asset
// FBX 等のファイルロード単位。メッシュ群とマテリアル群を保持する
#pragma once
#include <vector>
#include <memory>

namespace fbzz::renderer { struct Mesh; class Material; }

namespace fbzz::asset {

struct Model {
    std::vector<std::shared_ptr<renderer::Mesh>>     meshes;
    std::vector<std::shared_ptr<renderer::Material>> materials;  // meshes[i] と 1:1
};

} // namespace fbzz::asset
