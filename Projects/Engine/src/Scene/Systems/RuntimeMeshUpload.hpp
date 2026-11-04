/// @file    RuntimeMeshUpload.hpp
/// @brief   個体専用メッシュの所有者検証と二枚バッファへの転送。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <span>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::scene {
class Scene;
struct DoubleBufferedMesh;
/// @return 所有者変更・Reset・個別解放で GPU 状態を破棄した場合 true。借用 MeshRenderer を先に切り離す。
bool PrepareRuntimeMesh(Scene& scene, EntityID id, DoubleBufferedMesh& target, renderer::ResourceManager& resources);
/// @note 成功時だけ描画スロットを切り替える。失敗時は旧描画スロットを保ち、呼出し側は dirty を消さず再試行する。
bool UploadRuntimeMesh(DoubleBufferedMesh& target, renderer::ResourceManager& resources,
    std::span<const renderer::Vertex> vertices, std::span<const uint32_t> indices, MeshDirty dirty = MeshDirty::All);
} /// @note namespace fbzz::scene
