/// @file    SceneRenderResources.hpp
/// @brief   シーンのコンポーネントが個体ごとに確保した GPU リソースの一括返却。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once

#include <Engine/Scene/Entity.hpp>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class Scene;

/// @brief コンポーネントが «その個体専用» に確保した GPU リソースを ResourceManager へ返す。
/// @note Editor の Play→Stop はシーンを丸ごと読み直すため、返さないと前のシーンぶんの定数バッファ・頂点バッファ・キューブマップが積み上がる。
/// @note パスキャッシュ由来の共有リソース (LoadTexture/LoadShader の結果) は返さない。他のシーンや Editor UI が同じハンドルを使い続けている。
void ReleaseSceneOwnedGpuResources(Scene& scene, renderer::ResourceManager& resources);

/// @brief 1 エンティティぶんだけ同じ返却を行う。GameObject を畳む直前に呼ぶ。
void ReleaseEntityOwnedGpuResources(Scene& scene, EntityID id, renderer::ResourceManager& resources);

/// @brief 複製直後のエンティティから «コピー元と同じハンドル» を捨てる。
/// @note Component 配列の複製は値コピーで GPU ハンドルも写る。返すと複製元と 1 本のバッファを共有してしまうため、所有者でない複製先は返さず手放す。
void ClearDuplicatedGpuHandles(Scene& scene, EntityID id);

/// @name コンポーネント 1 つぶんの返却
/// @note GPU リソースを持つ型だけ非テンプレートの宣言を用意し、それ以外はテンプレートの空実装へ落とす。呼び出し側は型を意識しない。
/// @note ResourceManager を引数に取らないのは Scene.hpp へ Renderer 実体を持ち込まないため。返却先は ResourceManager::Active() を使い、無ければ何もしない。
///@{
struct AnimatorComponent;
struct SkinnedMeshRenderer;
struct TrailComponent;
struct MeshTrailComponent;
struct ParticleEmitter;
struct ReflectionProbeComponent;
struct SpriteRendererComponent;
struct LineRendererComponent;
struct ProceduralMeshComponent;
struct ClothComponent;

void ReleaseComponentGpuResources(AnimatorComponent& component);
void ReleaseComponentGpuResources(SkinnedMeshRenderer& component);
void ReleaseComponentGpuResources(TrailComponent& component);
void ReleaseComponentGpuResources(MeshTrailComponent& component);
void ReleaseComponentGpuResources(ParticleEmitter& component);
void ReleaseComponentGpuResources(ReflectionProbeComponent& component);
void ReleaseComponentGpuResources(SpriteRendererComponent& component);
void ReleaseComponentGpuResources(LineRendererComponent& component);
void ReleaseComponentGpuResources(ProceduralMeshComponent& component);
void ReleaseComponentGpuResources(ClothComponent& component);

/// @brief 上のどれにも当たらない型は GPU リソースを持たない。
template <class T>
inline void ReleaseComponentGpuResources(T&) {}
///@}

/// @name 値としてコピーした Component からハンドルだけ消す (返さない)
/// @note Undo は外す前の Component を値でコピーして持つ。実体は外した時点で返却済みで同じ枠が再利用され得るため、コピー側のハンドルは空にして «他人のバッファを掴んだ Component» の復活を防ぐ。
///@{
void ClearComponentGpuHandles(AnimatorComponent& component);
void ClearComponentGpuHandles(SkinnedMeshRenderer& component);
void ClearComponentGpuHandles(TrailComponent& component);
void ClearComponentGpuHandles(MeshTrailComponent& component);
void ClearComponentGpuHandles(ParticleEmitter& component);
void ClearComponentGpuHandles(ReflectionProbeComponent& component);
void ClearComponentGpuHandles(SpriteRendererComponent& component);
void ClearComponentGpuHandles(LineRendererComponent& component);
void ClearComponentGpuHandles(ProceduralMeshComponent& component);
void ClearComponentGpuHandles(ClothComponent& component);

template <class T>
inline void ClearComponentGpuHandles(T&) {}
///@}

} // namespace fbzz::scene
