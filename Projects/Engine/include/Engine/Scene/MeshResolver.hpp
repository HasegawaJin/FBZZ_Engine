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

/// メッシュ参照文字列を解決する。見つからなければ nullptr。
///
///   "primitive:sphere" → PrimitiveMesh::Sphere
///   "models/foo.fbx"   → AssetManager::LoadModel の mesh[0]
///   "models/foo.fbx:2" → mesh[2]
///
/// 返すのは生ポインタ。primitive は PrimitiveMesh の static キャッシュ、モデルは
/// AssetManager がそれぞれ所有し続けるので、呼び出し側は解放しない。
///
/// WHY 独立した関数か: 以前は SceneSerializer.cpp の無名名前空間にあり、シーン読み込み時に
///      しか呼べなかった。そのため実行中に meshPath を書き換えても «文字列だけ変わって
///      形は変わらない» 状態になっていた (ScriptMeshProxy::SetMeshPath)。
renderer::Mesh* ResolveMeshPath(const std::string& path, renderer::ResourceManager& resources);

} // namespace fbzz::scene
