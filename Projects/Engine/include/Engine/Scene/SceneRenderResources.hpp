/// @file    SceneRenderResources.hpp
/// @brief   シーンのコンポーネントが個体ごとに確保した GPU リソースの一括返却。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once

#include <Engine/Scene/Entity.hpp>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class Scene;

/// コンポーネントが «その個体専用» に確保した GPU リソースを ResourceManager へ返す。
///
/// WHY 要るか:
///   Scene を畳んでもコンポーネントが持っていた ResourceHandle の実体は ResourceManager 側に残る。
///   Editor の Play → Stop はシーンを丸ごと読み直すため、繰り返すたびに前のシーンぶんの
///   定数バッファ・頂点バッファ・キューブマップが積み上がっていた。
/// NOTE: パスキャッシュ由来の共有リソース (LoadTexture / LoadShader の結果) は返さない。
///       同じハンドルを他のシーンや Editor UI が使い続けている。
void ReleaseSceneOwnedGpuResources(Scene& scene, renderer::ResourceManager& resources);

/// 1 エンティティぶんだけ同じ返却を行う。GameObject を畳む直前に呼ぶ。
void ReleaseEntityOwnedGpuResources(Scene& scene, EntityID id, renderer::ResourceManager& resources);

/// 複製直後のエンティティから «コピー元と同じハンドル» を捨てる。
///
/// WHY 要るか: Component 配列の複製は値コピーなので、GPU ハンドルまで写る。
///     そのままだと複製元と複製先が 1 本のバッファを共有し、毎フレーム互いのポーズを
///     上書きし合ううえ、片方を畳んだ瞬間にもう片方の参照先が消える。
///     所有者は複製元なので、ここでは «返さずに手放す» のが正しい。
void ClearDuplicatedGpuHandles(Scene& scene, EntityID id);

/// @name コンポーネント 1 つぶんの返却
///
/// WHY 型ごとのオーバーロードにするか:
///   Scene::RemoveComponent<T>() は «どの型でも» 呼ばれるテンプレートで、
///   GPU リソースを持たない型が大半。持つ型だけ非テンプレートの宣言を用意し、
///   それ以外はテンプレートの何もしない版へ落とす — 呼び出し側は型を意識しない。
///   ResourceManager を引数に取らないのは、Scene.hpp へ Renderer の実体を持ち込まないため
///   (返却先は ResourceManager::Active() を使う。無ければ何もしない)。
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

void ReleaseComponentGpuResources(AnimatorComponent& component);
void ReleaseComponentGpuResources(SkinnedMeshRenderer& component);
void ReleaseComponentGpuResources(TrailComponent& component);
void ReleaseComponentGpuResources(MeshTrailComponent& component);
void ReleaseComponentGpuResources(ParticleEmitter& component);
void ReleaseComponentGpuResources(ReflectionProbeComponent& component);
void ReleaseComponentGpuResources(SpriteRendererComponent& component);
void ReleaseComponentGpuResources(LineRendererComponent& component);
void ReleaseComponentGpuResources(ProceduralMeshComponent& component);

/// 上のどれにも当たらない型は GPU リソースを持たない。
template <class T>
inline void ReleaseComponentGpuResources(T&) {}
///@}

/// @name 値としてコピーした Component からハンドルだけ消す (返さない)
///
/// WHY 要るか: Undo は «外す前の Component» を値でコピーして持つ。そのコピーには
///     コピー元と同じ GPU ハンドルが入っているが、実体は外した時点で返却済みで、
///     同じ枠が別のリソースに再利用され得る。やり直しで «他人のバッファを掴んだ
///     Component» が復活しないよう、コピー側のハンドルは空にしておく。
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

template <class T>
inline void ClearComponentGpuHandles(T&) {}
///@}

} // namespace fbzz::scene
