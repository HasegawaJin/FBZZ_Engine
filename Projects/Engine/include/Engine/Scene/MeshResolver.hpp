/// @file    MeshResolver.hpp
/// @brief   メッシュ参照文字列から renderer::Mesh を引く共通処理
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <string>

namespace fbzz::renderer {
struct Mesh;
class ResourceManager;
} // namespace fbzz::renderer

namespace fbzz::scene {

/// @brief メッシュ参照文字列を解決する。見つからなければ nullptr。
/// @note 形式: `"primitive:sphere"` → PrimitiveMesh::Sphere / `"models/foo.fbx"` → `AssetManager::Load<Model>` の mesh[0] / `"models/foo.fbx:2"` → mesh[2]
/// @note 返すのは生ポインタ。primitive は PrimitiveMesh の static キャッシュ、モデルは AssetManager が所有し続けるので呼び出し側は解放しない。
/// @note SceneSerializer.cpp の無名名前空間から独立させた。以前はシーン読み込み時にしか解決できず、実行中に meshPath を書き換えても見た目が変わらなかった (ScriptMeshProxy::SetMeshPath)。
renderer::Mesh* ResolveMeshPath(const std::string& path, renderer::ResourceManager& resources);

} // namespace fbzz::scene
