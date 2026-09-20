/// @file    PrimitiveMesh.hpp
/// @brief   手続き生成メッシュのファクトリ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note Cube / Sphere / Plane などを ResourceManager 経由で GPU リソース化する。
/// @note テスト用・デバッグ用の基本形状をアセット読み込みなしで作る。
#pragma once
#include "Mesh.hpp"

namespace fbzz::renderer {

class ResourceManager;

/// @note PrimitiveMesh は AssetManager 管轄外の手続き生成メッシュなので、static キャッシュで
/// @note       所有し raw pointer を返す。呼び出し元はポインタをキャッシュしてよい。
class PrimitiveMesh {
public:
    static Mesh* Cube    (ResourceManager& resources);
    static Mesh* Sphere  (ResourceManager& resources, int segments = 16);
    static Mesh* Plane   (ResourceManager& resources);
    static Mesh* Quad    (ResourceManager& resources);
    static Mesh* Cylinder(ResourceManager& resources, int segments = 16);
    static Mesh* Cone    (ResourceManager& resources, int segments = 16);
    static Mesh* Torus   (ResourceManager& resources, int segments = 24);
    static Mesh* Capsule (ResourceManager& resources, int segments = 24);
};

} /// @note namespace fbzz::renderer
