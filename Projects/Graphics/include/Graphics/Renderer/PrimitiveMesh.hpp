/// @file    PrimitiveMesh.hpp
/// @brief   手続き生成メッシュのファクトリ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note Cube / Sphere / Plane などを ResourceManager 経由で GPU リソース化する。
/// @note テスト用・デバッグ用の基本形状をアセット読み込みなしで作る。
#pragma once
#include <Graphics/Api.hpp>
#include "Mesh.hpp"

namespace fbzz::renderer {

class ResourceManager;

/// @note PrimitiveMesh は AssetManager 管轄外の手続き生成メッシュなので、static キャッシュで
/// @note       所有し raw pointer を返す。呼び出し元はポインタをキャッシュしてよい。
class PrimitiveMesh {
public:
    FBZZ_GRAPHICS_API static Mesh* Cube    (ResourceManager& resources);
    FBZZ_GRAPHICS_API static Mesh* Sphere  (ResourceManager& resources, int segments = 16);
    FBZZ_GRAPHICS_API static Mesh* Plane   (ResourceManager& resources);
    FBZZ_GRAPHICS_API static Mesh* Quad    (ResourceManager& resources);
    FBZZ_GRAPHICS_API static Mesh* Cylinder(ResourceManager& resources, int segments = 16);
    FBZZ_GRAPHICS_API static Mesh* Cone    (ResourceManager& resources, int segments = 16);
    FBZZ_GRAPHICS_API static Mesh* Torus   (ResourceManager& resources, int segments = 24);
    FBZZ_GRAPHICS_API static Mesh* Capsule (ResourceManager& resources, int segments = 24);
};

} /// @note namespace fbzz::renderer
